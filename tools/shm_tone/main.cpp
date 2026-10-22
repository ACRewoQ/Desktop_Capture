// dc-shm-tone: test writer / reader for the engine -> capture shared audio memory.
//
//   dc-shm-tone write [--seconds N]   act as the engine: sine tones into master and tracks 1-4
//   dc-shm-tone read  [--seconds N]   act as capd: read master and tracks 1-4, print levels
//
// Lane B uses "write" as the dummy audio source until mixd writes real audio (see the plan).

#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/shared_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static std::uint32_t currentPid() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
static std::uint32_t currentPid() { return static_cast<std::uint32_t>(getpid()); }
#endif

using namespace dc::shm;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kBlock = 256;

int runWriter(double seconds) {
    std::string err;
    auto mem = SharedMemory::create(kLayout.totalBytes, "", &err);
    if (!mem.valid()) {
        std::cerr << "create failed: " << err << "\n";
        return 1;
    }
    AudioRingWriter writer(mem.data(), currentPid());
    writer.configureSlot(kMasterSlot, 2, "master");
    for (std::uint32_t t = 1; t <= 4; ++t)
        writer.configureSlot(t, t == 1 ? 1 : 2, ("track/" + std::to_string(t)).c_str());

    std::cout << "writing tones: master 440 Hz, track N = 220*(N+1) Hz (" << kLayout.totalBytes / (1024 * 1024)
              << " MiB mapping). Ctrl+C to stop.\n";

    std::vector<float> buf(kBlock * 2);
    const auto start = std::chrono::steady_clock::now();
    const auto blockDuration = std::chrono::duration<double>(double(kBlock) / kSampleRate);
    std::uint64_t frame = 0;
    for (std::uint64_t block = 0;; ++block) {
        const auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(blockDuration * block);
        std::this_thread::sleep_until(due);
        if (seconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::duration<double>(seconds)) break;

        for (std::uint32_t slot = 0; slot <= 4; ++slot) {
            const double freq = slot == 0 ? 440.0 : 220.0 * (slot + 1);
            const std::uint32_t ch = slot == 1 ? 1 : 2;
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * freq * double(frame + i) / kSampleRate));
                for (std::uint32_t c = 0; c < ch; ++c) buf[i * ch + c] = s;
            }
            writer.write(slot, buf.data(), kBlock, ch);
        }
        writer.commit(kBlock, nowQpc());
        frame += kBlock;
    }
    return 0;
}

int runReader(double seconds) {
    std::string err;
    SharedMemory mem;
    while (!(mem = SharedMemory::open(kLayout.totalBytes, "", &err)).valid()) {
        std::cerr << "waiting for writer (" << err << ")\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    auto reader = AudioRingReader::attach(mem.data(), mem.size(), &err);
    if (!reader) {
        std::cerr << "attach failed: " << err << "\n";
        return 1;
    }
    std::vector<float> buf(4096 * 2);
    std::vector<float> peaks(5, 0.0f);
    std::uint64_t frames = 0, overruns = 0, restarts = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastPrint = start;
    while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto avail = static_cast<std::uint32_t>(std::min<std::uint64_t>(reader->available(), 4096));
        std::uint32_t got = 0;
        for (std::uint32_t slot = 0; slot <= 4; ++slot) {
            const auto info = reader->read(slot, buf, avail, false);
            if (info.status == AudioRingReader::Status::Overrun) ++overruns;
            if (info.status == AudioRingReader::Status::WriterRestarted) ++restarts;
            got = info.frames;
            for (std::uint32_t i = 0; i < info.frames * 2; ++i) peaks[slot] = std::max(peaks[slot], std::fabs(buf[i]));
        }
        reader->advance(got);
        frames += got;

        if (std::chrono::steady_clock::now() - lastPrint > std::chrono::milliseconds(500)) {
            lastPrint = std::chrono::steady_clock::now();
            const bool alive = reader->writerAlive(nowQpc(), qpcFrequency() / 5);
            std::printf("frames %llu  overruns %llu  restarts %llu  writer %s  peaks:",
                        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(overruns),
                        static_cast<unsigned long long>(restarts), alive ? "alive" : "STALLED");
            for (std::uint32_t s = 0; s <= 4; ++s) std::printf(" %u=%.2f", s, peaks[s]);
            // Age of the newest frame read: how far the reader is behind the engine clock.
            if (reader->position() > 0)
                if (auto q = reader->qpcForFrame(reader->position() - 1)) {
                    const double ageMs = double(nowQpc() - *q) * 1000.0 / double(qpcFrequency());
                    std::printf("  age %.1f ms", ageMs);
                }
            std::printf("\n");
            std::fill(peaks.begin(), peaks.end(), 0.0f);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dc-shm-tone write|read [--seconds N]\n";
        return 2;
    }
    double seconds = 0;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--seconds") seconds = std::stod(argv[i + 1]);
    const std::string mode = argv[1];
    if (mode == "write") return runWriter(seconds);
    if (mode == "read") return runReader(seconds);
    std::cerr << "usage: dc-shm-tone write|read [--seconds N]\n";
    return 2;
}
