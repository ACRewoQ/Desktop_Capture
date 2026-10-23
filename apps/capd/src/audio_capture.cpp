#include "audio_capture.hpp"

#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/layout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dc::capd {

namespace {

constexpr std::uint32_t kChunk = 1024;
// More drift than this between the engine's sample clock and QPC re-aligns the reader (a short skip or repeat).
constexpr double kMaxDriftMs = 40.0;

}  // namespace

AudioCapture::AudioCapture(std::vector<std::uint32_t> slots, std::string shmSuffix)
    : slots_(std::move(slots)), shmSuffix_(std::move(shmSuffix)) {
    freq_ = shm::qpcFrequency();
    buffers_.assign(slots_.size(), std::vector<float>(kChunk * 2, 0.0f));
    views_.resize(slots_.size());
}

AudioCapture::~AudioCapture() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::begin(std::int64_t startQpc, Sink sink) {
    startQpc_ = startQpc;
    sink_ = std::move(sink);
    position_ = 0;
    needsSync_ = true;
    nextAttachQpc_ = 0;
    nextDriftCheckQpc_ = startQpc + freq_;
}

void AudioCapture::start(std::int64_t startQpc, Sink sink) {
    begin(startQpc, std::move(sink));
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void AudioCapture::stop(std::int64_t endQpc) {
    endQpc_ = endQpc;
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

AudioCapture::Stats AudioCapture::stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

void AudioCapture::run() {
    while (!stopping_) {
        pump(shm::nowQpc(), kLagFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Finish at endQpc: give the engine up to 250 ms to commit the last blocks, then pad with silence.
    const std::int64_t end = endQpc_.load();
    if (end <= startQpc_) return;
    const auto target = static_cast<std::uint64_t>(framesAt(end));
    const std::int64_t giveUp = shm::nowQpc() + freq_ / 4;
    while (position_ < target && shm::nowQpc() < giveUp) {
        const std::int64_t now = std::min(shm::nowQpc(), end);
        pump(now, 0);
        if (position_ < target) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (shm::nowQpc() - end > 0 && reader_ && !reader_->writerAlive(shm::nowQpc(), freq_ / 5)) break;
    }
    if (position_ < target) emitSilence(target - position_);
}

std::int64_t AudioCapture::framesAt(std::int64_t qpc) const {
    const double seconds = static_cast<double>(qpc - startQpc_) / static_cast<double>(freq_);
    return static_cast<std::int64_t>(std::floor(seconds * shm::kSampleRate));
}

bool AudioCapture::ensureAttached(std::int64_t nowQpc) {
    if (reader_) {
        if (reader_->writerAlive(nowQpc, freq_ / 5)) {
            lastAliveQpc_ = nowQpc;
            return true;
        }
        // Silent for over a second: open the mapping by name again. On Windows that is the same mapping (the
        // restarted engine reuses it while we hold it); on POSIX a restarted writer makes a new object.
        if (nowQpc - lastAliveQpc_ < freq_ || nowQpc < nextAttachQpc_) return true;
        reader_.reset();
        memory_.reset();
    }
    if (nowQpc < nextAttachQpc_) return false;
    nextAttachQpc_ = nowQpc + freq_ / 4;  // the engine may start later: retry four times a second
    std::string err;
    auto mem = shm::SharedMemory::open(shm::kLayout.totalBytes, shmSuffix_, &err);
    if (!mem.valid()) return false;
    auto reader = shm::AudioRingReader::attach(mem.data(), mem.size(), &err);
    if (!reader) return false;
    memory_ = std::move(mem);
    reader_ = std::move(reader);
    lastAliveQpc_ = nowQpc;
    needsSync_ = true;
    return true;
}

// Chooses offset_ so that output frame position_ maps to the engine frame recorded at the same QPC time.
void AudioCapture::sync(std::int64_t nowQpc) {
    const std::uint64_t written = reader_->header()->framesWritten.load(std::memory_order_acquire);
    const std::int64_t wantQpc =
        startQpc_ + static_cast<std::int64_t>(static_cast<double>(position_) * double(freq_) / shm::kSampleRate);
    std::int64_t engineFrame = static_cast<std::int64_t>(written);
    if (written > 0) {
        if (auto q = reader_->qpcForFrame(written - 1)) {
            const double back = static_cast<double>(*q - wantQpc) * shm::kSampleRate / double(freq_);
            engineFrame = static_cast<std::int64_t>(written - 1) - static_cast<std::int64_t>(std::llround(back));
        } else {
            // No anchors yet: assume the newest frame is "now".
            const double back = static_cast<double>(nowQpc - wantQpc) * shm::kSampleRate / double(freq_);
            engineFrame = static_cast<std::int64_t>(written) - static_cast<std::int64_t>(std::llround(back));
        }
    }
    offset_ = engineFrame - static_cast<std::int64_t>(position_);
    needsSync_ = false;
    std::lock_guard lock(statsMutex_);
    ++stats_.resyncs;
}

void AudioCapture::checkDrift(std::int64_t nowQpc) {
    if (nowQpc < nextDriftCheckQpc_ || position_ == 0) return;
    nextDriftCheckQpc_ = nowQpc + freq_;
    const std::int64_t engineFrame = offset_ + static_cast<std::int64_t>(position_) - 1;
    if (engineFrame < 0) return;
    const auto q = reader_->qpcForFrame(static_cast<std::uint64_t>(engineFrame));
    if (!q) return;
    const std::int64_t wantQpc = startQpc_ + static_cast<std::int64_t>(static_cast<double>(position_ - 1) *
                                                                       double(freq_) / shm::kSampleRate);
    const double driftMs = static_cast<double>(*q - wantQpc) * 1000.0 / double(freq_);
    {
        std::lock_guard lock(statsMutex_);
        stats_.driftMs = driftMs;
    }
    if (std::fabs(driftMs) > kMaxDriftMs) needsSync_ = true;
}

void AudioCapture::emit(std::uint32_t frames) {
    for (std::size_t i = 0; i < buffers_.size(); ++i) views_[i] = std::span<const float>(buffers_[i].data(), frames * 2);
    if (sink_) sink_(views_, frames);
    position_ += frames;
    std::lock_guard lock(statsMutex_);
    stats_.frames = position_;
}

void AudioCapture::emitSilence(std::uint64_t frames) {
    for (auto& b : buffers_) std::fill(b.begin(), b.end(), 0.0f);
    {
        std::lock_guard lock(statsMutex_);
        stats_.silenceFrames += frames;
    }
    while (frames > 0) {
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, kChunk));
        emit(n);
        frames -= n;
    }
}

std::uint64_t AudioCapture::pump(std::int64_t nowQpc, std::uint32_t lagFrames) {
    const std::int64_t target = framesAt(nowQpc) - static_cast<std::int64_t>(lagFrames);
    const std::uint64_t before = position_;
    bool connected = false;

    while (static_cast<std::int64_t>(position_) < target) {
        const auto want = static_cast<std::uint64_t>(target - static_cast<std::int64_t>(position_));
        if (!ensureAttached(nowQpc) || !reader_->writerAlive(nowQpc, freq_ / 5)) {
            // Engine not running or stalled: keep the time line going with silence; re-align when it is back.
            emitSilence(want);
            needsSync_ = true;
            break;
        }
        connected = true;
        if (needsSync_) sync(nowQpc);

        const std::uint64_t written = reader_->header()->framesWritten.load(std::memory_order_acquire);
        std::int64_t engineFrame = offset_ + static_cast<std::int64_t>(position_);
        if (engineFrame < 0 || engineFrame > static_cast<std::int64_t>(written) + std::int64_t{shm::kSampleRate}) {
            // Nonsense mapping (anchors from a previous engine run): re-align on the next pass.
            needsSync_ = true;
            emitSilence(std::min<std::uint64_t>(want, kChunk));
            continue;
        }
        if (engineFrame < static_cast<std::int64_t>(written) - std::int64_t{shm::kCapacityFrames / 2}) {
            // Older than the ring still holds safely (startup far in the past, or a long stall): skip with silence.
            const auto skip = static_cast<std::uint64_t>(written - shm::kCapacityFrames / 2 - engineFrame);
            emitSilence(std::min<std::uint64_t>(skip, want));
            continue;
        }

        reader_->seek(static_cast<std::uint64_t>(engineFrame));
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>({want, reader_->available(), kChunk}));
        if (n == 0) break;  // the engine is alive but has not committed this far yet

        bool retry = false;
        for (std::size_t i = 0; i < slots_.size() && !retry; ++i) {
            auto info = reader_->read(slots_[i], buffers_[i], n, false);
            if (info.status == shm::AudioRingReader::Status::WriterRestarted) {
                needsSync_ = true;
                retry = true;
            } else if (info.status == shm::AudioRingReader::Status::Overrun) {
                {
                    std::lock_guard lock(statsMutex_);
                    ++stats_.overruns;
                }
                emitSilence(std::min<std::uint64_t>(info.skippedFrames, want));
                retry = true;
            } else if (info.frames != n) {
                retry = true;  // raced with the writer; read again
            } else if (reader_->slotChannels(slots_[i]) == 1) {
                auto& b = buffers_[i];
                for (std::uint32_t k = 0; k < n; ++k) b[k * 2 + 1] = b[k * 2];
            }
        }
        if (retry) continue;
        emit(n);
        checkDrift(nowQpc);
    }

    std::lock_guard lock(statsMutex_);
    stats_.engineConnected = connected || (reader_ && reader_->writerAlive(nowQpc, freq_ / 5));
    return position_ - before;
}

}  // namespace dc::capd
