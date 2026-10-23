// Hardware video encoder through FFmpeg, fed with D3D11 NV12 textures that never leave the GPU (V3).
//
// - Surfaces come from an FFmpeg D3D11 frame pool on our own device; the converter renders straight into them.
//   One texture per surface: some drivers refuse NV12 render-target arrays (RTX 5090, driver 616.56).
// - Encoding runs on its own thread. NVENC is opened with delay=0 (about 5 ms at 1080p instead of 34 ms), which
//   makes avcodec_send_frame wait for the GPU; the thread keeps that wait away from the video mix.
// - openVideoEncoder() picks the encoder: "auto" tries NVENC, AMF, QSV and Media Foundation in that order and
//   uses the first that opens.
#pragma once

#include "d3d.hpp"
#include "settings.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace dc::capd {

class VideoEncoder {
public:
    using PacketSink = std::function<void(AVPacket*)>;

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
        std::uint64_t poolEmpty = 0;  // frames dropped because every surface was still in the encoder
    };

    // codecName: FFmpeg encoder name (h264_nvenc, hevc_amf, ...). Throws if it cannot be opened.
    VideoEncoder(const Device& dev, const std::string& codecName, const VideoSettings& v, bool globalHeader);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    const AVCodecContext* context() const { return ctx_; }
    std::string description() const;  // e.g. "hevc_nvenc 3840x2160@60 vbr 40000 kbps (d3d11)"

    // Starts the encoding thread; packets go to sink (on that thread).
    void start(PacketSink sink);

    // An NV12 surface to render into, or a null frame when the pool is exhausted (the frame is then dropped).
    struct Surface {
        AVFrame* frame = nullptr;
        ID3D11Texture2D* texture = nullptr;
        UINT arrayIndex = 0;
    };
    Surface acquire();

    // Queues the rendered surface for encoding (takes ownership). pts is in 1/fps units.
    void submit(Surface s, std::int64_t pts);

    // Encodes what is queued, flushes the codec and stops the thread.
    void finish();

    // A codec error on the encoding thread (the recording must stop). Empty while all is well.
    std::string error() const;
    Stats stats() const;

private:
    void run();
    void encode(AVFrame* frame);

    Device dev_;
    VideoSettings settings_;
    std::string codecName_;
    std::string inputMode_;
    AVBufferRef* d3dDevice_ = nullptr;
    AVBufferRef* d3dFrames_ = nullptr;
    AVBufferRef* qsvDevice_ = nullptr;
    AVBufferRef* qsvFrames_ = nullptr;
    bool arrayPool_ = false;
    std::vector<AVFrame*> ownPool_;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    PacketSink sink_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AVFrame*> queue_;
    bool closing_ = false;
    bool finished_ = false;
    std::thread thread_;
    std::string error_;
    Stats stats_;
};

// Opens the encoder the settings ask for. encoder "auto" (or a family name: nvenc, amf, qsv, mf) with codec
// h264 / hevc / av1. log receives one line per candidate tried. Throws when nothing could be opened.
std::unique_ptr<VideoEncoder> openVideoEncoder(const Device& dev, const VideoSettings& v, bool globalHeader,
                                               std::vector<std::string>* log);

// Encoder names in this FFmpeg build that take D3D11 or QSV frames.
std::vector<std::string> availableHwEncoders();

std::string ffmpegVersion();

}  // namespace dc::capd
