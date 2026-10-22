// dc-v5-probe: P1/V5 verification of the remote controller's weight. A minimal Qt Quick mixer
// (fader strips with meters) that reports its own memory, CPU and frame rate, and checks that
// several faders can be dragged at the same time with touch. See docs/verification/v5-remote-lite.md.
//
//   dc-v5-probe [--ui custom|controls|minimal] [--tracks N] [--connect HOST[:PORT]]
//               [--backend default|d3d11|software|opengl] [--fullscreen]
//               [--touch-test] [--duration SEC] [--report FILE]

#include "probe.hpp"
#include "strip_model.hpp"
#include "touch_selftest.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/protocol/schema.hpp"
#include "dc/version.hpp"

#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

Probe* g_probe = nullptr;
QtMessageHandler g_previousHandler = nullptr;

// Counts QML warnings (binding errors, missing types) so a broken probe cannot pass silently.
void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (g_probe && type >= QtWarningMsg && (msg.contains(QLatin1String("qml")) || msg.contains(QLatin1String(".qml"))))
        g_probe->countQmlWarning();
    if (g_previousHandler) g_previousHandler(type, ctx, msg);
}

bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], name) == 0) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // Before the application object: lets Qt register the injected touch device at start-up.
    const bool touchTest = hasArg(argc, argv, "--touch-test");
    const bool injectionReady = touchTest && TouchSelfTest::prepare();

#ifdef _WIN32
    // Qt 6.10's DXGI VBlank thread never wakes on some setups (seen with a virtual monitor on
    // Reo's PC: "Unable to open monitor interface"), which freezes D3D11 after the first frame.
    if (qEnvironmentVariableIsEmpty("QT_D3D_NO_VBLANK_THREAD")) qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("dc-v5-probe"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(dc::kVersionString));

    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        {"ui", "custom (lightweight QtQuick items), controls (QtQuick.Controls Basic) or minimal (empty window).", "name", "custom"},
        {"tracks", "Number of strips (1-64).", "n", "16"},
        {"connect", "Mirror a control server (mixd or dc-mock-server) instead of simulating.", "host[:port]"},
        {"backend", "Scene graph backend: default, d3d11, software or opengl.", "name", "default"},
        {"fullscreen", "Show full screen (as on the remote device)."},
        {"touch-test", "Run the injected multi-touch self test, then exit."},
        {"duration", "Write the report and exit after this many seconds.", "sec", "0"},
        {"report", "Write a JSON report to this file on exit.", "file"},
    });
    cli.process(app);

    const QString ui = cli.value("ui");
    const int tracks = std::clamp(cli.value("tracks").toInt(), 1, dc::proto::kMaxTracks);
    const QString backend = cli.value("backend");
    if (backend == QLatin1String("software")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (backend == QLatin1String("d3d11")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    else if (backend == QLatin1String("opengl")) QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    Probe probe(QStringLiteral("Qt %1 · %2 · %3 tracks").arg(QString::fromLatin1(qVersion()), ui).arg(tracks));
    g_probe = &probe;
    g_previousHandler = qInstallMessageHandler(messageHandler);

    StripModel model;
    QString connectError;
    if (cli.isSet("connect")) {
        const QStringList hp = cli.value("connect").split(':');
        const quint16 port = hp.size() > 1 ? static_cast<quint16>(hp[1].toUInt()) : dc::proto::kDefaultControlPort;
        if (!model.connectTo(hp[0], port, tracks, &connectError)) {
            std::fprintf(stderr, "cannot connect: %s\n", qPrintable(connectError));
            return 1;
        }
    } else {
        model.simulate(tracks);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("strips"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
    engine.rootContext()->setContextProperty(QStringLiteral("startFullScreen"), cli.isSet("fullscreen"));
    const QString file = ui == QLatin1String("controls") ? QStringLiteral("MainControls.qml")
                         : ui == QLatin1String("minimal") ? QStringLiteral("Minimal.qml")
                                                          : QStringLiteral("Main.qml");
    engine.load(QUrl(QStringLiteral("qrc:/DcProbe/qml/") + file));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) return 1;
    probe.attach(window);

    QJsonObject touchResult;
    const QString reportPath = cli.value("report");
    auto finish = [&](int code) {
        QJsonObject report = probe.toJson();
        report.insert("version", QString::fromLatin1(dc::kVersionString));
        report.insert("qt", QString::fromLatin1(qVersion()));
        report.insert("ui", ui);
        report.insert("tracks", model.count());
        report.insert("connected", model.connected());
        report.insert("graphics_api", static_cast<int>(window->rendererInterface()->graphicsApi()));
        report.insert("device_pixel_ratio", window->devicePixelRatio());
        report.insert("window", QStringLiteral("%1x%2").arg(window->width()).arg(window->height()));
        if (touchTest) {
            if (!injectionReady) touchResult.insert("injection_init_failed", true);
            report.insert("touch_test", touchResult);
        }
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!reportPath.isEmpty()) {
            QFile f(reportPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(json);
        }
        std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fflush(stdout);
        QGuiApplication::exit(code);
    };

    TouchSelfTest selfTest(window, &model, &probe);
    if (touchTest) {
        window->setFlag(Qt::WindowStaysOnTopHint);
        QObject::connect(&selfTest, &TouchSelfTest::finished, &app, [&](const QJsonObject& r) {
            touchResult = r;
            finish(r.value("pass").toBool() ? 0 : 3);
        });
        // Give the window time to show and lay out before touching it.
        QTimer::singleShot(1500, &selfTest, &TouchSelfTest::start);
    }
    if (const int sec = cli.value("duration").toInt(); sec > 0 && !touchTest)
        QTimer::singleShot(sec * 1000, &app, [&] { finish(0); });

    return QGuiApplication::exec();
}
