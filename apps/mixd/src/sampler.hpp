#pragma once

// Sampler: 8 banks x 16 sound effect pads. Decoded pad audio (48 kHz stereo) lives in SampleData
// blocks shared by the mix plans (SamplerTable). The audio thread plays voices from the table of the
// plan it runs; pad presses arrive through a lock-free command queue. Each bank renders into its own
// stereo bus, and tracks whose input is the sampler mix one bank or all of them.
//
// A voice refers to the sample of the table it was started from. When a new plan carries a
// different sample for that pad (the file was replaced), the voice stops at adoption, before the
// old plan can be freed.

#include "plan.hpp"

#include "dc/clocksync/audio_fifo.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dc::mixd {

inline constexpr int kSamplerBanks = 8;
inline constexpr int kSamplerPadsPerBank = 16;
inline constexpr int kSamplerPadCount = kSamplerBanks * kSamplerPadsPerBank;
inline constexpr int kSamplerVoices = 32;
inline constexpr int kSamplerFadeFrames = 240; // 5 ms fade when a voice stops or restarts

enum class PadMode : std::uint8_t { OneShot, Toggle, Hold, Loop };
PadMode padModeFromString(const std::string& s);

struct SampleData {
    std::vector<float> samples; // interleaved stereo, 48 kHz
    std::size_t frames = 0;
};

struct PadPlan {
    std::shared_ptr<const SampleData> sample; // null = empty or not loaded
    PadMode mode = PadMode::OneShot;
    float gain = 1;
};

struct SamplerTable {
    std::array<PadPlan, kSamplerPadCount> pads; // index = (bank - 1) * 16 + (pad - 1)
};

inline int padIndex(int bank, int pad) { return (bank - 1) * kSamplerPadsPerBank + (pad - 1); }

class Sampler {
public:
    struct Command {
        enum class Type : std::uint8_t { Trigger, Release, StopAll, StopBank };
        Type type = Type::Trigger;
        std::int16_t index = 0; // pad index (Trigger / Release) or bank 1..8 (StopBank)
    };

    Sampler();

    // Control thread (single producer). False when the queue is full.
    bool post(Command c) { return queue_.push(c); }
    // Any thread: pads with a sounding voice, bit i = pad index i.
    std::array<std::uint64_t, 2> playing() const {
        return {playing_[0].load(std::memory_order_relaxed), playing_[1].load(std::memory_order_relaxed)};
    }

    // Audio thread.
    void adopt(const SamplerTable* table);
    void render(int frames);
    // 0 = all banks, 1..8 = one bank.
    const float* bus(int bank, int channel) const { return (*buses_)[static_cast<std::size_t>(bank)][static_cast<std::size_t>(channel)].data(); }

private:
    struct Voice {
        bool active = false;
        int pad = 0;
        const SampleData* sample = nullptr;
        std::size_t pos = 0;
        bool loop = false;
        bool hold = false;
        int fade = 0; // > 0: fading out, frames left
        float gain = 1;
        std::uint64_t started = 0;
    };

    void handle(const Command& c);
    void start(int pad);
    void fadeOut(Voice& v);
    Voice* sounding(int pad); // the voice of pad that is not fading out

    using Buffer = std::array<float, kMaxBlockFrames>;
    clocksync::SpscQueue<Command, 256> queue_;
    const SamplerTable* table_ = nullptr;
    std::array<Voice, kSamplerVoices> voices_{};
    std::unique_ptr<std::array<std::array<Buffer, 2>, kSamplerBanks + 1>> buses_;
    std::uint64_t counter_ = 0;
    std::atomic<std::uint64_t> playing_[2] = {0, 0};
};

// Decodes pad files on its own thread. Results are handed to the callback on that thread.
class SampleLoader {
public:
    struct Result {
        int pad = 0;
        std::string file;
        std::uint64_t serial = 0;
        std::shared_ptr<const SampleData> sample; // null on error
        std::string error;
    };
    using Callback = std::function<void(Result)>;

    explicit SampleLoader(Callback callback);
    ~SampleLoader();
    SampleLoader(const SampleLoader&) = delete;
    SampleLoader& operator=(const SampleLoader&) = delete;

    void load(int pad, std::string file, std::uint64_t serial);

private:
    struct Request {
        int pad;
        std::string file;
        std::uint64_t serial;
    };
    void run();

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace dc::mixd
