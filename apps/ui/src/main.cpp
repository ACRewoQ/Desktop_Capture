// dc-ui: the mixer user interface, for the local PC and the remote controller (Qt 6 Quick, no
// QtQuick.Controls; see docs/verification/v5-remote-lite.md). Talks to mixd, or to dc-mock-server
// during development, over the control protocol.
//
//   dc-ui [--connect HOST[:PORT]] [--remote] [--fullscreen] [--backend default|d3d11|software]
//         [--page KEY] [--screenshot FILE [--size WxH]] [--selftest] [--exercise MINUTES [--input ID|first] [--input-channel N] [--record]]

#include "connection.hpp"
#include "exercise.hpp"
#include "param_hub.hpp"
#include "recorder.hpp"
#include "route_choices.hpp"
#include "selftest.hpp"
#include "strip_model.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/version.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QImage>

#include <cstdio>
#include <cstring>

namespace {

int g_qmlWarnings = 0;
QtMessageHandler g_previousHandler = nullptr;

// Counts QML warnings (binding errors, missing types) so that the self test catches a broken screen.
void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (type >= QtWarningMsg && (msg.contains(QLatin1String("qml")) || msg.contains(QLatin1String(".qml"))))
        ++g_qmlWarnings;
    if (g_previousHandler) g_previousHandler(type, ctx, msg);
}

void print(const QString& line) {
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("dc-ui %s (protocol %u, Qt %s)\n", dc::kVersionString, dc::proto::kProtocolVersion, qVersion());
            return 0;
        }

#ifdef _WIN32
    // Qt 6.10's DXGI VBlank thread never wakes on some setups (Reo's PC with a virtual monitor),
    // which freezes D3D11 after the first frame. Found in P1/V6; must be set before the app object.
    if (qEnvironmentVariableIsEmpty("QT_D3D_NO_VBLANK_THREAD")) qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("dc-ui"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(dc::kVersionString));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Desktop Capture mixer UI"));
    cli.addHelpOption();
    cli.addOptions({
        {"connect", "Engine (mixd or dc-mock-server) to control.", "host[:port]", "127.0.0.1"},
        {"remote", "Connect as a remote controller (role 1) instead of the local UI."},
        {"fullscreen", "Show full screen (remote controller)."},
        {"pair", "With --remote: pair with the 6-digit CODE the engine shows (instead of typing it).", "code"},
        {"backend", "Scene graph backend: default (D3D11 on Windows), d3d11 or software.", "name", "default"},
        {"selftest", "Run the scripted check against the engine, print the result and exit (changes its state)."},
        {"no-meters", "With --selftest: do not expect meter packets."},
        {"exercise", "G2 run: keep moving the track controls for MINUTES with the screen up, print the result and exit.",
         "minutes"},
        {"input", "With --exercise: input device of the first track (an id from /devices/input, or \"first\").", "id"},
        {"input-channel", "With --exercise: first channel of that device to use.", "n", "1"},
        {"record", "With --exercise: record from start to end through the record button's logic."},
        {"page", "Page to show first (mixer, settings, ...).", "key"},
        {"screenshot", "Save the window as PNG FILE once connected and drawn, then exit.", "file"},
        {"size", "With --screenshot: window size.", "WxH", "1280x800"},
        {"demo", "Open a sheet with sample content (number, text, menu, input, output) for screenshots.", "name"},
    });
    cli.process(app);

    const QString backend = cli.value("backend");
    if (backend == QLatin1String("software")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    else if (backend == QLatin1String("d3d11")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    const bool selfTest = cli.isSet("selftest");
    const bool exercise = cli.isSet("exercise");
    if (selfTest || exercise) g_previousHandler = qInstallMessageHandler(messageHandler);

    dc::ui::Connection::Options opt;
    const QStringList hostPort = cli.value("connect").split(QLatin1Char(':'));
    opt.host = hostPort.value(0);
    opt.port = hostPort.size() > 1 ? static_cast<quint16>(hostPort[1].toUInt()) : dc::proto::kDefaultControlPort;
    opt.role = cli.isSet("remote") ? dc::proto::ClientRole::Remote : dc::proto::ClientRole::Local;
    opt.clientName = cli.isSet("remote") ? QStringLiteral("dc-ui-remote") : QStringLiteral("dc-ui");

    // A remote controller pairs once per engine (protocol.md 8.2): its id and the tokens it got are
    // kept in the user's settings (registry on Windows).
    QSettings settings(QStringLiteral("DesktopCapture"), QStringLiteral("dc-ui"));
    const QString tokenKey = QStringLiteral("tokens/%1_%2").arg(opt.host).arg(opt.port);
    if (opt.role == dc::proto::ClientRole::Remote) {
        QString id = settings.value(QStringLiteral("client_id")).toString();
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue(QStringLiteral("client_id"), id);
        }
        opt.clientId = id.toStdString();
        opt.token = settings.value(tokenKey).toString().toStdString();
        opt.clientName = QSysInfo::machineHostName().isEmpty() ? opt.clientName : QSysInfo::machineHostName();
    }

    dc::ui::Connection connection(opt);
    QObject::connect(&connection, &dc::ui::Connection::tokenChanged, [&](const QString& token) {
        if (token.isEmpty()) settings.remove(tokenKey);
        else settings.setValue(tokenKey, token);
    });
    dc::ui::StripModel tracks(connection, dc::ui::StripModel::Kind::Tracks);
    dc::ui::StripModel master(connection, dc::ui::StripModel::Kind::Master);
    dc::ui::Recorder recorder(connection);
    dc::ui::ParamHub params(connection);
    dc::ui::RouteChoices routes(connection);
    qmlRegisterType<dc::ui::ParamValue>("DcUi.Params", 1, 0, "Param");
    qmlRegisterType<dc::ui::ParamList>("DcUi.Params", 1, 0, "ParamList");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("connection"), &connection);
    engine.rootContext()->setContextProperty(QStringLiteral("tracks"), &tracks);
    engine.rootContext()->setContextProperty(QStringLiteral("master"), &master);
    engine.rootContext()->setContextProperty(QStringLiteral("recorder"), &recorder);
    engine.rootContext()->setContextProperty(QStringLiteral("params"), &params);
    engine.rootContext()->setContextProperty(QStringLiteral("routes"), &routes);
    engine.rootContext()->setContextProperty(QStringLiteral("startFullScreen"), cli.isSet("fullscreen"));
    engine.rootContext()->setContextProperty(QStringLiteral("startPage"), cli.value("page"));
    engine.rootContext()->setContextProperty(QStringLiteral("startDemo"), cli.value("demo"));
    engine.load(QUrl(QStringLiteral("qrc:/DcUi/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) return 1;

    if (cli.isSet("pair")) connection.pair(cli.value("pair"));
    connection.start();
    if (cli.isSet("screenshot")) {
        const QStringList size = cli.value("size").split(QLatin1Char('x'));
        window->resize(size.value(0).toInt(), size.value(1).toInt());
        QTimer::singleShot(0, [&] {
            dc::ui::waitUntil([&] { return connection.ready(); }, 5000);
            dc::ui::waitUntil([] { return false; }, 800); // let the page settle
            const QImage image = window->grabWindow();
            const bool ok = image.save(cli.value("screenshot"));
            print(QStringLiteral("%1 %2").arg(ok ? "saved" : "could not save", cli.value("screenshot")));
            QGuiApplication::exit(ok ? 0 : 1);
        });
        return QGuiApplication::exec();
    }
    if (exercise) {
        int frames = 0;
        QObject::connect(window, &QQuickWindow::frameSwapped, [&] { ++frames; });
        QTimer::singleShot(0, [&] {
            dc::ui::ExerciseOptions eo;
            eo.minutes = cli.value("exercise").toDouble();
            eo.input = cli.value("input");
            eo.inputChannel = cli.value("input-channel").toInt();
            eo.record = cli.isSet("record");
            print(QStringLiteral("dc-ui exercise against %1 for %2 min").arg(connection.address()).arg(eo.minutes));
            int failures = dc::ui::runExercise(connection, tracks, recorder, print, eo);
            print(QStringLiteral("%1 screen drew %2 frames, QML warnings %3")
                      .arg(frames > 0 && g_qmlWarnings == 0 ? "  OK  " : "  FAIL")
                      .arg(frames)
                      .arg(g_qmlWarnings));
            if (frames == 0 || g_qmlWarnings) ++failures;
            print(failures ? QStringLiteral("EXERCISE FAILED (%1)").arg(failures) : QStringLiteral("EXERCISE PASSED"));
            connection.stop();
            QGuiApplication::exit(failures ? 1 : 0);
        });
        return QGuiApplication::exec();
    }
    if (!selfTest) return QGuiApplication::exec();

    int frames = 0;
    QObject::connect(window, &QQuickWindow::frameSwapped, [&] { ++frames; });
    QTimer::singleShot(0, [&] {
        print(QStringLiteral("dc-ui self test against %1").arg(connection.address()));
        dc::ui::SelfTestOptions st;
        st.expectMeters = !cli.isSet("no-meters");
        int failures = dc::ui::runSelfTest(connection, tracks, master, recorder, print, st);
        // Every page once, so that a broken page shows up as QML warnings below.
        const QStringList pages = window->property("pageKeys").toStringList();
        for (const QString& key : pages) {
            window->setProperty("currentPage", key);
            dc::ui::waitUntil([] { return false; }, 300);
        }
        window->setProperty("currentPage", QStringLiteral("mixer"));
        print(QStringLiteral("%1 pages shown (%2)").arg(pages.size() > 1 ? "  OK  " : "  FAIL", pages.join(QLatin1Char(' '))));
        if (pages.size() <= 1) ++failures;
        // The screen kept drawing while the models changed (catches the D3D11 first-frame freeze).
        const bool drawing = dc::ui::waitUntil([&] { return frames >= 10; }, 3000);
        print(QStringLiteral("%1 screen keeps drawing (%2 frames)").arg(drawing ? "  OK  " : "  FAIL", QString::number(frames)));
        if (!drawing) ++failures;
        print(QStringLiteral("%1 no QML warnings (%2)").arg(g_qmlWarnings == 0 ? "  OK  " : "  FAIL").arg(g_qmlWarnings));
        if (g_qmlWarnings) ++failures;
        print(failures ? QStringLiteral("SELFTEST FAILED (%1)").arg(failures) : QStringLiteral("SELFTEST PASSED"));
        connection.stop();
        QGuiApplication::exit(failures ? 1 : 0);
    });
    return QGuiApplication::exec();
}
