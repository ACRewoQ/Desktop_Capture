// D3D11 side of the V6 pipeline.
//
//   decoder output (NV12 D3D11 texture, or NV12 in memory for software decoding)
//     -> FrameConverter: ID3D11VideoProcessor NV12 -> BGRA on the decode device
//     -> SharedFrameRing: two BGRA textures shared (NT handle + keyed mutex) with the display device
//     -> SharedFrameReader: on the display device (Qt's), copies the newest frame into a local texture
//
// The decode device is separate from Qt's device so the decoder threads never touch Qt's immediate
// context; the keyed mutex orders GPU work between the two devices (same approach as Qt Multimedia's
// D3D11 texture bridge).
#pragma once

#include "common.hpp"
#include "mf_codec.hpp"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace v6 {

// Creates a video capable, multithread protected D3D11 device. luid == nullptr picks the default adapter.
ComPtr<ID3D11Device> createVideoDevice(const LUID* luid, std::string* adapterName = nullptr);
LUID adapterLuidOf(ID3D11Device* device);

class FrameConverter {
public:
    FrameConverter(ID3D11Device* device, int outWidth, int outHeight);

    // Converts the decoded frame into target (BGRA, render target bindable, outWidth x outHeight).
    void convert(IMFSample* sample, const VideoInfo& info, ID3D11Texture2D* target);

private:
    void ensureProcessor(UINT inW, UINT inH);
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* tex, UINT slice);
    ID3D11VideoProcessorOutputView* outputView(ID3D11Texture2D* tex);
    ID3D11Texture2D* uploadSoftwareFrame(IMFMediaBuffer* buf, const VideoInfo& info);

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    UINT vpInW_ = 0, vpInH_ = 0;
    int outW_, outH_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inViews_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outViews_;
    ComPtr<ID3D11Texture2D> staging_, uploaded_;  // software decoding path
};

class SharedFrameRing {
public:
    static constexpr int kSlots = 2;

    SharedFrameRing(ID3D11Device* device, int width, int height);
    ~SharedFrameRing();

    int width() const { return w_; }
    int height() const { return h_; }

    // Producer (decode thread). beginWrite never blocks for long: a slot the display has not copied yet is
    // simply overwritten with the newer frame.
    ID3D11Texture2D* beginWrite();
    void endWrite(std::uint64_t frameId);

    // Consumer side helpers.
    HANDLE sharedHandle(int slot) const { return handles_[slot]; }
    std::uint64_t sequence() const { return seq_.load(std::memory_order_acquire); }
    int latestSlot() const { return latest_.load(std::memory_order_acquire); }
    std::uint64_t frameIdOf(int slot) const { return ids_[slot].load(std::memory_order_acquire); }

private:
    int w_, h_;
    ComPtr<ID3D11DeviceContext> ctx_;
    std::array<ComPtr<ID3D11Texture2D>, kSlots> tex_;
    std::array<ComPtr<IDXGIKeyedMutex>, kSlots> mutex_;
    std::array<HANDLE, kSlots> handles_{};
    std::array<std::atomic<std::uint64_t>, kSlots> ids_{};
    std::atomic<int> latest_{-1};
    std::atomic<std::uint64_t> seq_{0};
    int writing_ = -1;
    int lastWritten_ = kSlots - 1;
};

// Lives on the display device. fetch() runs on the thread that owns that device's immediate context.
class SharedFrameReader {
public:
    SharedFrameReader(const SharedFrameRing& ring, ID3D11Device* displayDevice);

    // Copies the newest frame into dst if there is one newer than the last fetch. Returns true on copy.
    bool fetch(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, std::uint64_t* frameId);

private:
    const SharedFrameRing& ring_;
    std::array<ComPtr<ID3D11Texture2D>, SharedFrameRing::kSlots> tex_;
    std::array<ComPtr<IDXGIKeyedMutex>, SharedFrameRing::kSlots> mutex_;
    std::uint64_t lastSeq_ = 0;
};

// Reads a BGRA texture back to the CPU and decodes the frame-id barcode the test pattern burns in, to prove
// the pixels (not just the timestamps) make it through. Stalls the GPU, so callers use it once in a while.
class FrameProbe {
public:
    FrameProbe(ID3D11Device* device, int width, int height);
    // Returns true if the barcode decodes to expectedId plus the offset seen on the first frame (the LAN
    // stream numbers frames from the first one sent, the pattern from the sender's start). detail gets
    // the decoded id and a sample pixel.
    bool check(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, std::uint64_t expectedId, std::string* detail);
    std::uint64_t ok() const { return ok_; }
    std::uint64_t bad() const { return bad_; }
    const std::string& lastDetail() const { return last_; }

private:
    int w_, h_;
    ComPtr<ID3D11Texture2D> staging_;
    std::uint64_t ok_ = 0, bad_ = 0;
    bool haveOffset_ = false;
    std::uint64_t offset_ = 0;  // barcode - expectedId, mod 2^24
    std::string last_;
};

}  // namespace v6
