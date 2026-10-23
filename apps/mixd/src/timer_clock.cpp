#include "device.hpp"
#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace dc::mixd {

namespace {

class TimerClock final : public Stream {
public:
    TimerClock(int blockFrames, OutputCallback cb) : block_(std::clamp(blockFrames, 16, kMaxBlockFrames)), cb_(std::move(cb)) {}
    ~TimerClock() override { stop(); }

    bool start(std::string*) override {
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    int channels() const override { return 0; }
    int periodFrames() const override { return block_; }
    double latencySeconds() const override { return 0; }
    bool failed() const override { return false; }
    std::uint64_t glitches() const override { return 0; }

private:
    void run() {
#ifdef _WIN32
        timeBeginPeriod(1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
        const double t0 = qpcSecondsNow();
        std::uint64_t rendered = 0;
        while (!stop_) {
            const double now = qpcSecondsNow();
            auto due = static_cast<std::uint64_t>((now - t0) * kSampleRate);
            if (due > rendered + 48000) rendered = due - static_cast<std::uint64_t>(block_); // stalled: skip ahead
            while (due >= rendered + static_cast<std::uint64_t>(block_) && !stop_) {
                cb_(nullptr, block_, 0, t0 + static_cast<double>(rendered) / kSampleRate);
                rendered += static_cast<std::uint64_t>(block_);
            }
            const double next = t0 + static_cast<double>(rendered + static_cast<std::uint64_t>(block_)) / kSampleRate;
            const double wait = next - qpcSecondsNow();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.05)));
        }
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }

    int block_;
    OutputCallback cb_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<Stream> makeTimerClock(int blockFrames, OutputCallback cb) {
    return std::make_unique<TimerClock>(blockFrames, std::move(cb));
}

} // namespace dc::mixd
