// V4 prototype: frame hand-off from the producer (capd in the real app, vcam-feed here)
// to the Media Foundation virtual camera source.
//
// The MF source runs inside the Windows Camera Frame Server service (svchost, LOCAL SERVICE,
// session 0), not in the user's session. So:
//   - the section lives in the Global\ namespace,
//   - it is CREATED by the source (services hold SeCreateGlobalPrivilege, normal users do not),
//   - the producer only OPENS it, and keeps retrying while no camera app is using the camera.
// When the source is loaded somewhere without that privilege it falls back to Local\.
//
// Layout: header + 2 BGRA slots (top-down). Writer fills the slot that is not `latest`, bumps
// that slot's sequence (odd while writing, seqlock style) and then publishes it as `latest`.
#pragma once

#include <atomic>
#include <cstdint>

namespace dc::vcam {

inline constexpr wchar_t kBridgeGlobalName[] = L"Global\\DesktopCapture.VCam.v1";
inline constexpr wchar_t kBridgeLocalName[] = L"Local\\DesktopCapture.VCam.v1";
// Auto-reset event "<section name>.Frame", set by the producer after each published frame.
inline constexpr wchar_t kBridgeEventSuffix[] = L".Frame";
inline constexpr uint32_t kBridgeMagic = 0x4D414356;  // 'VCAM'
inline constexpr uint32_t kBridgeVersion = 1;
inline constexpr uint32_t kBridgeMaxWidth = 1920;
inline constexpr uint32_t kBridgeMaxHeight = 1080;
inline constexpr uint32_t kBridgeSlotBytes = kBridgeMaxWidth * kBridgeMaxHeight * 4;
// Producer is considered gone when its heartbeat is older than this.
inline constexpr int64_t kBridgeStaleMs = 500;

struct BridgeSlot {
    std::atomic<uint64_t> seq;  // odd while the writer is inside the slot
    uint32_t width;
    uint32_t height;
    int64_t qpc;           // QueryPerformanceCounter when the producer rendered the frame
    uint64_t frame_index;  // producer's running frame counter
    uint8_t reserved[32];
};

struct alignas(64) BridgeHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_bytes;
    std::atomic<uint32_t> latest;           // index of the newest complete slot, 0 or 1
    std::atomic<uint32_t> producer_pid;     // 0 = no producer attached
    std::atomic<int64_t> producer_heartbeat_qpc;
    std::atomic<uint64_t> consumer_frames;  // frames the source has delivered (diagnostics)
    std::atomic<int64_t> consumer_heartbeat_qpc;
    BridgeSlot slots[2];
};

inline constexpr uint32_t kBridgeHeaderBytes = (sizeof(BridgeHeader) + 4095) & ~4095u;
inline constexpr uint64_t kBridgeTotalBytes = uint64_t(kBridgeHeaderBytes) + 2ull * kBridgeSlotBytes;

inline uint8_t* bridge_slot_pixels(BridgeHeader* h, uint32_t slot) {
    return reinterpret_cast<uint8_t*>(h) + h->header_bytes + uint64_t(slot) * h->slot_bytes;
}

}  // namespace dc::vcam
