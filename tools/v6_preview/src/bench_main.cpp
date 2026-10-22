// dc-v6-bench: V6 measurement without a window.
//
//   dc-v6-bench [options]                    encode -> decode -> NV12->BGRA in one process; reports the
//                                            latency up to "frame ready in a shared texture"
//   dc-v6-bench --send HOST:PORT [options]   network sender for dc-v6-preview --listen PORT on another PC;
//                                            reports the full round trip including the remote display

#include "common.hpp"
#include "options.hpp"
#include "pipeline.hpp"
#include "stats.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

using namespace v6;

namespace {

void usage() {
    std::cout << "usage: dc-v6-bench [--send HOST:PORT] [options]\n" << kCommonUsage;
}

void printCpu(std::int64_t cpu0, std::int64_t wall0) {
    const double cpu = static_cast<double>(processCpuUs() - cpu0);
    const double wall = static_cast<double>(nowUs() - wall0);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::printf("process CPU: %.1f%% of one core, %.2f%% of all %lu logical cores\n", 100.0 * cpu / wall,
                100.0 * cpu / wall / si.dwNumberOfProcessors, static_cast<unsigned long>(si.dwNumberOfProcessors));
}

int runLocal(const CliOptions& o) {
    Stats stats(Converted, static_cast<std::uint64_t>(o.warmup));
    ReceiverPipeline rx(o.pipe, stats, nullptr, nullptr);
    SenderPipeline tx(o.pipe, stats, [&](EncodedUnit&& u) { rx.push(std::move(u)); });
    tx.start();
    std::printf("adapter : %s\nencoder : %s%s\ndecoder : %s\nstream  : %dx%d @ %d fps, %d kbps\n",
                rx.adapterName().c_str(), tx.encoderName().c_str(), tx.encoderIsHardware() ? " [hardware]" : "",
                rx.decoderName().c_str(), o.pipe.width, o.pipe.height, o.pipe.fps, o.pipe.kbps);

    const std::int64_t cpu0 = processCpuUs(), wall0 = nowUs();
    const double seconds = o.seconds > 0 ? o.seconds : 20;
    for (int i = 0; i < static_cast<int>(seconds * 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if ((i & 3) == 3) std::printf("  %s\n", stats.liveLine().c_str());
        if (!rx.error().empty()) break;
    }
    tx.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rx.stop();

    std::cout << stats.report(o.label.empty() ? "dc-v6-bench local (gen -> shared texture)" : o.label);
    printCpu(cpu0, wall0);
    std::cout << rx.probeSummary() << "\n";
    if (!o.csv.empty()) stats.writeCsv(o.csv);
    if (!rx.error().empty()) {
        std::cout << "RESULT: ERROR " << rx.error() << "\n";
        return 1;
    }
    return 0;
}

int runSender(const CliOptions& o) {
    std::string host;
    int port = 0;
    if (!splitHostPort(o.sendTo, host, port)) {
        std::cerr << "--send expects HOST:PORT\n";
        return 2;
    }
    Stats stats(Presented, static_cast<std::uint64_t>(o.warmup));
    TcpSender link(host, port, [&](const AckTimes& a) {
        const std::int64_t now = nowUs();
        const std::int64_t t0 = stats.stamp(a.id, Gen), t1 = stats.stamp(a.id, Encoded);
        if (t0 < 0 || t1 < 0) return;
        FrameTiming ft;
        ft.id = a.id;
        ft.step.fill(-1);
        ft.step[Encoded] = t1 - t0;
        ft.step[Decoded] = a.decode;
        ft.step[Converted] = a.convert;
        ft.step[Copied] = a.copy;
        ft.step[Presented] = a.present;
        ft.total = now - t0;
        // What is left is the network both ways plus queueing before the decoder.
        ft.step[Received] = ft.total - ft.step[Encoded] - a.decode - a.convert - a.copy - a.present;
        stats.addTiming(ft);
    });
    SenderPipeline tx(o.pipe, stats, [&](EncodedUnit&& u) { link.send(u); });
    tx.start();
    std::printf("sending to %s:%d\nencoder : %s%s\nstream  : %dx%d @ %d fps, %d kbps\n", host.c_str(), port,
                tx.encoderName().c_str(), tx.encoderIsHardware() ? " [hardware]" : "", o.pipe.width, o.pipe.height,
                o.pipe.fps, o.pipe.kbps);
    const double seconds = o.seconds > 0 ? o.seconds : 20;
    for (int i = 0; i < static_cast<int>(seconds * 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if ((i & 3) == 3) std::printf("  %s\n", stats.liveLine().c_str());
    }
    tx.stop();
    std::cout << stats.report(o.label.empty() ? "dc-v6-bench network round trip (gen -> remote present -> ack)"
                                              : o.label);
    std::cout << "note: 'transfer' = network both ways + queueing; the remote screen adds about one refresh more.\n";
    if (!o.csv.empty()) stats.writeCsv(o.csv);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions o;
    if (!parseCli(argc, argv, o)) {
        usage();
        return 2;
    }
    if (o.help) {
        usage();
        return 0;
    }
    installCrashReporter();
    try {
        MfRuntime mf;
        return o.sendTo.empty() ? runLocal(o) : runSender(o);
    } catch (const std::exception& e) {
        std::cerr << "RESULT: ERROR " << e.what() << "\n";
        return 1;
    }
}
