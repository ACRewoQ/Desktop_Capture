#include "measure.hpp"

#include "monitor_audio.hpp"
#include "preview_stream.hpp"
#include "preview_video.hpp"
#include "selftest.hpp"

#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <ctime>
#include <fstream>
#include <string>
#endif

namespace dc::ui {

namespace {

struct Memory {
    double workingSetMb = 0;
    double privateMb = 0;
};

Memory memory() {
    Memory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.workingSetMb = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.privateMb = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
#else
    std::ifstream f("/proc/self/status");
    std::string key;
    double kb = 0;
    std::string unit;
    while (f >> key) {
        if (key == "VmRSS:" && f >> kb >> unit) m.workingSetMb = kb / 1024.0;
        else if (key == "RssAnon:" && f >> kb >> unit) m.privateMb = kb / 1024.0;
    }
#endif
    return m;
}

// CPU time of this process, seconds (all threads).
double cpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto sec = [](const FILETIME& t) { return double((ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7; };
    return sec(k) + sec(u);
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
#endif
}

QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) return root;
    for (QQuickItem* c : root->childItems())
        if (QQuickItem* f = findItem(c, name)) return f;
    return nullptr;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(double(v.size()) * p))];
}

void sleepMs(int ms) { waitUntil([] { return false; }, ms); }

} // namespace

int runMeasure(QQuickWindow* window, PreviewStream& stream, MonitorAudio* monitor, double seconds,
               const std::function<void(const QString&)>& log) {
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("  OK  ") : QStringLiteral("  FAIL"), what));
        if (!ok) ++failures;
    };
    const int cores = std::max(1, QThread::idealThreadCount());

    // The mixer first: the memory of the remote that is not watching.
    window->setProperty("currentPage", QStringLiteral("mixer"));
    sleepMs(3000);
    const Memory idle = memory();
    log(QStringLiteral("memory on the mixer page: working set %1 MB, private %2 MB")
            .arg(idle.workingSetMb, 0, 'f', 1).arg(idle.privateMb, 0, 'f', 1));

    // The preview tab of the video page.
    window->setProperty("currentPage", QStringLiteral("video"));
    sleepMs(300);
    if (QQuickItem* page = findItem(window->contentItem(), QStringLiteral("videoPage"))) page->setProperty("view", QStringLiteral("preview"));
    QQuickItem* item = nullptr;
    waitUntil([&] { return (item = findItem(window->contentItem(), QStringLiteral("previewVideo"))) != nullptr; }, 3000);
    auto* video = qobject_cast<PreviewVideo*>(item);
    if (!video) {
        check(false, QStringLiteral("preview tab opened"));
        return failures;
    }
    const bool showing = waitUntil([&] { return video->showing(); }, 15000);
    check(showing, QStringLiteral("preview picture shown (%1)").arg(video->error().isEmpty() ? stream.statusText() : video->error()));
    if (!showing) return failures;
    check(waitUntil([&] { return stream.clockKnown(); }, 5000), QStringLiteral("engine clock offset known"));
    sleepMs(2000);  // past the first keyframe and the decoder start
    video->takeLatencySamples();

    std::vector<double> all, ws, priv;
    double cpuStart = cpuSeconds();
    QElapsedTimer wall;
    wall.start();
    const std::uint64_t framesStart = stream.frames();
    double lastCpu = cpuStart;
    qint64 lastWall = 0;
    for (int s = 1; s <= int(std::ceil(seconds)); ++s) {
        sleepMs(1000);
        const auto samples = video->takeLatencySamples();
        all.insert(all.end(), samples.begin(), samples.end());
        const Memory m = memory();
        ws.push_back(m.workingSetMb);
        priv.push_back(m.privateMb);
        const double cpu = cpuSeconds();
        const qint64 now = wall.elapsed();
        const double cpuPct = 100.0 * (cpu - lastCpu) / (double(now - lastWall) / 1000.0) / cores;
        lastCpu = cpu;
        lastWall = now;
        log(QStringLiteral("  %1 s: %2 frames shown, delay median %3 ms / max %4 ms, network %5 ms, WS %6 MB, private %7 MB, CPU %8%")
                .arg(s, 3)
                .arg(samples.size())
                .arg(percentile(samples, 0.5), 0, 'f', 1)
                .arg(samples.empty() ? -1.0 : *std::max_element(samples.begin(), samples.end()), 0, 'f', 1)
                .arg(stream.networkMs(), 0, 'f', 1)
                .arg(m.workingSetMb, 0, 'f', 1)
                .arg(m.privateMb, 0, 'f', 1)
                .arg(cpuPct, 0, 'f', 1));
        if (monitor && monitor->stream() > 0)
            log(QStringLiteral("        monitor audio: %1, peak %2 dBFS, %3")
                    .arg(monitor->playing() ? QStringLiteral("playing") : monitor->statusText())
                    .arg(monitor->level(), 0, 'f', 1)
                    .arg(monitor->infoText()));
    }
    const double elapsed = double(wall.elapsed()) / 1000.0;
    const double fps = double(stream.frames() - framesStart) / elapsed;
    const double cpuPct = 100.0 * (cpuSeconds() - cpuStart) / elapsed / cores;
    const double p50 = percentile(all, 0.5), p95 = percentile(all, 0.95), worst = all.empty() ? -1 : *std::max_element(all.begin(), all.end());
    log(QStringLiteral("summary: %1 frames shown in %2 s (received %3 fps), delay p50 %4 ms, p95 %5 ms, max %6 ms")
            .arg(all.size()).arg(elapsed, 0, 'f', 0).arg(fps, 0, 'f', 1).arg(p50, 0, 'f', 1).arg(p95, 0, 'f', 1).arg(worst, 0, 'f', 1));
    log(QStringLiteral("summary: while showing the preview working set max %1 MB, private max %2 MB, CPU %3% of %4 cores")
            .arg(*std::max_element(ws.begin(), ws.end()), 0, 'f', 1)
            .arg(*std::max_element(priv.begin(), priv.end()), 0, 'f', 1)
            .arg(cpuPct, 0, 'f', 1)
            .arg(cores));
    check(!all.empty() && p95 <= 200, QStringLiteral("preview delay p95 <= 200 ms (G4): %1 ms").arg(p95, 0, 'f', 1));
    if (monitor && monitor->stream() > 0)
        check(monitor->playing(), QStringLiteral("monitor audio playing (%1)").arg(monitor->infoText()));

    // Leave the preview: the subscription ends and the decoder goes.
    window->setProperty("currentPage", QStringLiteral("mixer"));
    sleepMs(3000);
    const Memory after = memory();
    log(QStringLiteral("memory back on the mixer page: working set %1 MB, private %2 MB")
            .arg(after.workingSetMb, 0, 'f', 1).arg(after.privateMb, 0, 'f', 1));
    check(!stream.subscribed(), QStringLiteral("preview unsubscribed after leaving the tab"));
    return failures;
}

} // namespace dc::ui
