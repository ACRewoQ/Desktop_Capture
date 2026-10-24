#include "dc/lanmedia/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc::lan {

VideoReceiver::VideoReceiver(Config config) : cfg_(config), slots_(kSlots), arrivalClock_(config.arrival) {}

void VideoReceiver::beginStream(std::uint32_t id) {
    if (haveStream_) ++st_.streamChanges;
    haveStream_ = true;
    streamId_ = id;
    st_.streamId = id;
    for (auto& s : slots_) s.used = false;
    haveNext_ = false;
    next_ = 0;
    haveSeq_ = false;
    seq_.reset();
    arrivalClock_.reset();
    echoTimeUs_ = 0;
    waitingKey_ = true;
    requestPending_ = true; // a viewer that joins mid-GOP needs a keyframe now
    lastRequest_ = -1e9;
}

VideoReceiver::Slot* VideoReceiver::slotFor(std::uint64_t number, bool create) {
    Slot& s = slots_[number % kSlots];
    if (s.used && s.number == number) return &s;
    if (!create) return nullptr;
    s.used = true;
    s.number = number;
    s.fragCount = 0;
    s.fragsSeen = 0;
    s.complete = false;
    return &s;
}

void VideoReceiver::needKeyframe(double) {
    if (!waitingKey_) {
        waitingKey_ = true;
        requestPending_ = true;
        lastRequest_ = -1e9;
    }
}

void VideoReceiver::dropBefore(std::uint64_t number) {
    if (!haveNext_ || number <= next_) return;
    st_.droppedFrames += number - next_;
    for (auto& s : slots_)
        if (s.used && s.number < number) s.used = false;
    next_ = number;
}

void VideoReceiver::deliver(Slot& s, const FrameHandler& out) {
    Frame f;
    f.data = std::span<const std::uint8_t>(s.data.data(), s.frameBytes);
    f.number = s.number;
    f.streamId = streamId_;
    f.keyframe = s.keyframe;
    f.captureTime = s.captureTime;
    f.arrival = s.completedAt;
    f.lateness = s.lateness;
    ++st_.frames;
    if (s.keyframe) ++st_.keyframes;
    s.used = false;
    next_ = s.number + 1;
    if (out) out(f);
}

void VideoReceiver::deliverReady(const FrameHandler& out) {
    if (!haveNext_) return;
    if (waitingKey_) {
        // the oldest complete keyframe ahead of us ends the wait; everything before it is dropped
        const Slot* key = nullptr;
        for (const auto& s : slots_)
            if (s.used && s.complete && s.keyframe && s.number >= next_ && (!key || s.number < key->number)) key = &s;
        if (!key) return;
        dropBefore(key->number);
        waitingKey_ = false;
        requestPending_ = false;
    }
    while (Slot* s = slotFor(next_, false)) {
        if (!s->complete) break;
        deliver(*s, out);
    }
}

bool VideoReceiver::onPacket(std::span<const std::uint8_t> datagram, double arrival, const FrameHandler& out) {
    const auto media = parseMedia(datagram);
    if (!media || media->header.kind != MediaKind::Video) {
        ++st_.rejected;
        return false;
    }
    const auto video = parseVideo(media->payload);
    const auto& h = media->header;
    if (!video || video->header.frameBytes > cfg_.maxFrameBytes) {
        ++st_.rejected;
        return false;
    }
    if (!haveStream_) {
        if (cfg_.streamId != 0 && h.streamId != cfg_.streamId) {
            ++st_.rejected;
            return false;
        }
        beginStream(h.streamId);
    } else if (h.streamId != streamId_) {
        if (!(h.flags & kFlagStreamStart) || (cfg_.streamId != 0 && h.streamId != cfg_.streamId)) {
            ++st_.rejected;
            return false;
        }
        beginStream(h.streamId); // the sender restarted (new encoder)
    }
    lastArrival_ = arrival;
    if (h.timeUs >= echoTimeUs_) {
        echoTimeUs_ = h.timeUs;
        echoArrival_ = arrival;
    }

    const std::uint64_t n = h.position;
    if (!haveNext_) {
        haveNext_ = true;
        next_ = n;
    }
    if (n < next_) {
        ++st_.lateFragments;
        publish();
        return true;
    }
    if (n >= next_ + kSlots) {
        // too far ahead to keep the frames in between: they are lost
        if (!waitingKey_) needKeyframe(arrival);
        dropBefore(n + 1 - kSlots);
    }

    Slot* s = slotFor(n, true);
    if (s->fragCount == 0) {
        s->fragCount = h.fragCount;
        s->frameBytes = video->header.frameBytes;
        s->keyframe = (h.flags & kFlagKeyframe) != 0;
        s->captureTime = double(h.timeUs) / 1e6;
        s->firstArrival = arrival;
        s->have.assign(h.fragCount, false);
        if (s->data.size() < s->frameBytes) s->data.resize(s->frameBytes);
    } else if (s->fragCount != h.fragCount || s->frameBytes != video->header.frameBytes) {
        ++st_.rejected;
        return false;
    }
    if (s->have[h.fragIndex]) {
        ++st_.duplicates;
        publish();
        return true;
    }
    s->have[h.fragIndex] = true;
    ++s->fragsSeen;
    std::memcpy(s->data.data() + video->header.offset, video->data.data(), video->data.size());

    ++st_.datagrams;
    const std::uint64_t seq = seq_.unwrap(h.seq);
    if (!haveSeq_) {
        haveSeq_ = true;
        firstSeq_ = maxSeq_ = seq;
    }
    firstSeq_ = std::min(firstSeq_, seq);
    maxSeq_ = std::max(maxSeq_, seq);

    if (s->fragsSeen == s->fragCount) {
        s->complete = true;
        s->completedAt = arrival;
        s->lateness = arrivalClock_.update(h.timeUs, arrival);
        deliverReady(out);
    }
    publish();
    return true;
}

std::optional<Report> VideoReceiver::poll(double now, const FrameHandler& out) {
    if (!haveStream_) return std::nullopt;

    // An open gap with a later frame complete for longer than the reorder window: the gap is lost.
    if (haveNext_ && !waitingKey_) {
        const Slot* head = slotFor(next_, false);
        if (!head || !head->complete) {
            const double wait =
                std::clamp(arrivalClock_.latenessPeak(now) + 0.005, cfg_.reorderSeconds, cfg_.maxReorderSeconds);
            bool lost = head && now - head->firstArrival >= wait; // the fragments of a frame leave together
            for (const auto& s : slots_)
                if (s.used && s.complete && s.number > next_ && now - s.completedAt >= wait) lost = true;
            if (lost) needKeyframe(now);
        }
    }
    deliverReady(out);

    std::optional<Report> r;
    const bool request = waitingKey_ && requestPending_ && now - lastRequest_ >= cfg_.keyframeRetrySeconds;
    if (request || now - lastReport_ >= cfg_.reportSeconds) {
        Report rep;
        rep.streamId = streamId_;
        if (request) {
            rep.flags = kReportKeyframeRequest;
            lastRequest_ = now;
            ++st_.keyframeRequests;
        }
        rep.received = static_cast<std::uint32_t>(st_.datagrams);
        rep.lost = static_cast<std::uint32_t>(st_.lostDatagrams);
        rep.late = static_cast<std::uint32_t>(st_.lateFragments);
        rep.concealed = static_cast<std::uint32_t>(st_.droppedFrames);
        rep.jitterUs = static_cast<std::uint32_t>(std::lround(arrivalClock_.latenessPeak(now) * 1e6));
        rep.echoTimeUs = echoTimeUs_;
        rep.echoHoldUs = echoTimeUs_ ? static_cast<std::uint32_t>(std::lround(std::max(0.0, now - echoArrival_) * 1e6)) : 0;
        lastReport_ = now;
        r = rep;
    }
    st_.latenessPeakMs = arrivalClock_.latenessPeak(now) * 1000.0;
    publish();
    return r;
}

void VideoReceiver::publish() {
    if (haveSeq_) {
        const std::uint64_t expected = maxSeq_ - firstSeq_ + 1;
        st_.lostDatagrams = expected > st_.datagrams ? expected - st_.datagrams : 0;
    }
    st_.waitingForKeyframe = waitingKey_;
    while (statsLock_.test_and_set(std::memory_order_acquire)) {
    }
    published_ = st_;
    statsLock_.clear(std::memory_order_release);
}

VideoReceiver::Stats VideoReceiver::stats() const {
    while (statsLock_.test_and_set(std::memory_order_acquire)) {
    }
    Stats s = published_;
    statsLock_.clear(std::memory_order_release);
    return s;
}

} // namespace dc::lan
