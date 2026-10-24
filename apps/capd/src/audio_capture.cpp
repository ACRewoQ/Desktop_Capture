#include "audio_capture.hpp"

#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/layout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dc::capd {

namespace {

constexpr std::uint32_t kChunk = 1024;

clocksync::DriftController::Config controllerConfig() {
    clocksync::DriftController::Config c;
    c.nominalRate = shm::kSampleRate;
    return c;
}

}  // namespace

AudioCapture::AudioCapture(std::vector<std::uint32_t> slots, std::string shmSuffix)
    : slots_(std::move(slots)), shmSuffix_(std::move(shmSuffix)), controller_(controllerConfig()) {
    freq_ = shm::qpcFrequency();
    resamplers_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i)
        resamplers_.emplace_back(2, clocksync::VariableResampler::Quality{}, std::size_t{kChunk} * 2);
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
    controller_.reset();
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

// Finds the engine frame recorded at the QPC time of output frame position_. Returns it (negative when that time is
// before the engine's first frame); readFrame_ is set to it when it is not negative.
std::int64_t AudioCapture::sync(std::int64_t nowQpc) {
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
    if (engineFrame < 0) return engineFrame;
    readFrame_ = static_cast<std::uint64_t>(engineFrame);
    // The resamplers restart with silence in front, so their next output frame is exactly readFrame_. The learned
    // clock difference is kept: it belongs to the engine's device, which a stall does not change.
    for (auto& r : resamplers_) r.reset();
    controller_.reset(true);
    needsSync_ = false;
    needsPrime_ = true;
    std::lock_guard lock(statsMutex_);
    ++stats_.resyncs;
    return engineFrame;
}

std::optional<double> AudioCapture::alignmentError() const {
    // Engine position (fractional) of the next output frame: the resamplers trail the frames handed to them.
    const double pos = double(readFrame_) - resamplers_.front().bufferedFrames();
    if (pos < 0) return std::nullopt;
    const auto whole = static_cast<std::uint64_t>(pos);
    const auto q = reader_->qpcForFrame(whole);
    if (!q) return std::nullopt;
    const double engineQpc = double(*q) + (pos - double(whole)) * double(freq_) / shm::kSampleRate;
    const double wantQpc = double(startQpc_) + double(position_) * double(freq_) / shm::kSampleRate;
    return (wantQpc - engineQpc) * shm::kSampleRate / double(freq_);
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
        bool justSynced = false;
        if (needsSync_) {
            const std::int64_t at = sync(nowQpc);
            if (at < 0) {
                // The recording is ahead of the engine's first frame: silence until it starts.
                emitSilence(std::min<std::uint64_t>(static_cast<std::uint64_t>(-at), want));
                continue;
            }
            justSynced = true;
        }

        const std::uint64_t written = reader_->header()->framesWritten.load(std::memory_order_acquire);
        const auto engineFrame = static_cast<std::int64_t>(readFrame_);
        if (engineFrame > static_cast<std::int64_t>(written) + std::int64_t{shm::kSampleRate}) {
            // Nonsense mapping (anchors from a previous engine run): re-align on the next pass.
            needsSync_ = true;
            emitSilence(std::min<std::uint64_t>(want, kChunk));
            continue;
        }
        if (engineFrame < static_cast<std::int64_t>(written) - std::int64_t{shm::kCapacityFrames / 2}) {
            // Older than the ring still holds safely (startup far in the past, or a long stall): skip with silence
            // and re-align at the new position.
            const auto skip = static_cast<std::uint64_t>(written - shm::kCapacityFrames / 2 - engineFrame);
            emitSilence(std::min<std::uint64_t>(skip, want));
            needsSync_ = true;
            continue;
        }

        if (needsPrime_) {
            // Fill the resamplers' history with the audio before readFrame_ instead of the silence they restart
            // with, so a re-alignment does not ring: hand them halfTaps frames before and after it and throw away
            // the first halfTaps outputs (ratio 1). Their next output is then readFrame_ again.
            const std::size_t h = static_cast<std::size_t>(resamplers_.front().halfTaps());
            if (readFrame_ >= h && engineFrame - static_cast<std::int64_t>(h) >=
                                       static_cast<std::int64_t>(written) - std::int64_t{shm::kCapacityFrames / 2}) {
                reader_->seek(readFrame_ - h);
                if (reader_->available() < 2 * h) break;  // not committed yet
                bool ok = true;
                for (std::size_t i = 0; i < slots_.size() && ok; ++i) {
                    float* in = resamplers_[i].inputWritePointer(2 * h);
                    const auto info = reader_->read(slots_[i], std::span<float>(in, 4 * h), static_cast<std::uint32_t>(2 * h), false);
                    ok = info.status == shm::AudioRingReader::Status::Ok && info.frames == 2 * h;
                    if (ok && reader_->slotChannels(slots_[i]) == 1)
                        for (std::size_t k = 0; k < 2 * h; ++k) in[k * 2 + 1] = in[k * 2];
                }
                if (!ok) {
                    needsSync_ = true;
                    continue;
                }
                for (std::size_t i = 0; i < slots_.size(); ++i) {
                    resamplers_[i].commitInput(2 * h);
                    resamplers_[i].produce(buffers_[i].data(), h, 1.0);
                }
                readFrame_ += h;
            }
            needsPrime_ = false;
        }

        // Clock correction: the alignment error drives the resampling ratio; a jump re-aligns instead.
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(want, kChunk));
        double ratio = controller_.ratio();
        if (const auto err = alignmentError()) {
            const double errMs = *err * 1000.0 / shm::kSampleRate;
            {
                std::lock_guard lock(statsMutex_);
                stats_.driftMs = -errMs;
            }
            if (std::fabs(errMs) > kMaxDriftMs && !justSynced) {
                needsSync_ = true;
                continue;
            }
            if (std::fabs(errMs) <= kMaxDriftMs) ratio = controller_.update(*err, double(n) / shm::kSampleRate);
        }
        const std::size_t need = resamplers_.front().inputFramesRequired(n, ratio);
        reader_->seek(readFrame_);
        if (reader_->available() < need) break;  // the engine is alive but has not committed this far yet

        bool retry = false;
        for (std::size_t i = 0; i < slots_.size() && !retry && need > 0; ++i) {
            float* in = resamplers_[i].inputWritePointer(need);
            auto info = reader_->read(slots_[i], std::span<float>(in, need * 2), static_cast<std::uint32_t>(need), false);
            if (info.status == shm::AudioRingReader::Status::WriterRestarted) {
                needsSync_ = true;
                retry = true;
            } else if (info.status == shm::AudioRingReader::Status::Overrun) {
                {
                    std::lock_guard lock(statsMutex_);
                    ++stats_.overruns;
                }
                emitSilence(std::min<std::uint64_t>(info.skippedFrames, want));
                needsSync_ = true;
                retry = true;
            } else if (info.frames != need) {
                retry = true;  // raced with the writer; read again (nothing was committed yet)
            } else if (reader_->slotChannels(slots_[i]) == 1) {
                for (std::size_t k = 0; k < need; ++k) in[k * 2 + 1] = in[k * 2];
            }
        }
        if (retry) continue;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            resamplers_[i].commitInput(need);
            resamplers_[i].produce(buffers_[i].data(), n, ratio);
        }
        readFrame_ += need;
        {
            std::lock_guard lock(statsMutex_);
            stats_.ratio = ratio;
        }
        emit(n);
    }

    std::lock_guard lock(statsMutex_);
    stats_.engineConnected = connected || (reader_ && reader_->writerAlive(nowQpc, freq_ / 5));
    return position_ - before;
}

}  // namespace dc::capd
