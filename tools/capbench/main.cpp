// dc-capbench: verification tool for P1-V3 (desktop capture + hardware encoding, all on the GPU).
//
//   dc-capbench list                       monitors, GPUs and the hardware encoders this FFmpeg build has
//   dc-capbench run [options]              capture -> NV12 (GPU) -> encode, prints load once per second
//   dc-capbench pattern [--monitor N] [--full] [--seconds S]   moving test pattern window
//
// Pipeline (no CPU copy of any pixel):
//   WGC / Desktop Duplication surface --CopyResource--> BGRA texture --ID3D11VideoProcessor--> NV12 pool texture
//   --FFmpeg (NVENC / AMF / MF: D3D11 frames, QSV: mapped)--> packets --> .mkv/.mp4 or discarded
// The loop runs at a fixed output rate; when the desktop did not change, the last image is converted again (as the
// compositor in lane B will do).

#include "capture_source.hpp"
#include "converter.hpp"
#include "d3d.hpp"
#include "encoder.hpp"
#include "metrics.hpp"
#include "pattern.hpp"

#include "dc/version.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace dc::capbench;

namespace {

struct RunOptions {
    std::string source = "wgc";
    int monitor = -1;  // -1 = primary
    std::string size = "1920x1080";
    double seconds = 30;
    double warmup = 3;
    std::string pattern;  // "", "window" or "full"
    std::string csv;
    bool quiet = false;
    EncoderConfig enc;
};

void usage() {
    std::cout <<
        R"(dc-capbench - P1-V3 capture + hardware encode bench

usage:
  dc-capbench list
  dc-capbench run [options]
  dc-capbench pattern [--monitor N] [--full] [--seconds S]

run options:
  --source wgc|dda        capture API (default wgc; dda = Desktop Duplication, the Win10 fallback)
  --monitor N             monitor index from "list" (default: primary)
  --encoder NAME|none     FFmpeg encoder: h264_nvenc hevc_nvenc av1_nvenc h264_amf hevc_amf av1_amf
                          h264_qsv hevc_qsv av1_qsv h264_mf hevc_mf ... (default h264_nvenc)
                          none = capture and convert only
  --size WxH|native       encoded size (default 1920x1080; the desktop is scaled on the GPU)
  --fps N                 output frame rate (default 60)
  --bitrate N[k|M]        target bitrate (default 12M)
  --opt key=value         encoder private option (repeatable), e.g. --opt preset=p1
  --pool N                NV12 surfaces in the pool (default 32)
  --seconds S             measured duration (default 30)
  --warmup S              seconds excluded from the result (default 3)
  --output FILE           write .mkv/.mp4 (default: packets are discarded)
  --pattern window|full   show the moving test pattern (separate process) while measuring
  --csv FILE              per-second samples as CSV
  --quiet                 only the final RESULT line
)";
}

std::int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

std::int64_t qpcFreq() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

std::int64_t parseBitrate(const std::string& s) {
    double v = std::atof(s.c_str());
    if (!s.empty()) {
        const char suffix = s.back();
        if (suffix == 'k' || suffix == 'K') v *= 1e3;
        if (suffix == 'm' || suffix == 'M') v *= 1e6;
    }
    return static_cast<std::int64_t>(v);
}

const MonitorInfo& pickMonitor(const std::vector<MonitorInfo>& monitors, int index) {
    if (monitors.empty()) throw std::runtime_error("no monitor attached to the desktop");
    if (index >= 0) {
        if (index >= static_cast<int>(monitors.size())) throw std::runtime_error("no such monitor index");
        return monitors[static_cast<size_t>(index)];
    }
    for (const auto& m : monitors)
        if (m.primary) return m;
    return monitors.front();
}

int cmdList() {
    std::cout << "dc-capbench " << dc::kVersionString << ", FFmpeg " << ffmpegVersion() << "\n\nmonitors:\n";
    for (const auto& m : enumerateMonitors()) {
        std::cout << "  [" << m.index << "] " << m.deviceName << "  " << (m.desktop.right - m.desktop.left) << "x"
                  << (m.desktop.bottom - m.desktop.top) << " at (" << m.desktop.left << "," << m.desktop.top << ")"
                  << (m.primary ? "  primary" : "") << "  on " << m.adapterName << "\n";
    }
    std::cout << "\nhardware encoders in this FFmpeg build (D3D11/QSV input):\n";
    const auto monitors = enumerateMonitors();
    const MonitorInfo& mon = pickMonitor(monitors, -1);
    const Device dev = createDevice(mon.adapter.Get());
    for (const auto& name : availableHwEncoders()) {
        std::string status;
        try {
            // Open a small session to see whether the GPU/driver really supports it.
            EncoderConfig cfg;
            cfg.codec = name;
            cfg.width = 1280;
            cfg.height = 720;
            cfg.poolSize = 4;
            cfg.bitrate = 2'000'000;
            Encoder enc(dev, cfg);
            auto s = enc.acquire();
            if (!s.frame) throw std::runtime_error("pool empty");
            enc.submit(s, 0, qpcNow());
            enc.finish();
            status = enc.stats().packets > 0 ? "works" : "opened, no packet";
        } catch (const std::exception& e) {
            status = std::string("unavailable: ") + e.what();
        }
        std::cout << "  " << name << "  " << status << "\n";
    }
    return 0;
}

// Starts "dc-capbench pattern" as a child process; returns its handle (null on failure).
HANDLE startPattern(const RunOptions& o) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" pattern --seconds " +
                       std::to_wstring(static_cast<int>(o.seconds + o.warmup + 10));
    if (o.monitor >= 0) cmd += L" --monitor " + std::to_wstring(o.monitor);
    if (o.pattern == "full") cmd += L" --full";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    Sleep(1000);  // let the window appear before capture starts
    return pi.hProcess;
}

struct Totals {
    double seconds = 0;
    std::uint64_t captured = 0, encoded = 0, dropped = 0, poolEmpty = 0;
    double cpuSum = 0, cpuMax = 0, cpuCoreSum = 0, sysSum = 0;
    double gpu3d = 0, gpuEnc = 0, gpuVp = 0, gpuEncAll = 0;
    int samples = 0, gpuSamples = 0;
    double tickMsSum = 0, tickMsMax = 0;
    std::uint64_t ticks = 0;
    std::uint64_t workingSetMax = 0;
};

int cmdRun(RunOptions& o) {
    const auto monitors = enumerateMonitors();
    const MonitorInfo& mon = pickMonitor(monitors, o.monitor);
    const Device dev = createDevice(mon.adapter.Get());

    std::unique_ptr<CaptureSource> source =
        o.source == "dda" ? createDdaSource(dev, mon) : createWgcSource(dev, mon);

    // Wait for the first frame to learn the desktop size (WGC delivers one right away; DDA after any change).
    HANDLE pattern = o.pattern.empty() ? nullptr : startPattern(o);
    const std::int64_t freq = qpcFreq();
    {
        const std::int64_t deadline = qpcNow() + 3 * freq;
        while (!source->texture() && qpcNow() < deadline) {
            source->poll();
            Sleep(5);
        }
        if (!source->texture()) {
            // Desktop Duplication only reports changes; with a static desktop nothing comes. Not fatal.
            std::cerr << "warning: no frame within 3 s (static desktop?); continuing\n";
        }
    }

    if (o.size == "native") {
        o.enc.width = static_cast<int>(mon.desktop.right - mon.desktop.left);
        o.enc.height = static_cast<int>(mon.desktop.bottom - mon.desktop.top);
    } else if (std::sscanf(o.size.c_str(), "%dx%d", &o.enc.width, &o.enc.height) != 2) {
        throw std::runtime_error("bad --size");
    }
    o.enc.width &= ~1;
    o.enc.height &= ~1;

    Encoder encoder(dev, o.enc);
    Converter converter(dev, static_cast<UINT>(o.enc.width), static_cast<UINT>(o.enc.height),
                        static_cast<UINT>(o.enc.fps));
    LoadMeter meter;

    std::ofstream csv;
    if (!o.csv.empty()) {
        csv.open(o.csv);
        csv << "t,capture_fps,encode_fps,dropped,proc_cpu_pct,proc_cpu_core_pct,sys_cpu_pct,gpu_3d_pct,"
               "gpu_videoproc_pct,gpu_encode_pct,gpu_encode_all_pct,mbps,latency_avg_ms,latency_max_ms,"
               "tick_avg_ms,tick_max_ms,working_set_mb\n";
    }

    if (!o.quiet) {
        std::cout << "source " << source->name() << " on " << mon.deviceName << " ("
                  << (mon.desktop.right - mon.desktop.left) << "x" << (mon.desktop.bottom - mon.desktop.top)
                  << ") GPU " << dev.adapterName << "\nencode " << encoder.description() << " " << o.enc.width << "x"
                  << o.enc.height << "@" << o.enc.fps << " " << o.enc.bitrate / 1000 << " kbps, CPU cores "
                  << meter.logicalCores() << "\n";
    }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    const std::int64_t period = freq / o.enc.fps;
    const std::int64_t t0 = qpcNow();
    const std::int64_t warmupEnd = t0 + static_cast<std::int64_t>(o.warmup * double(freq));
    const std::int64_t end = warmupEnd + static_cast<std::int64_t>(o.seconds * double(freq));
    std::int64_t nextSample = t0 + freq;
    bool measuring = false;

    Totals tot;
    std::uint64_t secCaptured = 0, secEncoded = 0, secDropped = 0;
    double secTickSum = 0, secTickMax = 0;
    std::uint64_t secTicks = 0;
    EncoderStats lastEnc{};   // at the previous per-second sample
    EncoderStats startEnc{};  // when the measured part began

    std::int64_t tick = 0;
    for (;;) {
        const std::int64_t due = t0 + tick * period;
        std::int64_t now = qpcNow();
        if (due > now) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>((due - now) * 10'000'000 / freq);
            SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
            now = qpcNow();
        } else if (now - due >= period) {
            // Fell behind by at least a frame: skip the missed ticks (they would be dropped frames in a recording).
            const std::int64_t missed = (now - due) / period;
            secDropped += static_cast<std::uint64_t>(missed);
            tick += missed;
        }
        if (now >= end) break;

        const std::int64_t work0 = qpcNow();
        if (source->poll()) ++secCaptured;
        if (source->texture()) {
            auto surf = encoder.acquire();
            if (!surf.frame) {
                if (measuring) ++tot.poolEmpty;
            } else {
                converter.convert(source->texture(), surf.texture, surf.arrayIndex);
                encoder.submit(surf, tick, work0);
                ++secEncoded;
            }
        }
        const double tickMs = double(qpcNow() - work0) * 1000.0 / double(freq);
        secTickSum += tickMs;
        secTickMax = std::max(secTickMax, tickMs);
        ++secTicks;
        ++tick;

        if (qpcNow() >= nextSample) {
            nextSample += freq;
            const LoadSample ls = meter.sample();
            const EncoderStats& es = encoder.stats();
            const double mbps = double(es.bytes - lastEnc.bytes) * 8 / 1e6;
            const std::uint64_t latN = es.latencyCount - lastEnc.latencyCount;
            const double latAvg = latN ? (es.latencySumMs - lastEnc.latencySumMs) / double(latN) : 0;
            const double elapsed = double(qpcNow() - t0) / double(freq);

            if (!o.quiet) {
                char line[400];
                std::snprintf(line, sizeof line,
                              "%5.0fs %s cap %3llu enc %3llu drop %llu | cpu %5.2f%% (%5.1f%% of 1 core) sys %5.1f%% "
                              "| gpu 3d %4.1f vproc %4.1f venc %4.1f (all %4.1f) | %5.1f Mbps lat %4.1f ms | tick "
                              "%4.2f/%4.2f ms",
                              elapsed, measuring ? " " : "w", static_cast<unsigned long long>(secCaptured),
                              static_cast<unsigned long long>(secEncoded), static_cast<unsigned long long>(secDropped),
                              ls.processCpuPct, ls.processCpuCorePct, ls.systemCpuPct, ls.gpu3dPct,
                              ls.gpuVideoProcessingPct, ls.gpuEncodePct, ls.gpuEncodeAllPct, mbps, latAvg,
                              secTicks ? secTickSum / double(secTicks) : 0, secTickMax);
                std::cout << line << "\n" << std::flush;
            }
            if (csv.is_open()) {
                csv << elapsed << "," << secCaptured << "," << secEncoded << "," << secDropped << ","
                    << ls.processCpuPct << "," << ls.processCpuCorePct << "," << ls.systemCpuPct << ","
                    << ls.gpu3dPct << "," << ls.gpuVideoProcessingPct << "," << ls.gpuEncodePct << ","
                    << ls.gpuEncodeAllPct << "," << mbps << "," << latAvg << "," << es.latencyMaxMs << ","
                    << (secTicks ? secTickSum / double(secTicks) : 0) << "," << secTickMax << ","
                    << ls.workingSetBytes / (1024 * 1024) << "\n";
            }
            if (measuring) {
                tot.seconds += 1;
                tot.captured += secCaptured;
                tot.encoded += secEncoded;
                tot.dropped += secDropped;
                tot.cpuSum += ls.processCpuPct;
                tot.cpuCoreSum += ls.processCpuCorePct;
                tot.cpuMax = std::max(tot.cpuMax, ls.processCpuPct);
                tot.sysSum += ls.systemCpuPct;
                if (ls.gpuEncodeAllPct >= 0) {
                    tot.gpu3d += ls.gpu3dPct;
                    tot.gpuVp += ls.gpuVideoProcessingPct;
                    tot.gpuEnc += ls.gpuEncodePct;
                    tot.gpuEncAll += ls.gpuEncodeAllPct;
                    ++tot.gpuSamples;
                }
                tot.tickMsSum += secTickSum;
                tot.ticks += secTicks;
                tot.tickMsMax = std::max(tot.tickMsMax, secTickMax);
                tot.workingSetMax = std::max<std::uint64_t>(tot.workingSetMax, ls.workingSetBytes);
                ++tot.samples;
            }
            if (!measuring && qpcNow() >= warmupEnd) {
                measuring = true;
                encoder.resetLatencyMax();  // the first frames after opening the encoder are slow
                startEnc = es;
            }
            lastEnc = es;
            secCaptured = secEncoded = secDropped = 0;
            secTickSum = secTickMax = 0;
            secTicks = 0;
        }
    }

    const EncoderStats es = encoder.stats();  // before the flush, which adds the queued frames at once
    encoder.finish();
    if (timer) CloseHandle(timer);
    if (pattern) {
        TerminateProcess(pattern, 0);
        CloseHandle(pattern);
    }

    const double n = tot.samples ? double(tot.samples) : 1;
    const double g = tot.gpuSamples ? double(tot.gpuSamples) : 1;
    const std::uint64_t latN = es.latencyCount - startEnc.latencyCount;
    char res[900];
    std::snprintf(res, sizeof res,
                  "RESULT source=%s encoder=%s size=%dx%d fps=%d seconds=%.0f capture_fps=%.1f encode_fps=%.1f "
                  "dropped=%llu pool_empty=%llu cpu_avg_pct=%.2f cpu_max_pct=%.2f cpu_core_avg_pct=%.1f "
                  "sys_cpu_avg_pct=%.1f gpu_3d_pct=%.1f gpu_videoproc_pct=%.1f gpu_encode_pct=%.1f "
                  "gpu_encode_all_pct=%.1f mbps=%.2f latency_avg_ms=%.2f latency_max_ms=%.2f tick_avg_ms=%.3f "
                  "tick_max_ms=%.2f working_set_mb=%llu recoveries=%llu cores=%d gpu=\"%s\"",
                  source->name(), o.enc.codec.c_str(), o.enc.width, o.enc.height, o.enc.fps, tot.seconds,
                  double(tot.captured) / n, double(tot.encoded) / n, static_cast<unsigned long long>(tot.dropped),
                  static_cast<unsigned long long>(tot.poolEmpty), tot.cpuSum / n, tot.cpuMax, tot.cpuCoreSum / n,
                  tot.sysSum / n, tot.gpu3d / g, tot.gpuVp / g, tot.gpuEnc / g, tot.gpuEncAll / g,
                  double(es.bytes - startEnc.bytes) * 8 / 1e6 / std::max(1.0, tot.seconds),
                  latN ? (es.latencySumMs - startEnc.latencySumMs) / double(latN) : 0.0, es.latencyMaxMs,
                  tot.ticks ? tot.tickMsSum / double(tot.ticks) : 0.0, tot.tickMsMax,
                  static_cast<unsigned long long>(tot.workingSetMax / (1024 * 1024)),
                  static_cast<unsigned long long>(source->recoveries()), meter.logicalCores(),
                  dev.adapterName.c_str());
    std::cout << res << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "--version") {
        std::cout << "dc-capbench " << dc::kVersionString << " (FFmpeg " << ffmpegVersion() << ")\n";
        return 0;
    }

    try {
        const std::string cmd = args[0];
        RunOptions o;
        bool full = false;
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= args.size()) throw std::runtime_error("missing value for " + a);
                return args[++i];
            };
            if (a == "--source") o.source = next();
            else if (a == "--monitor") o.monitor = std::atoi(next().c_str());
            else if (a == "--encoder") o.enc.codec = next();
            else if (a == "--size") o.size = next();
            else if (a == "--fps") o.enc.fps = std::max(1, std::atoi(next().c_str()));
            else if (a == "--bitrate") o.enc.bitrate = parseBitrate(next());
            else if (a == "--pool") o.enc.poolSize = std::max(4, std::atoi(next().c_str()));
            else if (a == "--seconds") o.seconds = std::atof(next().c_str());
            else if (a == "--warmup") o.warmup = std::atof(next().c_str());
            else if (a == "--output") o.enc.output = next();
            else if (a == "--pattern") o.pattern = next();
            else if (a == "--csv") o.csv = next();
            else if (a == "--quiet") o.quiet = true;
            else if (a == "--full") full = true;
            else if (a == "--opt") {
                const std::string kv = next();
                const auto eq = kv.find('=');
                if (eq == std::string::npos) throw std::runtime_error("--opt needs key=value");
                o.enc.options.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
            } else {
                throw std::runtime_error("unknown option " + a);
            }
        }
        if (o.source != "wgc" && o.source != "dda") throw std::runtime_error("--source must be wgc or dda");
        if (!o.pattern.empty() && o.pattern != "window" && o.pattern != "full")
            throw std::runtime_error("--pattern must be window or full");

        if (cmd == "list") return cmdList();
        if (cmd == "run") return cmdRun(o);
        if (cmd == "pattern") {
            const auto monitors = enumerateMonitors();
            return runPattern(pickMonitor(monitors, o.monitor), full, o.seconds);
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
