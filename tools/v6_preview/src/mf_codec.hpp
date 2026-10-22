// Media Foundation H.264 encoder and decoder wrappers for the V6 measurement.
//
// The encoder only stands in for the real capture pipeline (lane B uses FFmpeg; see V3); it is configured
// like a preview stream would be: no B-frames, low-latency mode, CBR, 1 s GOP.
// The decoder is the object under test: the Microsoft H.264 decoder MFT, either with a D3D11 device manager
// (DXVA hardware decoding, output stays in a D3D11 texture) or without one (software decoding to memory).
#pragma once

#include "common.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;

namespace v6 {

// Starts COM (MTA) and Media Foundation for the lifetime of the object.
class MfRuntime {
public:
    MfRuntime();
    ~MfRuntime();
    MfRuntime(const MfRuntime&) = delete;
    MfRuntime& operator=(const MfRuntime&) = delete;
};

struct EncodedUnit {
    std::uint64_t id = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> data;  // Annex B, SPS/PPS included on keyframes
};

enum class CodecKind { Auto, Hardware, Software };
const char* toString(CodecKind k);

class H264Encoder {
public:
    using OutputFn = std::function<void(EncodedUnit&&)>;

    H264Encoder(int width, int height, int fps, int kbps, CodecKind kind, OutputFn onOutput);
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // nv12 = tightly packed width*height*3/2 bytes. Called from one thread.
    void encode(const std::uint8_t* nv12, std::uint64_t id);

    const std::string& name() const { return name_; }
    bool hardware() const { return hardware_; }
    bool async() const { return async_; }

private:
    bool tryCreate(bool hardwareOnly);
    void configure();
    ComPtr<IMFSample> makeInput(const std::uint8_t* nv12, std::uint64_t id);
    void drainOutput();
    void asyncLoop();

    int w_, h_, fps_, kbps_;
    OutputFn onOutput_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    std::string name_;
    bool hardware_ = false;
    bool async_ = false;
    std::vector<std::uint8_t> seqHeader_;
    std::int64_t frameDuration_ = 0;  // 100 ns units

    // Async (hardware) MFT plumbing.
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<ComPtr<IMFSample>> pending_;
    std::atomic<bool> stop_{false};
};

// Information about the decoder's current output type.
struct VideoInfo {
    UINT32 codedWidth = 0, codedHeight = 0;  // surface size (height often aligned to 16, e.g. 1088)
    RECT visible{};                          // display aperture inside the surface
    LONG stride = 0;                         // software output only
};

class H264Decoder {
public:
    // device == nullptr -> software decoding. With a device the MFT decodes through DXVA into D3D11 textures.
    H264Decoder(ID3D11Device* device, int width, int height, int fps);
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    using FrameFn = std::function<void(IMFSample* sample, std::uint64_t id, const VideoInfo& info)>;
    // Feeds one access unit and calls onFrame for every frame the decoder releases.
    void decode(const EncodedUnit& unit, const FrameFn& onFrame);

    bool hardware() const { return hardware_; }
    const std::string& name() const { return name_; }
    const VideoInfo& info() const { return info_; }

private:
    void setOutputType();

    ComPtr<IMFTransform> mft_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    UINT resetToken_ = 0;
    bool hardware_ = false;
    bool providesSamples_ = false;
    DWORD outputBufferSize_ = 0;
    std::string name_;
    VideoInfo info_;
    std::int64_t frameDuration_ = 0;
};

}  // namespace v6
