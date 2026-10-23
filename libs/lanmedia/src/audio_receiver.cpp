#include "dc/lanmedia/audio_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc::lan {

namespace {

constexpr std::size_t kHistoryFrames = 2048; // insertions repeat at most half of this at once
constexpr std::size_t kCrossfadeFrames = 96; // PCM: concealment -> real audio
constexpr std::size_t kSkipFadeFrames = 64;
constexpr std::size_t kBadRunResync = 8;     // consecutive packets far outside the play window

} // namespace

struct AudioReceiver::Published {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Stats stats;
};

AudioReceiver::AudioReceiver(Config config)
    : cfg_(config), outCh_(std::size_t(std::clamp(config.channels, 1, 2))),
      queue_(std::make_unique<clocksync::SpscQueue<RawPacket, kQueue>>()), slots_(kSlots),
      arrival_([&] {
          auto a = config.arrival;
          a.nominalRate = config.nominalRate;
          return a;
      }()),
      controller_([&] {
          auto c = config.controller;
          c.nominalRate = config.nominalRate;
          return c;
      }()),
      resampler_(int(std::clamp(config.channels, 1, 2)), config.quality, config.maxBlockFrames + kHistoryFrames),
      history_(kHistoryFrames * std::size_t(std::clamp(config.channels, 1, 2))), pub_(std::make_unique<Published>()) {
    cfg_.channels = int(outCh_);
    publish();
}

AudioReceiver::~AudioReceiver() = default;

// --- network thread -------------------------------------------------------------------------------

bool AudioReceiver::onPacket(std::span<const std::uint8_t> datagram, double arrival) {
    if (datagram.size() < kMediaHeaderBytes + kAudioHeaderBytes || datagram.size() > kMaxDatagramBytes ||
        peekType(datagram) != PacketType::Media || datagram[5] != static_cast<std::uint8_t>(MediaKind::Audio))
        return false;
    RawPacket raw;
    raw.arrival = arrival;
    raw.size = static_cast<std::uint16_t>(datagram.size());
    std::memcpy(raw.data, datagram.data(), datagram.size());
    if (!queue_->push(raw)) {
        queueFull_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // for the report's round-trip echo
    std::uint64_t timeUs = 0;
    for (int i = 0; i < 8; ++i) timeUs |= std::uint64_t(datagram[16 + std::size_t(i)]) << (8 * i);
    echoTimeUs_.store(timeUs, std::memory_order_relaxed);
    echoArrival_.store(arrival, std::memory_order_relaxed);
    return true;
}

// --- audio thread ---------------------------------------------------------------------------------

void AudioReceiver::beginStream(std::uint32_t id, const AudioHeader& fmt, std::uint64_t pos) {
    const bool sameFormat = decoder_ && fmt_ == fmt;
    if (state_ != State::Idle) ++streamChanges_;
    streamId_ = id;
    fmt_ = fmt;
    grid_ = pos % fmt.frames;
    if (!sameFormat || fmt.codec == AudioCodec::Opus) {
        // allocates; happens once per stream start (Opus needs a fresh decoder state for a new stream)
        decoder_ = std::make_unique<AudioDecoder>(fmt.codec, fmt.channels, fmt.frames);
        decodeBuf_.assign(std::size_t(fmt.frames) * fmt.channels, 0.0f);
        concealBuf_.assign(decodeBuf_.size(), 0.0f);
        lastPacket_.assign(decodeBuf_.size(), 0.0f);
        staging_.assign(std::size_t(fmt.frames) * outCh_, 0.0f);
    }
    for (auto& s : slots_) s.used = false;
    haveSeq_ = false;
    seq_.reset();
    received_ = late_ = duplicates_ = concealed_ = fec_ = 0;
    resync();
}

void AudioReceiver::resync() {
    state_ = State::Waiting;
    firstArrival_ = -1;
    arrival_.reset();
    controller_.reset(true);
    resampler_.reset();
    stagingOff_ = stagingLen_ = 0;
    fadeIn_ = 0;
    haveLast_ = false;
    missRun_ = 0;
    concealGain_ = 1;
    historyLen_ = historyPos_ = 0;
    badRun_ = 0;
}

AudioReceiver::Slot* AudioReceiver::slotFor(std::uint64_t pos) {
    auto& s = slots_[std::size_t((pos / fmt_.frames) % kSlots)];
    return &s;
}

void AudioReceiver::handle(const RawPacket& raw) {
    const auto media = parseMedia({raw.data, raw.size});
    if (!media || media->header.kind != MediaKind::Audio || media->header.fragCount != 1) {
        ++rejected_;
        return;
    }
    const auto audio = parseAudio(media->payload);
    if (!audio || audio->data.size() > kMaxMediaPayloadBytes ||
        !validPacketFrames(audio->header.codec, audio->header.channels, audio->header.frames)) {
        ++rejected_;
        return;
    }
    const auto& h = media->header;

    if (state_ == State::Idle) {
        if (cfg_.streamId != 0 && h.streamId != cfg_.streamId) {
            ++rejected_;
            return;
        }
        beginStream(h.streamId, audio->header, h.position);
    } else if (h.streamId != streamId_) {
        // another stream: switch only when it announces a (re)start of the sender
        if ((cfg_.streamId != 0 && h.streamId != cfg_.streamId) || !(h.flags & kFlagStreamStart)) {
            ++rejected_;
            return;
        }
        beginStream(h.streamId, audio->header, h.position);
    } else if (!(audio->header == fmt_) || h.position % fmt_.frames != grid_) {
        beginStream(h.streamId, audio->header, h.position); // same sender, new format
    }

    // sequence statistics
    const auto s = seq_.unwrap(h.seq);
    if (!haveSeq_) {
        haveSeq_ = true;
        firstSeq_ = maxSeq_ = s;
    } else {
        firstSeq_ = std::min(firstSeq_, s);
        maxSeq_ = std::max(maxSeq_, s);
    }

    const std::uint64_t end = h.position + fmt_.frames;
    if (firstArrival_ < 0) firstArrival_ = raw.arrival;
    arrival_.update(end, raw.arrival);

    if (state_ == State::Running) {
        if (end <= decodePos_) {
            ++late_;
            ++received_;
            if (++badRun_ >= kBadRunResync) { // the sender's timing moved (paused, restarted): start over
                ++resyncs_;
                resync();
                firstArrival_ = raw.arrival;
                arrival_.update(end, raw.arrival);
            }
            return;
        }
        if (h.position >= decodePos_ + kSlots * fmt_.frames) {
            ++received_;
            if (++badRun_ >= kBadRunResync) {
                ++resyncs_;
                resync();
                firstArrival_ = raw.arrival;
                arrival_.update(end, raw.arrival);
            }
            return;
        }
    }
    badRun_ = 0;

    Slot* slot = slotFor(h.position);
    if (slot->used && slot->pos == h.position) {
        ++duplicates_;
        return;
    }
    ++received_;
    slot->used = true;
    slot->pos = h.position;
    slot->size = static_cast<std::uint16_t>(audio->data.size());
    std::memcpy(slot->data, audio->data.data(), audio->data.size());
}

double AudioReceiver::currentTarget(double now) const {
    const double rate = cfg_.nominalRate;
    double t;
    if (cfg_.targetMs > 0) {
        t = cfg_.targetMs * rate / 1000.0;
    } else {
        // the block being produced (plus the resampler's look-ahead) must be complete even when the packet
        // holding its last frame is as late as the latest recently seen; that packet ends up to one
        // packet beyond the block
        t = double(blockPeak_) + double(cfg_.quality.halfTaps) + double(fmt_.frames) +
            arrival_.latenessPeak(now) * rate + cfg_.marginMs * rate / 1000.0;
    }
    return std::clamp(t, cfg_.minTargetMs * rate / 1000.0, cfg_.maxTargetMs * rate / 1000.0);
}

double AudioReceiver::readPosition() const { return double(srcEnd_) - resampler_.bufferedFrames(); }

bool AudioReceiver::start(double now) {
    if (!arrival_.ready() || firstArrival_ < 0 || now - firstArrival_ < cfg_.startSeconds) return false;
    target_ = currentTarget(now);
    const double startPos = arrival_.availableAt(now) - target_;
    if (startPos < 0) return false;
    // first packet boundary at or before start, then skip into it
    const auto start = std::max(static_cast<std::uint64_t>(startPos), grid_);
    const std::uint64_t f = fmt_.frames;
    const std::uint64_t aligned = (start - grid_) / f * f + grid_;
    decodePos_ = aligned;
    srcEnd_ = start;
    stagingOff_ = stagingLen_ = 0;
    decodeNext();
    const std::size_t skip = std::size_t(std::min<std::uint64_t>(start - aligned, stagingLen_));
    stagingOff_ += skip;
    stagingLen_ -= skip;
    controller_.reset(true);
    lastPull_ = now;
    state_ = State::Running;
    return true;
}

void AudioReceiver::toOutputChannels(const float* src, float* dst) {
    const std::size_t n = fmt_.frames;
    if (std::size_t(fmt_.channels) == outCh_) {
        std::memcpy(dst, src, n * outCh_ * sizeof(float));
    } else if (fmt_.channels == 1) {
        for (std::size_t i = 0; i < n; ++i) dst[2 * i] = dst[2 * i + 1] = src[i];
    } else {
        for (std::size_t i = 0; i < n; ++i) dst[i] = 0.5f * (src[2 * i] + src[2 * i + 1]);
    }
}

// Waveform repetition: the last good packet alternately backwards and forwards, which keeps the
// signal continuous at every joint (backwards starts where the packet ended, forwards starts where
// backwards ended), fading out over concealFadeMs.
void AudioReceiver::concealPcm(float* dst, std::size_t frames) {
    const std::size_t ch = std::size_t(fmt_.channels);
    const std::size_t n = fmt_.frames;
    const double fade = std::max(1.0, cfg_.concealFadeMs * cfg_.nominalRate / 1000.0);
    const bool backwards = (missRun_ % 2) == 1; // missRun_ counts this packet already
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t src = backwards ? n - 1 - (i % n) : (i % n);
        const float g = float(concealGain_);
        for (std::size_t c = 0; c < ch; ++c) dst[i * ch + c] = haveLast_ ? lastPacket_[src * ch + c] * g : 0.0f;
        concealGain_ = std::max(0.0, concealGain_ - 1.0 / fade);
    }
}

void AudioReceiver::decodeNext() {
    const std::uint64_t pos = decodePos_;
    const std::size_t n = fmt_.frames;
    const std::size_t ch = std::size_t(fmt_.channels);
    Slot* slot = slotFor(pos);
    const bool have = slot->used && slot->pos == pos;
    bool ok = false;
    if (have) {
        ok = decoder_->decode({slot->data, slot->size}, decodeBuf_.data());
        slot->used = false;
    }
    if (have && !ok && fmt_.codec == AudioCodec::Opus) {
        ++missRun_; // malformed: decode() already concealed it
        ++concealed_;
    } else if (ok) {
        if (missRun_ > 0 && fmt_.codec != AudioCodec::Opus) {
            // crossfade from where the concealment would have continued into the real audio
            ++missRun_;
            const std::size_t xf = std::min(kCrossfadeFrames, n);
            concealPcm(concealBuf_.data(), xf);
            for (std::size_t i = 0; i < xf; ++i) {
                const float a = float(i + 1) / float(xf + 1);
                for (std::size_t c = 0; c < ch; ++c)
                    decodeBuf_[i * ch + c] = decodeBuf_[i * ch + c] * a + concealBuf_[i * ch + c] * (1 - a);
            }
        }
        std::memcpy(lastPacket_.data(), decodeBuf_.data(), decodeBuf_.size() * sizeof(float));
        haveLast_ = true;
        missRun_ = 0;
        concealGain_ = 1;
    } else {
        ++missRun_;
        bool rebuilt = false;
        if (fmt_.codec == AudioCodec::Opus) {
            const Slot* next = slotFor(pos + n);
            if (next->used && next->pos == pos + n) rebuilt = decoder_->decodeFec({next->data, next->size}, decodeBuf_.data());
            if (rebuilt) ++fec_;
            else decoder_->conceal(decodeBuf_.data());
        } else {
            concealPcm(decodeBuf_.data(), n);
        }
        if (!rebuilt) ++concealed_;
        else missRun_ = 0;
    }
    toOutputChannels(decodeBuf_.data(), staging_.data());
    stagingOff_ = 0;
    stagingLen_ = n;
    decodePos_ = pos + n;
    if (fadeIn_ > 0) {
        const std::size_t m = std::min(fadeIn_, n);
        for (std::size_t i = 0; i < m; ++i) {
            const float a = float(kSkipFadeFrames - fadeIn_ + i + 1) / float(kSkipFadeFrames + 1);
            for (std::size_t c = 0; c < outCh_; ++c) staging_[i * outCh_ + c] *= a;
        }
        fadeIn_ -= m;
    }
}

void AudioReceiver::feedResampler(std::size_t frames) {
    float* dst = resampler_.inputWritePointer(frames);
    std::size_t done = 0;
    while (done < frames) {
        if (stagingLen_ == 0) decodeNext();
        const std::size_t m = std::min(frames - done, stagingLen_);
        const float* src = staging_.data() + stagingOff_ * outCh_;
        std::memcpy(dst + done * outCh_, src, m * outCh_ * sizeof(float));
        // remember what was given, for insertions
        for (std::size_t i = 0; i < m; ++i) {
            std::memcpy(&history_[historyPos_ * outCh_], src + i * outCh_, outCh_ * sizeof(float));
            historyPos_ = (historyPos_ + 1) % kHistoryFrames;
        }
        historyLen_ = std::min(kHistoryFrames, historyLen_ + m);
        stagingOff_ += m;
        stagingLen_ -= m;
        done += m;
    }
    resampler_.commitInput(frames);
    srcEnd_ += frames;
}

// Grows the buffer by `frames` without a jump in the waveform: the last L given frames are repeated
// backwards and then forwards (2L frames), which joins the signal at both ends.
void AudioReceiver::insertFrames(std::size_t frames) {
    const std::size_t L = std::min({(frames + 1) / 2, historyLen_, kHistoryFrames / 2});
    if (L == 0) return;
    float* dst = resampler_.inputWritePointer(2 * L);
    auto at = [&](std::size_t back) { // back = 1 is the newest frame
        return &history_[((historyPos_ + kHistoryFrames - back) % kHistoryFrames) * outCh_];
    };
    for (std::size_t i = 0; i < L; ++i) std::memcpy(dst + i * outCh_, at(i + 1), outCh_ * sizeof(float));
    for (std::size_t i = 0; i < L; ++i) std::memcpy(dst + (L + i) * outCh_, at(L - i), outCh_ * sizeof(float));
    resampler_.commitInput(2 * L);
    ++insertions_;
}

// Shrinks the buffer by dropping `frames` source frames ahead of the play position.
void AudioReceiver::skipFrames(std::size_t frames) {
    std::size_t left = frames;
    const std::size_t m = std::min(left, stagingLen_);
    stagingOff_ += m;
    stagingLen_ -= m;
    left -= m;
    while (left >= fmt_.frames) {
        Slot* s = slotFor(decodePos_);
        if (s->pos == decodePos_) s->used = false;
        decodePos_ += fmt_.frames;
        left -= fmt_.frames;
    }
    if (left > 0) {
        decodeNext();
        stagingOff_ += left;
        stagingLen_ -= left;
    }
    srcEnd_ += frames;
    fadeIn_ = kSkipFadeFrames;
    if (stagingLen_ > 0) { // fade what is already staged
        const std::size_t f = std::min(fadeIn_, stagingLen_);
        for (std::size_t i = 0; i < f; ++i) {
            const float a = float(i + 1) / float(kSkipFadeFrames + 1);
            for (std::size_t c = 0; c < outCh_; ++c) staging_[(stagingOff_ + i) * outCh_ + c] *= a;
        }
        fadeIn_ -= f;
    }
    ++skips_;
}

void AudioReceiver::pull(float* out, std::size_t frames, double now) {
    RawPacket raw;
    while (queue_->pop(raw)) handle(raw);
    frames = std::min(frames, cfg_.maxBlockFrames);
    blockPeak_ = std::max(blockPeak_, frames);

    if (state_ == State::Waiting && !start(now)) {
        std::memset(out, 0, frames * outCh_ * sizeof(float));
        publish();
        return;
    }
    if (state_ == State::Idle) {
        std::memset(out, 0, frames * outCh_ * sizeof(float));
        publish();
        return;
    }

    const double rate = cfg_.nominalRate;
    target_ = currentTarget(now);
    fill_ = arrival_.availableAt(now) - readPosition();
    double err = fill_ - target_;
    if (std::abs(err) > cfg_.resyncMs * rate / 1000.0) {
        ++resyncs_;
        // keep the arrival history: it is still valid, only the play position is off
        state_ = State::Waiting;
        resampler_.reset();
        stagingOff_ = stagingLen_ = 0;
        controller_.reset(true);
        if (!start(now)) {
            std::memset(out, 0, frames * outCh_ * sizeof(float));
            publish();
            return;
        }
        fill_ = arrival_.availableAt(now) - readPosition();
        err = fill_ - target_;
    }

    // large deviations are corrected at once; the PI loop only handles the slow ones
    const double grow = std::max(0.5e-3 * rate, 0.15 * target_);
    const double shrink = std::max(20e-3 * rate, target_);
    if (err < -grow) {
        insertFrames(std::size_t(-err));
        fill_ = arrival_.availableAt(now) - readPosition();
    } else if (err > shrink) {
        skipFrames(std::size_t(err));
        fill_ = arrival_.availableAt(now) - readPosition();
    }

    const double dt = std::max(now - lastPull_, 0.0);
    lastPull_ = now;
    const double ratio = controller_.update(fill_ - target_, dt);
    const std::size_t need = resampler_.inputFramesRequired(frames, ratio);
    if (need > 0) feedResampler(need);
    resampler_.produce(out, frames, ratio);
    outputFrames_ += frames;
    publish();
}

// --- any thread -----------------------------------------------------------------------------------

void AudioReceiver::publish() {
    auto& p = *pub_;
    if (p.lock.test_and_set(std::memory_order_acquire)) return; // a reader holds it: next time
    Stats& s = p.stats;
    const double ms = 1000.0 / cfg_.nominalRate;
    s.state = state_;
    s.streamId = streamId_;
    s.codec = fmt_.codec;
    s.streamChannels = state_ == State::Idle ? 0 : fmt_.channels;
    s.packetFrames = state_ == State::Idle ? 0 : fmt_.frames;
    s.ratio = controller_.ratio();
    s.fillMs = fill_ * ms;
    s.targetMs = target_ * ms;
    s.latenessPeakMs = arrival_.latenessPeak(lastPull_) * 1000.0;
    s.playPosition = state_ == State::Running ? readPosition() : 0.0;
    s.received = received_;
    s.lost = haveSeq_ && maxSeq_ - firstSeq_ + 1 > received_ ? maxSeq_ - firstSeq_ + 1 - received_ : 0;
    s.late = late_;
    s.duplicates = duplicates_;
    s.concealed = concealed_;
    s.fecRecovered = fec_;
    s.insertions = insertions_;
    s.skips = skips_;
    s.resyncs = resyncs_;
    s.streamChanges = streamChanges_;
    s.rejected = rejected_ + queueFull_.load(std::memory_order_relaxed);
    s.outputFrames = outputFrames_;
    s.locked = state_ == State::Running && controller_.locked();
    p.lock.clear(std::memory_order_release);
}

AudioReceiver::Stats AudioReceiver::stats() const {
    auto& p = *pub_;
    while (p.lock.test_and_set(std::memory_order_acquire)) {
    }
    Stats s = p.stats;
    p.lock.clear(std::memory_order_release);
    return s;
}

Report AudioReceiver::makeReport(double now) const {
    const Stats s = stats();
    Report r;
    r.streamId = s.streamId;
    r.received = static_cast<std::uint32_t>(s.received);
    r.lost = static_cast<std::uint32_t>(s.lost);
    r.late = static_cast<std::uint32_t>(s.late);
    r.concealed = static_cast<std::uint32_t>(s.concealed);
    r.jitterUs = static_cast<std::uint32_t>(std::max(0.0, s.latenessPeakMs * 1000.0));
    r.latencyUs = static_cast<std::uint32_t>(std::max(0.0, s.targetMs * 1000.0));
    r.echoTimeUs = echoTimeUs_.load(std::memory_order_relaxed);
    const double hold = now - echoArrival_.load(std::memory_order_relaxed);
    r.echoHoldUs = r.echoTimeUs ? static_cast<std::uint32_t>(std::clamp(hold * 1e6, 0.0, 4e9)) : 0;
    return r;
}

} // namespace dc::lan
