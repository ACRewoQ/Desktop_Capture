#pragma once

// Binary layout of the engine -> capture shared audio memory.
// The authoritative description is docs/spec/shm-audio.md; keep both in sync and bump kVersion on
// any incompatible change.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dc::shm {

inline constexpr std::uint32_t kMagic = 0x55414344; // "DCAU" in little-endian byte order
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kSampleRate = 48000;
inline constexpr std::uint32_t kSlotCount = 65;          // 0 = master, 1..64 = track N
inline constexpr std::uint32_t kMasterSlot = 0;
inline constexpr std::uint32_t kMaxChannels = 2;         // frames are always stored with stride 2
inline constexpr std::uint32_t kCapacityFrames = 65536;  // power of two, ~1.37 s at 48 kHz
inline constexpr std::uint32_t kMaxBlockFrames = 4096;   // writer never commits more at once
inline constexpr std::uint32_t kAnchorCount = 64;        // power of two
inline constexpr std::size_t kPageBytes = 4096;

inline constexpr const wchar_t* kWindowsMappingName = L"Local\\DesktopCapture.Audio.v1";
inline constexpr const char* kPosixMappingName = "/DesktopCapture.Audio.v1";

static_assert((kCapacityFrames & (kCapacityFrames - 1)) == 0, "capacity must be a power of two");
static_assert((kAnchorCount & (kAnchorCount - 1)) == 0, "anchor count must be a power of two");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
static_assert(std::atomic<std::int64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free");

// Pairs an engine frame index with the QueryPerformanceCounter value of that frame.
// Written with a sequence lock: seq is odd while the entry is being updated.
struct TimeAnchor {
    std::atomic<std::uint64_t> seq;
    std::atomic<std::uint64_t> frame;
    std::atomic<std::int64_t> qpc;
    std::uint64_t reserved;
};
static_assert(sizeof(TimeAnchor) == 32);

struct alignas(64) SharedHeader {
    // --- immutable after initialization (offset 0) ---
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t headerBytes;      // sizeof(SharedHeader)
    std::uint32_t slotHeaderBytes;  // sizeof(SlotHeader)
    std::uint32_t sampleRate;
    std::uint32_t slotCount;
    std::uint32_t capacityFrames;
    std::uint32_t maxChannels;
    std::uint64_t totalBytes;
    std::uint64_t slotsOffset;      // byte offset of SlotHeader[slotCount]
    std::uint64_t dataOffset;       // byte offset of the first slot's sample data
    std::uint64_t slotDataBytes;    // bytes per slot = capacityFrames * maxChannels * 4
    std::int64_t qpcFrequency;      // ticks per second of the qpc values below
    std::uint32_t anchorCount;
    std::uint32_t writerPid;

    // --- writer state ---
    alignas(64) std::atomic<std::uint64_t> generation;   // odd while (re)initializing; changes on engine restart
    alignas(64) std::atomic<std::uint64_t> framesWritten; // engine clock: total frames committed, all slots
    std::atomic<std::int64_t> heartbeatQpc;               // updated on every commit
    std::atomic<std::uint64_t> anchorWriteIndex;          // total anchors written
    alignas(64) TimeAnchor anchors[kAnchorCount];
};

struct alignas(64) SlotHeader {
    std::atomic<std::uint32_t> channels;          // 0 = unused (readers output silence), 1 or 2
    std::atomic<std::uint32_t> flags;             // see SlotFlags
    std::atomic<std::uint64_t> activeSinceFrame;  // first frame index holding valid data for this activation
    char label[48];                               // "master", "track/3" (diagnostics only)
};
static_assert(sizeof(SlotHeader) == 64);

enum SlotFlags : std::uint32_t {
    kSlotFlagSourceDisconnected = 1u << 0, // input lost; the slot carries silence
};

struct Layout {
    std::size_t slotsOffset;
    std::size_t dataOffset;
    std::size_t slotDataBytes;
    std::size_t totalBytes;
};

constexpr std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

constexpr Layout computeLayout() {
    Layout l{};
    l.slotsOffset = alignUp(sizeof(SharedHeader), kPageBytes);
    l.dataOffset = alignUp(l.slotsOffset + sizeof(SlotHeader) * kSlotCount, kPageBytes);
    l.slotDataBytes = std::size_t{kCapacityFrames} * kMaxChannels * sizeof(float);
    l.totalBytes = l.dataOffset + l.slotDataBytes * kSlotCount;
    return l;
}

inline constexpr Layout kLayout = computeLayout();

} // namespace dc::shm
