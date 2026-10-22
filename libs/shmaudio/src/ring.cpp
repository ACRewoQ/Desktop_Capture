#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/clock.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace dc::shm {

namespace {
constexpr std::uint64_t kMask = kCapacityFrames - 1;
// Frames the reader may lag behind before data can be overwritten while it copies.
constexpr std::uint64_t kSafeLag = kCapacityFrames - kMaxBlockFrames;
} // namespace

// ---------------- writer ----------------

AudioRingWriter::AudioRingWriter(void* memory, std::uint32_t writerPid)
    : header_(static_cast<SharedHeader*>(memory)),
      slots_(reinterpret_cast<SlotHeader*>(static_cast<std::uint8_t*>(memory) + kLayout.slotsOffset)),
      base_(static_cast<std::uint8_t*>(memory)) {
    const std::uint64_t g = header_->generation.load(std::memory_order_relaxed);
    const std::uint64_t initializing = (g % 2 == 0) ? g + 1 : g + 2;
    header_->generation.store(initializing, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    header_->magic = kMagic;
    header_->version = kVersion;
    header_->headerBytes = sizeof(SharedHeader);
    header_->slotHeaderBytes = sizeof(SlotHeader);
    header_->sampleRate = kSampleRate;
    header_->slotCount = kSlotCount;
    header_->capacityFrames = kCapacityFrames;
    header_->maxChannels = kMaxChannels;
    header_->totalBytes = kLayout.totalBytes;
    header_->slotsOffset = kLayout.slotsOffset;
    header_->dataOffset = kLayout.dataOffset;
    header_->slotDataBytes = kLayout.slotDataBytes;
    header_->qpcFrequency = qpcFrequency();
    header_->anchorCount = kAnchorCount;
    header_->writerPid = writerPid;

    header_->framesWritten.store(0, std::memory_order_relaxed);
    header_->heartbeatQpc.store(0, std::memory_order_relaxed);
    header_->anchorWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& a : header_->anchors) {
        a.seq.store(0, std::memory_order_relaxed);
        a.frame.store(0, std::memory_order_relaxed);
        a.qpc.store(0, std::memory_order_relaxed);
    }
    for (std::uint32_t i = 0; i < kSlotCount; ++i) {
        slots_[i].channels.store(0, std::memory_order_relaxed);
        slots_[i].flags.store(0, std::memory_order_relaxed);
        slots_[i].activeSinceFrame.store(0, std::memory_order_relaxed);
        std::memset(slots_[i].label, 0, sizeof slots_[i].label);
    }
    std::memset(base_ + kLayout.dataOffset, 0, kLayout.slotDataBytes * kSlotCount);

    header_->generation.store(initializing + 1, std::memory_order_release);
}

float* AudioRingWriter::slotData(std::uint32_t slot) const {
    return reinterpret_cast<float*>(base_ + kLayout.dataOffset + kLayout.slotDataBytes * slot);
}

void AudioRingWriter::configureSlot(std::uint32_t slot, std::uint32_t channels, const char* label) {
    if (slot >= kSlotCount) return;
    auto& s = slots_[slot];
    s.channels.store(0, std::memory_order_release);
    std::memset(s.label, 0, sizeof s.label);
    if (label) std::strncpy(s.label, label, sizeof s.label - 1);
    s.activeSinceFrame.store(header_->framesWritten.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s.channels.store(std::min(channels, kMaxChannels), std::memory_order_release);
}

void AudioRingWriter::setSlotFlags(std::uint32_t slot, std::uint32_t flags) {
    if (slot < kSlotCount) slots_[slot].flags.store(flags, std::memory_order_release);
}

void AudioRingWriter::write(std::uint32_t slot, const float* interleaved, std::uint32_t frames,
                            std::uint32_t channels) {
    if (slot >= kSlotCount || frames > kMaxBlockFrames || channels == 0) return;
    float* dst = slotData(slot);
    const std::uint64_t pos = header_->framesWritten.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint64_t idx = (pos + i) & kMask;
        dst[idx * 2] = interleaved[i * channels];
        dst[idx * 2 + 1] = channels >= 2 ? interleaved[i * channels + 1] : 0.0f;
    }
}

void AudioRingWriter::writeSilence(std::uint32_t slot, std::uint32_t frames) {
    if (slot >= kSlotCount || frames > kMaxBlockFrames) return;
    float* dst = slotData(slot);
    const std::uint64_t pos = header_->framesWritten.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint64_t idx = (pos + i) & kMask;
        dst[idx * 2] = 0.0f;
        dst[idx * 2 + 1] = 0.0f;
    }
}

void AudioRingWriter::commit(std::uint32_t frames, std::int64_t qpcOfFirstFrame) {
    if (frames == 0 || frames > kMaxBlockFrames) return;
    const std::uint64_t pos = header_->framesWritten.load(std::memory_order_relaxed);

    const std::uint64_t ai = header_->anchorWriteIndex.load(std::memory_order_relaxed);
    TimeAnchor& a = header_->anchors[ai & (kAnchorCount - 1)];
    const std::uint64_t seq = a.seq.load(std::memory_order_relaxed);
    a.seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    a.frame.store(pos, std::memory_order_relaxed);
    a.qpc.store(qpcOfFirstFrame, std::memory_order_relaxed);
    a.seq.store(seq + 2, std::memory_order_release);
    header_->anchorWriteIndex.store(ai + 1, std::memory_order_release);

    header_->heartbeatQpc.store(qpcOfFirstFrame, std::memory_order_relaxed);
    header_->framesWritten.store(pos + frames, std::memory_order_release);
}

std::uint64_t AudioRingWriter::framesWritten() const {
    return header_->framesWritten.load(std::memory_order_relaxed);
}

// ---------------- reader ----------------

AudioRingReader::AudioRingReader(const SharedHeader* header, const SlotHeader* slots, const std::uint8_t* base)
    : header_(header), slots_(slots), base_(base) {
    generation_ = header_->generation.load(std::memory_order_acquire);
}

std::optional<AudioRingReader> AudioRingReader::attach(const void* memory, std::size_t bytes, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<AudioRingReader> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!memory || bytes < kLayout.totalBytes) return fail("shared memory is too small");
    const auto* h = static_cast<const SharedHeader*>(memory);
    if (h->generation.load(std::memory_order_acquire) % 2 != 0) return fail("writer is initializing");
    if (h->magic != kMagic) return fail("bad magic");
    if (h->version != kVersion) return fail("unsupported version");
    if (h->headerBytes != sizeof(SharedHeader) || h->slotHeaderBytes != sizeof(SlotHeader) ||
        h->slotCount != kSlotCount || h->capacityFrames != kCapacityFrames || h->maxChannels != kMaxChannels ||
        h->totalBytes != kLayout.totalBytes || h->dataOffset != kLayout.dataOffset ||
        h->slotsOffset != kLayout.slotsOffset)
        return fail("layout mismatch");
    const auto* base = static_cast<const std::uint8_t*>(memory);
    AudioRingReader r(h, reinterpret_cast<const SlotHeader*>(base + kLayout.slotsOffset), base);
    r.seekToLatest();
    return r;
}

const float* AudioRingReader::slotData(std::uint32_t slot) const {
    return reinterpret_cast<const float*>(base_ + kLayout.dataOffset + kLayout.slotDataBytes * slot);
}

void AudioRingReader::seekToLatest(std::uint32_t backlogFrames) {
    const std::uint64_t w = header_->framesWritten.load(std::memory_order_acquire);
    const std::uint64_t backlog = std::min<std::uint64_t>(backlogFrames, kSafeLag);
    position_ = w > backlog ? w - backlog : 0;
}

std::uint64_t AudioRingReader::available() const {
    const std::uint64_t w = header_->framesWritten.load(std::memory_order_acquire);
    return w > position_ ? w - position_ : 0;
}

AudioRingReader::ReadInfo AudioRingReader::read(std::uint32_t slot, std::span<float> out, std::uint32_t maxFrames,
                                                 bool advanceAfter) {
    ReadInfo info;
    const std::uint64_t g = header_->generation.load(std::memory_order_acquire);
    if (g % 2 != 0) {
        info.status = Status::NoData;
        return info;
    }
    if (g != generation_) {
        generation_ = g;
        seekToLatest();
        info.status = Status::WriterRestarted;
        info.startFrame = position_;
        return info;
    }

    const std::uint64_t w = header_->framesWritten.load(std::memory_order_acquire);
    if (position_ > w) position_ = w;
    if (w - position_ > kSafeLag) {
        const std::uint64_t next = w - kCapacityFrames / 2;
        info.skippedFrames = next - position_;
        info.status = Status::Overrun;
        position_ = next;
    }

    maxFrames = std::min<std::uint32_t>(maxFrames, static_cast<std::uint32_t>(out.size() / 2));
    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(w - position_, maxFrames));
    info.startFrame = position_;
    if (n == 0) {
        if (info.status == Status::Ok) info.status = Status::NoData;
        return info;
    }

    const std::uint32_t channels = slot < kSlotCount ? slots_[slot].channels.load(std::memory_order_acquire) : 0;
    const std::uint64_t since = slot < kSlotCount ? slots_[slot].activeSinceFrame.load(std::memory_order_relaxed) : 0;
    if (channels == 0) {
        std::fill(out.begin(), out.begin() + std::ptrdiff_t{n} * 2, 0.0f);
    } else {
        const float* src = slotData(slot);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint64_t frame = position_ + i;
            if (frame < since) {
                out[i * 2] = 0.0f;
                out[i * 2 + 1] = 0.0f;
            } else {
                const std::uint64_t idx = frame & kMask;
                out[i * 2] = src[idx * 2];
                out[i * 2 + 1] = src[idx * 2 + 1];
            }
        }
    }

    // The writer may have lapped us while copying; then the copy is not trustworthy.
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t w2 = header_->framesWritten.load(std::memory_order_acquire);
    if (header_->generation.load(std::memory_order_acquire) != generation_) {
        info.status = Status::WriterRestarted;
        info.frames = 0;
        return info;
    }
    if (w2 - position_ > kSafeLag) {
        const std::uint64_t next = w2 - kCapacityFrames / 2;
        info.skippedFrames += next - position_;
        info.status = Status::Overrun;
        info.frames = 0;
        position_ = next;
        return info;
    }

    info.frames = n;
    if (advanceAfter) position_ += n;
    return info;
}

std::optional<std::int64_t> AudioRingReader::qpcForFrame(std::uint64_t frame) const {
    const std::uint64_t written = header_->anchorWriteIndex.load(std::memory_order_acquire);
    if (written == 0 || header_->qpcFrequency <= 0) return std::nullopt;
    const std::uint64_t count = std::min<std::uint64_t>(written, kAnchorCount);

    bool found = false;
    std::uint64_t bestFrame = 0;
    std::int64_t bestQpc = 0;
    std::uint64_t bestDistance = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t k = 0; k < count; ++k) {
        const TimeAnchor& a = header_->anchors[(written - 1 - k) & (kAnchorCount - 1)];
        const std::uint64_t s1 = a.seq.load(std::memory_order_acquire);
        if (s1 % 2 != 0) continue;
        const std::uint64_t f = a.frame.load(std::memory_order_relaxed);
        const std::int64_t q = a.qpc.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (a.seq.load(std::memory_order_relaxed) != s1) continue;
        const std::uint64_t d = f > frame ? f - frame : frame - f;
        if (d < bestDistance) {
            bestDistance = d;
            bestFrame = f;
            bestQpc = q;
            found = true;
        }
    }
    if (!found) return std::nullopt;
    const double deltaFrames = static_cast<double>(static_cast<std::int64_t>(frame - bestFrame));
    const double ticks = deltaFrames * static_cast<double>(header_->qpcFrequency) / header_->sampleRate;
    return bestQpc + static_cast<std::int64_t>(ticks);
}

bool AudioRingReader::writerAlive(std::int64_t nowQpcValue, std::int64_t maxAgeQpcTicks) const {
    const std::int64_t hb = header_->heartbeatQpc.load(std::memory_order_relaxed);
    return hb != 0 && nowQpcValue - hb <= maxAgeQpcTicks;
}

std::uint32_t AudioRingReader::slotChannels(std::uint32_t slot) const {
    return slot < kSlotCount ? slots_[slot].channels.load(std::memory_order_acquire) : 0;
}

std::uint32_t AudioRingReader::slotFlags(std::uint32_t slot) const {
    return slot < kSlotCount ? slots_[slot].flags.load(std::memory_order_acquire) : 0;
}

} // namespace dc::shm
