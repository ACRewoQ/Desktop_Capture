#include "probe.hpp"

#include <QQuickWindow>
#include <QThread>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <cstdio>
#include <cstring>
#endif

namespace {

constexpr double kMb = 1024.0 * 1024.0;

// Total CPU time (user + kernel) of this process in seconds.
double processCpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto toSec = [](const FILETIME& ft) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<double>(u.QuadPart) / 1e7;
    };
    return toSec(kernel) + toSec(user);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#endif
}

} // namespace

Probe::Memory Probe::sampleMemory() {
    Memory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.workingSetMb = static_cast<double>(pmc.WorkingSetSize) / kMb;
        m.privateMb = static_cast<double>(pmc.PrivateUsage) / kMb;
        m.peakWorkingSetMb = static_cast<double>(pmc.PeakWorkingSetSize) / kMb;
    }
#else
    // Linux (development only): VmRSS / VmHWM, and RssAnon as the closest match to private bytes.
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            long kb = 0;
            if (std::sscanf(line, "VmRSS: %ld", &kb) == 1) m.workingSetMb = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "VmHWM: %ld", &kb) == 1) m.peakWorkingSetMb = static_cast<double>(kb) / 1024.0;
            else if (std::sscanf(line, "RssAnon: %ld", &kb) == 1) m.privateMb = static_cast<double>(kb) / 1024.0;
        }
        std::fclose(f);
    }
#endif
    return m;
}

Probe::Probe(QString info, QObject* parent) : QObject(parent), info_(std::move(info)) {
    wall_.start();
    lastCpuSec_ = processCpuSeconds();
    mem_ = sampleMemory();
    connect(&timer_, &QTimer::timeout, this, &Probe::tick);
    timer_.start(1000);
}

void Probe::attach(QQuickWindow* window) {
    connect(window, &QQuickWindow::frameSwapped, this, [this] { ++frames_; });
}

void Probe::tick() {
    const double now = static_cast<double>(wall_.nsecsElapsed()) / 1e9;
    const double cpu = processCpuSeconds();
    const double dt = now - lastWallSec_;
    if (dt > 0) {
        // Percent of the whole machine, like Task Manager.
        const int cores = std::max(1, QThread::idealThreadCount());
        cpuPercent_ = 100.0 * (cpu - lastCpuSec_) / dt / cores;
        fps_ = static_cast<int>(frames_ / dt + 0.5);
    }
    lastWallSec_ = now;
    lastCpuSec_ = cpu;
    frames_ = 0;
    mem_ = sampleMemory();
    emit statsChanged();
}

void Probe::faderGrab(int row, bool grabbed) {
    if (grabbed) grabbed_.insert(row);
    else grabbed_.remove(row);
    maxSimultaneous_ = std::max(maxSimultaneous_, activeFaders());
    emit fadersChanged();
}

void Probe::resetMaxSimultaneous() {
    maxSimultaneous_ = activeFaders();
    emit fadersChanged();
}

QJsonObject Probe::toJson() const {
    const Memory now = sampleMemory();
    return {
        {"working_set_mb", now.workingSetMb},
        {"private_mb", now.privateMb},
        {"peak_working_set_mb", now.peakWorkingSetMb},
        {"cpu_percent", cpuPercent_},
        {"fps", fps_},
        {"max_simultaneous_faders", maxSimultaneous_},
        {"qml_warnings", qmlWarnings_},
    };
}
