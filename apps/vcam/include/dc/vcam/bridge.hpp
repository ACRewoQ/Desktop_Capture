// Frame hand-off from the producer (capd) to the Media Foundation virtual camera source (vcam_mf.dll).
//
// The MF source runs inside the Windows Camera Frame Server service (svchost, LOCAL SERVICE, session 0), not in
// the user's session (V4). So:
//   - the section lives in the Global\ namespace,
//   - it is CREATED by the source (services hold SeCreateGlobalPrivilege, normal users do not),
//   - the producer only OPENS it, and keeps retrying while no camera app is using the camera.
// When the source is loaded somewhere without that privilege it falls back to Local\.
//
// Layout: header + 2 slots. The writer fills the slot that is not `latest`, bumps that slot's sequence (odd while
// writing, seqlock style), then publishes it as `latest` and sets the frame event; the source sends a sample on
// each event, so the two 30 fps clocks never beat against each other (V4 finding 4).
//
// Version 2 (P3): a slot holds NV12 (what capd's GPU converter makes and what camera apps take, so the
// source only copies it) or BGRA (test pattern, prototypes).
#pragma once

#include <atomic>
#include <cstdint>

namespace dc::vcam {

inline constexpr wchar_t kBridgeGlobalName[] = L"Global\\DesktopCapture.VCam.v2";
inline constexpr wchar_t kBridgeLocalName[] = L"Local\\DesktopCapture.VCam.v2";
// Auto-reset event "<section name>.Frame", set by the producer after each published frame.
inline constexpr wchar_t kBridgeEventSuffix[] = L".Frame";
inline constexpr uint32_t kBridgeMagic = 0x4D414356;  // 'VCAM'
inline constexpr uint32_t kBridgeVersion = 2;
inline constexpr uint32_t kBridgeMaxWidth = 1920;
inline constexpr uint32_t kBridgeMaxHeight = 1080;
inline constexpr uint32_t kBridgeSlotBytes = kBridgeMaxWidth * kBridgeMaxHeight * 4;
// Producer is considered gone when its heartbeat is older than this.
inline constexpr int64_t kBridgeStaleMs = 500;

enum class PixelFormat : uint32_t {
    Bgra = 0,  // width * 4 bytes per row, top-down
    Nv12 = 1,  // Y plane (width bytes per row) followed by the interleaved UV plane (height / 2 rows)
};

struct BridgeSlot {
    std::atomic<uint64_t> seq;  // odd while the writer is inside the slot
    uint32_t width;
    uint32_t height;
    int64_t qpc;           // QueryPerformanceCounter when the producer rendered the frame
    uint64_t frame_index;  // producer's running frame counter
    uint32_t format;       // PixelFormat
    uint8_t reserved[28];
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
    std::atomic<int64_t> consumer_heartbeat_qpc;  // a camera app is reading while this is recent
    BridgeSlot slots[2];
};

inline constexpr uint32_t kBridgeHeaderBytes = (sizeof(BridgeHeader) + 4095) & ~4095u;
inline constexpr uint64_t kBridgeTotalBytes = uint64_t(kBridgeHeaderBytes) + 2ull * kBridgeSlotBytes;

inline uint8_t* bridge_slot_pixels(BridgeHeader* h, uint32_t slot) {
    return reinterpret_cast<uint8_t*>(h) + h->header_bytes + uint64_t(slot) * h->slot_bytes;
}

inline uint64_t frame_bytes(PixelFormat f, uint32_t w, uint32_t h) {
    return f == PixelFormat::Nv12 ? uint64_t(w) * h * 3 / 2 : uint64_t(w) * h * 4;
}

}  // namespace dc::vcam
