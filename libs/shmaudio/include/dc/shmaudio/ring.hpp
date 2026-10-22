#pragma once

#include "dc/shmaudio/layout.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace dc::shm {

// Engine side. Real-time safe: no allocation, no locks, no system calls besides the clock.
// Usage per audio block:
//   for each slot to export: writer.write(slot, samples, frames, channels)
//   writer.commit(frames, qpcOfFirstFrame)
class AudioRingWriter {
public:
    // Initializes the header in memory of at least kLayout.totalBytes bytes. Bumps the generation so
    // readers attached to a previous engine instance resynchronize.
    AudioRingWriter(void* memory, std::uint32_t writerPid);

    // Declares a slot as carrying audio from the next commit on (channels 1 or 2), or unused (0).
    void configureSlot(std::uint32_t slot, std::uint32_t channels, const char* label);
    void setSlotFlags(std::uint32_t slot, std::uint32_t flags);

    // Writes interleaved samples (frames * channels) for the block that the next commit() publishes.
    // Mono input fills only channel 0 of the stride-2 frame.
    void write(std::uint32_t slot, const float* interleaved, std::uint32_t frames, std::uint32_t channels);

    // Writes silence for the next block (used for unused or disconnected slots that readers follow).
    void writeSilence(std::uint32_t slot, std::uint32_t frames);

    // Publishes the block: advances framesWritten by frames and records a time anchor.
    void commit(std::uint32_t frames, std::int64_t qpcOfFirstFrame);

    std::uint64_t framesWritten() const;
    SharedHeader* header() const { return header_; }

private:
    float* slotData(std::uint32_t slot) const;

    SharedHeader* header_;
    SlotHeader* slots_;
    std::uint8_t* base_;
};

// Capture side. Each reader keeps its own position; the writer never waits for readers.
class AudioRingReader {
public:
    // Validates magic, version and sizes. Returns nullopt with a reason when the memory is not usable.
    static std::optional<AudioRingReader> attach(const void* memory, std::size_t bytes, std::string* error);

    enum class Status {
        Ok,
        NoData,          // nothing new yet
        Overrun,         // reader fell behind more than the ring; position was moved forward
        WriterRestarted, // generation changed; position was reset to the writer's current frame
    };

    struct ReadInfo {
        Status status = Status::Ok;
        std::uint64_t startFrame = 0; // engine frame index of out[0]
        std::uint32_t frames = 0;     // frames copied
        std::uint64_t skippedFrames = 0; // frames lost because of an overrun
    };

    // Starts reading at the writer's current position (minus an optional backlog).
    void seekToLatest(std::uint32_t backlogFrames = 0);
    void seek(std::uint64_t frame) { position_ = frame; }
    std::uint64_t position() const { return position_; }

    // Frames available to read right now.
    std::uint64_t available() const;

    // Copies up to maxFrames stride-2 frames of the slot into out (out.size() >= maxFrames * 2).
    // Slots that are unused or not yet active at a frame produce silence.
    // To read several slots aligned, call read(slot, ..., false) for each slot with the same
    // maxFrames, then advance(frames) once. With advanceAfter = true the position moves by itself.
    ReadInfo read(std::uint32_t slot, std::span<float> out, std::uint32_t maxFrames, bool advanceAfter = true);
    void advance(std::uint32_t frames) { position_ += frames; }

    // Maps an engine frame index to a QPC value using the most recent time anchors.
    std::optional<std::int64_t> qpcForFrame(std::uint64_t frame) const;

    // True if the writer committed within maxAgeQpcTicks.
    bool writerAlive(std::int64_t nowQpcValue, std::int64_t maxAgeQpcTicks) const;

    std::uint32_t slotChannels(std::uint32_t slot) const;
    std::uint32_t slotFlags(std::uint32_t slot) const;
    const SharedHeader* header() const { return header_; }

private:
    AudioRingReader(const SharedHeader* header, const SlotHeader* slots, const std::uint8_t* base);
    const float* slotData(std::uint32_t slot) const;

    const SharedHeader* header_;
    const SlotHeader* slots_;
    const std::uint8_t* base_;
    std::uint64_t generation_ = 0;
    std::uint64_t position_ = 0;
};

} // namespace dc::shm
