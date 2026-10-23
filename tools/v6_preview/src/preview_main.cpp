// dc-v6-preview: V6 measurement with the real display path (Qt Quick on Direct3D 11).
//
//   dc-v6-preview [options]                 generate + encode + decode + show in one process; reports the
//                                           latency from frame generation to QQuickWindow::frameSwapped
//   dc-v6-preview --listen PORT [options]   receiver for dc-v6-bench --send on another PC (e.g. run this on
//                                           the remote device); reports receive -> present locally, the
//                                           sender reports the full round trip
//   --fullscreen                            borderless full screen

#include "common.hpp"
#include "options.hpp"
#include "pipeline.hpp"
#include "stats.hpp"
#include "video_item.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <psapi.h>

#include <cstdio>
#include <iostream>
#include <memory>

using namespace v6;

namespace {


struct Session {
    CliOptions opt;
    std::unique_ptr<Stats> stats;
    std::unique_ptr<ReceiverPipeline> rx;
    std::unique_ptr<SenderPipeline> tx;
    std::unique_ptr<TcpReceiver> link;
    VideoItem* item = nullptr;
    std::int64_t cpu0 = 0, wall0 = 0;
    bool finished = false;

    void start(const LUID& luid) {
        const bool listen = opt.listenPort > 0;
        stats = std::make_unique<Stats>(Presented, listen ? 0 : static_cast<std::uint64_t>(opt.warmup));
        rx = std::make_unique<ReceiverPipeline>(opt.pipe, *stats, &luid, [this](std::uint64_t) {
            QMetaObject::invokeMethod(item, &QQuickItem::update, Qt::QueuedConnection);
        });
        QString info;
        if (listen) {
            link = std::make_unique<TcpReceiver>(opt.listenPort, [this](EncodedUnit&& u) { rx->push(std::move(u)); });
            item->attach(&rx->ring(), stats.get(), [this](const AckTimes& a) { link->sendAck(a); });
            info = QStringLiteral("listening on TCP %1").arg(opt.listenPort);
        } else {
            item->attach(&rx->ring(), stats.get(), nullptr);
            tx = std::make_unique<SenderPipeline>(opt.pipe, *stats, [this](EncodedUnit&& u) { rx->push(std::move(u)); });
            tx->start();
            info = QStringLiteral("encoder %1%2")
                       .arg(QString::fromStdString(tx->encoderName()), tx->encoderIsHardware() ? " [hw]" : "");
        }
        info += QStringLiteral(" | decoder %1 | %2x%3@%4 %5 kbps | %6")
                    .arg(QString::fromStdString(rx->decoderName()))
                    .arg(opt.pipe.width)
                    .arg(opt.pipe.height)
                    .arg(opt.pipe.fps)
                    .arg(opt.pipe.kbps)
                    .arg(QString::fromStdString(rx->adapterName()));
        item->setInfoText(info);
        std::cout << info.toStdString() << std::endl;
        cpu0 = processCpuUs();
        wall0 = nowUs();
    }

    void finish() {
        if (finished) return;
        finished = true;
        if (item) item->detach();
        if (tx) tx->stop();
        link.reset();
        if (rx) rx->stop();
        if (!stats) return;
        const bool listen = opt.listenPort > 0;
        std::string title = opt.label;
        if (title.empty())
            title = listen ? "dc-v6-preview receiver (receive -> present)" : "dc-v6-preview local (gen -> present)";
        std::cout << stats->report(title);
        const double cpu = static_cast<double>(processCpuUs() - cpu0), wall = static_cast<double>(nowUs() - wall0);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        std::printf("process CPU: %.1f%% of one core, %.2f%% of all %lu logical cores\n", 100.0 * cpu / wall,
                    100.0 * cpu / wall / si.dwNumberOfProcessors, static_cast<unsigned long>(si.dwNumberOfProcessors));
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
            std::printf("memory: working set %.0f MB, peak %.0f MB\n", pmc.WorkingSetSize / 1048576.0,
                        pmc.PeakWorkingSetSize / 1048576.0);
        if (item) std::printf("%s, decoded %llu\n", item->counters().c_str(),
                              static_cast<unsigned long long>(rx ? rx->decodedFrames() : 0));
        if (rx) std::printf("%s\n", rx->probeSummary().c_str());
        if (item) std::printf("%s\n", item->probeText().c_str());
        std::printf("note: the monitor shows the frame about one refresh after frameSwapped (not included).\n");
        if (rx && !rx->error().empty()) std::cout << "RESULT: ERROR " << rx->error() << "\n";
        if (!opt.csv.empty()) stats->writeCsv(opt.csv);
        std::fflush(stdout);
        // rx (decode device, creator of the shared textures) is destroyed with the Session, which outlives
        // the QML engine: Qt's device releases its opened shared textures first. Releasing them after the
        // creating device was gone crashed in the D3D11 runtime at exit.
    }
};

}  // namespace

int main(int argc, char** argv) {
    CliOptions opt;
    if (!parseCli(argc, argv, opt) || opt.help) {
        std::cout << "usage: dc-v6-preview [--listen PORT] [--fullscreen] [options]\n"
                  << kCommonUsage;
        return opt.help ? 0 : 2;
    }

    installCrashReporter();
    // Qt 6.10 paces requestUpdate() with a DXGI vblank thread. On the development PC (RTX 5090 plus a
    // "Meta Virtual Monitor" display driver, "Unable to open monitor interface") that thread never delivers a
    // vblank, so the window renders one frame and stops. Fall back to timer pacing unless told otherwise.
    if (!qEnvironmentVariableIsSet("QT_D3D_NO_VBLANK_THREAD")) qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    std::unique_ptr<MfRuntime> mf;
    try {
        mf = std::make_unique<MfRuntime>();
    } catch (const std::exception& e) {
        std::cerr << "RESULT: ERROR " << e.what() << "\n";
        return 1;
    }

    // Declared before the engine so it is destroyed after the window and its render thread.
    Session session;
    session.opt = opt;

    QQmlApplicationEngine engine;
    engine.loadFromModule("DcV6", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* item = window ? window->findChild<VideoItem*>("video") : nullptr;
    if (!item) {
        std::cerr << "RESULT: ERROR VideoItem not found\n";
        return 1;
    }

    session.item = item;
    int exitCode = 0;

    // The decode device must sit on the same adapter as Qt's device, which exists once the scene graph is up.
    QObject::connect(
        window, &QQuickWindow::sceneGraphInitialized, window,
        [window, &session, &app, &exitCode] {
            auto* ri = window->rendererInterface();
            auto* dev = static_cast<ID3D11Device*>(ri->getResource(window, QSGRendererInterface::DeviceResource));
            if (!dev) {
                std::cerr << "RESULT: ERROR scene graph has no D3D11 device\n";
                QMetaObject::invokeMethod(&app, [&exitCode] { exitCode = 1; QCoreApplication::quit(); },
                                          Qt::QueuedConnection);
                return;
            }
            const LUID luid = adapterLuidOf(dev);
            QMetaObject::invokeMethod(
                &app,
                [&session, luid, &exitCode] {
                    try {
                        session.start(luid);
                    } catch (const std::exception& e) {
                        std::cerr << "RESULT: ERROR " << e.what() << "\n";
                        exitCode = 1;
                        QCoreApplication::quit();
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);

    QTimer live;
    int ticks = 0;
    QObject::connect(&live, &QTimer::timeout, [&] {
        if (!session.stats) return;
        item->setStatsText(QString::fromStdString(session.stats->liveLine()));
        if (++ticks % 4 == 0)
            std::cout << "  " << session.stats->liveLine() << " | " << item->counters() << std::endl;
        if (session.rx && !session.rx->error().empty()) {
            exitCode = 1;
            QCoreApplication::quit();
        }
    });
    live.start(500);

    if (opt.seconds > 0)
        QTimer::singleShot(static_cast<int>(opt.seconds * 1000), &app, [] { QCoreApplication::quit(); });
    // Show only now: the threaded render loop initializes the scene graph as soon as the window is exposed,
    // and sceneGraphInitialized must not fire before the connection above exists.
    // Stay on top: a fully covered window is "occluded" and Qt stops rendering it, which would stall the
    // measurement when the tool is started from a script behind other windows.
    window->setFlag(Qt::WindowStaysOnTopHint, true);
    if (opt.fullscreen) window->showFullScreen();
    else window->show();
    window->raise();
    window->requestActivate();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&] { session.finish(); });
    const int rc = app.exec();
    session.finish();
    return rc ? rc : exitCode;
}
