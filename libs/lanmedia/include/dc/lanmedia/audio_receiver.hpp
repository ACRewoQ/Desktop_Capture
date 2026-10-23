#pragma once

#include "dc/clocksync/audio_fifo.hpp"
#include "dc/clocksync/drift_controller.hpp"
#include "dc/clocksync/resampler.hpp"
#include "dc/lanmedia/arrival_clock.hpp"
#include "dc/lanmedia/audio_codec.hpp"
#include "dc/lanmedia/packet.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace dc::lan {

// Receiving end of one audio stream: jitter buffer, loss concealment and clock drift correction in one
// place, so the stream plays at a constant, small latency on a device whose clock is not the sender's.
//
//   network thread  onPacket(datagram, arrival time)  -> lock-free queue
//   audio thread    pull(out, frames, now)            <- queue -> reorder slots -> decoder/concealment
//                                                        -> variable-rate resampler -> out
//
// Buffer level ("fill") = source frames that should have arrived by now (ArrivalClock: lower envelope
// of the arrival times) minus the source position being played. A PI loop (clocksync::DriftController,
// as in V2) adjusts the resampling ratio to hold the fill at the target, which absorbs the clock
// difference between the PCs. The target is either fixed or follows the measured arrival lateness
// (auto). A packet that is not there when its turn comes is concealed (PCM: waveform repetition with a
// fade; Opus: FEC from the next packet, else the codec's PLC) and dropped if it arrives later.
//
// Threads: onPacket() on one network thread, pull() on the audio thread, stats() and makeReport()
// anywhere. pull() and onPacket() do not allocate, except pull() when a new stream with a different
// format starts (creates the decoder once).
class AudioReceiver {
public:
    struct Config {
        int channels = 2;          // output channels (a mono stream is duplicated, stereo is averaged to mono)
        double nominalRate = 48000;
        std::uint32_t streamId = 0; // 0 = take the first stream seen; a packet flagged StreamStart switches streams
        double targetMs = 0;       // buffer level to hold; 0 = auto (consumer block + lateness peak + margin)
        double minTargetMs = 1;
        double maxTargetMs = 200;
        double marginMs = 1.0;     // auto: added to the measured need
        double startSeconds = 0.1; // packets to watch before playback starts (first arrival estimate)
        double resyncMs = 300;     // fill error that restarts playback
        double concealFadeMs = 10; // PCM: concealment fades to silence over this time
        std::size_t maxBlockFrames = 4096; // largest pull() block
        clocksync::DriftController::Config controller{};
        clocksync::VariableResampler::Quality quality{};
        ArrivalClock::Config arrival{};
    };

    enum class State : int { Idle = 0, Waiting = 1, Running = 2 };

    struct Stats {
        State state = State::Idle;
        std::uint32_t streamId = 0;
        AudioCodec codec = AudioCodec::Pcm24;
        int streamChannels = 0;
        std::size_t packetFrames = 0;
        double ratio = 1;          // source frames per output frame
        double fillMs = 0;         // buffer level at the last pull
        double targetMs = 0;
        double latenessPeakMs = 0; // arrival jitter (peak over the last seconds)
        double playPosition = 0;   // source frame position of the next output frame
        std::uint64_t received = 0;   // datagrams accepted (duplicates excluded)
        std::uint64_t lost = 0;       // sequence numbers never seen
        std::uint64_t late = 0;       // arrived after their turn
        std::uint64_t duplicates = 0;
        std::uint64_t concealed = 0;  // packets replaced by concealment (PCM repetition / Opus PLC)
        std::uint64_t fecRecovered = 0; // Opus packets rebuilt from the next packet's FEC data
        std::uint64_t insertions = 0; // buffer grown by repeating a short waveform segment
        std::uint64_t skips = 0;      // buffer shrunk by dropping audio
        std::uint64_t resyncs = 0;    // playback restarted (timing lost)
        std::uint64_t streamChanges = 0;
        std::uint64_t rejected = 0;   // malformed datagrams, other streams, queue full
        std::uint64_t outputFrames = 0;
        bool locked = false;
    };

    explicit AudioReceiver(Config config);
    ~AudioReceiver();

    // Network thread: a datagram arrived at `arrival` (seconds, receiver clock shared with pull()).
    // Returns false if it was not accepted (not an audio media packet, or the queue is full).
    bool onPacket(std::span<const std::uint8_t> datagram, double arrival);

    // Audio thread: fills `frames` output frames. now = time of this block on the receiver clock.
    void pull(float* out, std::size_t frames, double now);

    Stats stats() const;
    // Any thread: a report for the sender. now = receiver clock, seconds.
    Report makeReport(double now) const;

    const Config& config() const { return cfg_; }

private:
    struct RawPacket {
        double arrival;
        std::uint16_t size;
        std::uint8_t data[kMaxDatagramBytes];
    };
    static constexpr std::size_t kQueue = 256;
    static constexpr std::size_t kSlots = 128;

    struct Slot {
        bool used = false;
        std::uint64_t pos = 0;
        std::uint16_t size = 0;
        std::uint8_t data[kMaxMediaPayloadBytes];
    };

    void handle(const RawPacket& raw);
    void beginStream(std::uint32_t id, const AudioHeader& fmt, std::uint64_t pos);
    void resync();
    bool start(double now);
    double currentTarget(double now) const;
    double readPosition() const;
    void feedResampler(std::size_t frames);
    void decodeNext();
    void concealPcm(float* dst, std::size_t frames);
    void toOutputChannels(const float* src, float* dst);
    void insertFrames(std::size_t frames);
    void skipFrames(std::size_t frames);
    Slot* slotFor(std::uint64_t pos);
    void publish();

    Config cfg_;
    std::size_t outCh_;
    std::unique_ptr<clocksync::SpscQueue<RawPacket, kQueue>> queue_;
    std::atomic<std::uint64_t> queueFull_{0};
    std::atomic<std::uint64_t> echoTimeUs_{0};
    std::atomic<double> echoArrival_{0};

    // audio thread
    State state_ = State::Idle;
    std::uint32_t streamId_ = 0;
    AudioHeader fmt_{};
    std::uint64_t grid_ = 0; // position % packet frames of the stream
    std::unique_ptr<AudioDecoder> decoder_;
    std::vector<Slot> slots_;
    ArrivalClock arrival_;
    clocksync::DriftController controller_;
    clocksync::VariableResampler resampler_;
    double firstArrival_ = 0;
    double lastPull_ = 0;
    std::size_t blockPeak_ = 0;

    std::uint64_t decodePos_ = 0;  // source position of the next packet to decode
    std::uint64_t srcEnd_ = 0;     // source position at the end of the resampler's input
    std::vector<float> staging_;   // decoded frames (output channels) not yet given to the resampler
    std::size_t stagingOff_ = 0, stagingLen_ = 0;
    std::size_t fadeIn_ = 0;       // frames of fade-in still to apply to staging (after a skip)
    std::vector<float> decodeBuf_, concealBuf_, lastPacket_;
    bool haveLast_ = false;
    std::size_t missRun_ = 0;      // consecutive concealed packets
    double concealGain_ = 1;
    std::vector<float> history_;   // last frames given to the resampler (output channels), ring
    std::size_t historyPos_ = 0, historyLen_ = 0;
    std::size_t badRun_ = 0;       // consecutive packets far outside the play window

    // sequence statistics
    bool haveSeq_ = false;
    SeqUnwrapper seq_;
    std::uint64_t firstSeq_ = 0, maxSeq_ = 0;

    std::uint64_t received_ = 0, late_ = 0, duplicates_ = 0, concealed_ = 0, fec_ = 0, insertions_ = 0,
                  skips_ = 0, resyncs_ = 0, streamChanges_ = 0, rejected_ = 0, outputFrames_ = 0;
    double fill_ = 0, target_ = 0;

    // published for stats()
    struct Published;
    std::unique_ptr<Published> pub_;
};

} // namespace dc::lan
