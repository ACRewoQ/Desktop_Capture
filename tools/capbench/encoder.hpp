// Hardware video encoder through FFmpeg, fed with D3D11 NV12 textures that never leave the GPU.
//
// The frames come from an FFmpeg D3D11 frame pool (one array texture) that lives on our own D3D11 device, so the
// converter writes straight into the encoder's input surfaces. NVENC, AMF and Media Foundation take those directly;
// QSV gets them mapped into QSV surfaces (same memory, no copy).
#pragma once

#include "d3d.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

namespace dc::capbench {

struct EncoderConfig {
    std::string codec = "h264_nvenc";  // FFmpeg encoder name, or "none" (capture + convert only)
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::int64_t bitrate = 12'000'000;
    int gopSeconds = 2;
    int poolSize = 32;
    std::vector<std::pair<std::string, std::string>> options;  // encoder private options (preset, rc, ...)
    std::string output;                                        // .mkv / .mp4 path; empty = discard the packets
};

struct EncoderStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t keyframes = 0;
    double latencySumMs = 0;  // submit -> packet out
    double latencyMaxMs = 0;
    std::uint64_t latencyCount = 0;
};

class Encoder {
public:
    Encoder(const Device& dev, const EncoderConfig& cfg);
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // A pool frame to render into. texture/arrayIndex address the NV12 surface. Null frame if the pool is empty
    // (the encoder still holds every surface).
    struct Surface {
        AVFrame* frame = nullptr;
        ID3D11Texture2D* texture = nullptr;
        UINT arrayIndex = 0;
    };
    Surface acquire();

    // Encodes the surface (takes ownership of the frame). pts is in 1/fps units.
    void submit(Surface s, std::int64_t pts, std::int64_t submitQpc);

    // Flushes the encoder and closes the output file.
    void finish();

    const EncoderStats& stats() const { return stats_; }
    void resetLatencyMax() { stats_.latencyMaxMs = 0; }
    std::string description() const;  // e.g. "h264_nvenc (d3d11 direct)"
    bool encoding() const { return ctx_ != nullptr; }

private:
    void openOutput();
    void openCodec();
    void drain();

    Device dev_;
    EncoderConfig cfg_;
    AVBufferRef* d3dDevice_ = nullptr;
    AVBufferRef* d3dFrames_ = nullptr;
    AVBufferRef* qsvDevice_ = nullptr;
    AVBufferRef* qsvFrames_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFormatContext* fmt_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool finished_ = false;
    std::string inputMode_;
    EncoderStats stats_;
    std::int64_t qpcFreq_ = 1;
    std::array<std::int64_t, 1024> submitQpc_{};
};

// Encoder names that exist in this FFmpeg build and can take D3D11 or QSV input.
std::vector<std::string> availableHwEncoders();

// FFmpeg version string (for reports).
std::string ffmpegVersion();

}  // namespace dc::capbench
