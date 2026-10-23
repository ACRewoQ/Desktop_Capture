#include "encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <stdexcept>

namespace dc::capbench {

namespace {

std::string avErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

void check(int err, const char* what) {
    if (err < 0) throw std::runtime_error(std::string(what) + ": " + avErr(err));
}

// Pixel formats the encoder accepts, AV_PIX_FMT_NONE terminated (empty = anything).
std::vector<AVPixelFormat> pixelFormats(const AVCodec* codec) {
    const void* list = nullptr;
    int count = 0;
    std::vector<AVPixelFormat> out;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &count) < 0 || !list)
        return out;
    const auto* fmts = static_cast<const AVPixelFormat*>(list);
    for (int i = 0; i < count; ++i) out.push_back(fmts[i]);
    return out;
}

bool accepts(const AVCodec* codec, AVPixelFormat f) {
    for (auto p : pixelFormats(codec))
        if (p == f) return true;
    return false;
}

std::int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Whether the device can make an NV12 array texture the video processor can render into. Some drivers refuse
// NV12 arrays with any bind flag but DECODER (seen on GeForce RTX 5090, driver 616.56: E_INVALIDARG).
bool nv12RenderTargetArrays(ID3D11Device* device, int width, int height, int count) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = static_cast<UINT>(count);
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, nullptr));  // null texture = validate only
}

}  // namespace

Encoder::Encoder(const Device& dev, const EncoderConfig& cfg) : dev_(dev), cfg_(cfg) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;

    // FFmpeg D3D11 device wrapping our own device, so capture, conversion and encoding share one GPU context.
    d3dDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!d3dDevice_) throw std::runtime_error("this FFmpeg build has no D3D11VA support");
    auto* devCtx = reinterpret_cast<AVHWDeviceContext*>(d3dDevice_->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(devCtx->hwctx);
    d3d->device = dev_.device.Get();
    d3d->device->AddRef();
    check(av_hwdevice_ctx_init(d3dDevice_), "av_hwdevice_ctx_init(d3d11va)");

    d3dFrames_ = av_hwframe_ctx_alloc(d3dDevice_);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(d3dFrames_->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = cfg_.width;
    frames->height = cfg_.height;
    // Fixed pool = one array texture. Where the driver refuses that, FFmpeg makes one texture per surface instead;
    // that pool has no limit, so we allocate poolSize frames up front and hand them out ourselves.
    arrayPool_ = nv12RenderTargetArrays(dev_.device.Get(), cfg_.width, cfg_.height, cfg_.poolSize);
    frames->initial_pool_size = arrayPool_ ? cfg_.poolSize : 0;
    static_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    check(av_hwframe_ctx_init(d3dFrames_), "av_hwframe_ctx_init(d3d11 nv12)");
    if (!arrayPool_) {
        for (int i = 0; i < cfg_.poolSize; ++i) {
            AVFrame* surface = av_frame_alloc();
            ownPool_.push_back(surface);
            check(av_hwframe_get_buffer(d3dFrames_, surface, 0), "allocate nv12 surface");
        }
    }

    pkt_ = av_packet_alloc();
    if (cfg_.codec != "none") {
        openOutput();
        openCodec();
    } else {
        inputMode_ = "no encoding";
    }
}

Encoder::~Encoder() {
    try {
        finish();
    } catch (...) {
    }
    avcodec_free_context(&ctx_);
    if (fmt_) {
        if (!(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
    }
    av_packet_free(&pkt_);
    for (AVFrame*& f : ownPool_) av_frame_free(&f);
    av_buffer_unref(&qsvFrames_);
    av_buffer_unref(&qsvDevice_);
    av_buffer_unref(&d3dFrames_);
    av_buffer_unref(&d3dDevice_);
}

void Encoder::openOutput() {
    if (cfg_.output.empty()) return;
    check(avformat_alloc_output_context2(&fmt_, nullptr, nullptr, cfg_.output.c_str()), "output format");
}

void Encoder::openCodec() {
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg_.codec.c_str());
    if (!codec) throw std::runtime_error("encoder not in this FFmpeg build: " + cfg_.codec);

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = cfg_.width;
    ctx_->height = cfg_.height;
    ctx_->time_base = AVRational{1, cfg_.fps};
    ctx_->framerate = AVRational{cfg_.fps, 1};
    ctx_->sample_aspect_ratio = AVRational{1, 1};
    ctx_->bit_rate = cfg_.bitrate;
    ctx_->rc_max_rate = cfg_.bitrate;
    ctx_->rc_buffer_size = static_cast<int>(cfg_.bitrate);  // 1 s VBV
    ctx_->gop_size = cfg_.fps * cfg_.gopSeconds;
    ctx_->max_b_frames = 0;
    ctx_->color_primaries = AVCOL_PRI_BT709;
    ctx_->color_trc = AVCOL_TRC_BT709;
    ctx_->colorspace = AVCOL_SPC_BT709;
    ctx_->color_range = AVCOL_RANGE_MPEG;
    if (fmt_ && (fmt_->oformat->flags & AVFMT_GLOBALHEADER)) ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (accepts(codec, AV_PIX_FMT_D3D11)) {
        ctx_->pix_fmt = AV_PIX_FMT_D3D11;
        ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
        ctx_->hw_frames_ctx = av_buffer_ref(d3dFrames_);
        inputMode_ = "d3d11 direct";
    } else if (accepts(codec, AV_PIX_FMT_QSV)) {
        // Intel: QSV device and frame pool derived from ours; frames are mapped, not copied.
        check(av_hwdevice_ctx_create_derived(&qsvDevice_, AV_HWDEVICE_TYPE_QSV, d3dDevice_, 0),
              "derive QSV device (needs an Intel GPU driving this device)");
        check(av_hwframe_ctx_create_derived(&qsvFrames_, AV_PIX_FMT_QSV, qsvDevice_, d3dFrames_, 0),
              "derive QSV frames");
        ctx_->pix_fmt = AV_PIX_FMT_QSV;
        ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
        ctx_->hw_frames_ctx = av_buffer_ref(qsvFrames_);
        inputMode_ = "d3d11 mapped to qsv";
    } else {
        throw std::runtime_error(cfg_.codec + " takes neither D3D11 nor QSV frames (software encoder?)");
    }

    // Defaults that favour low GPU/CPU load at good quality; --opt overrides them.
    std::vector<std::pair<std::string, std::string>> opts;
    const std::string name = cfg_.codec;
    if (name.find("_nvenc") != std::string::npos) {
        opts = {{"preset", "p4"}, {"tune", "hq"}, {"rc", "vbr"}};
    } else if (name.find("_amf") != std::string::npos) {
        opts = {{"usage", "transcoding"}, {"quality", "balanced"}, {"rc", "vbr_peak"}};
    } else if (name.find("_qsv") != std::string::npos) {
        opts = {{"preset", "medium"}};
    } else if (name.find("_mf") != std::string::npos) {
        opts = {{"hw_encoding", "1"}, {"scenario", "display_remoting"}};
    }
    for (const auto& o : cfg_.options) opts.push_back(o);
    for (const auto& [k, v] : opts) {
        const int r = av_opt_set(ctx_->priv_data, k.c_str(), v.c_str(), 0);
        if (r < 0) throw std::runtime_error("encoder option " + k + "=" + v + ": " + avErr(r));
    }

    check(avcodec_open2(ctx_, codec, nullptr), ("open " + cfg_.codec).c_str());

    if (fmt_) {
        stream_ = avformat_new_stream(fmt_, nullptr);
        check(avcodec_parameters_from_context(stream_->codecpar, ctx_), "codec parameters");
        stream_->time_base = ctx_->time_base;
        stream_->avg_frame_rate = ctx_->framerate;
        if (!(fmt_->oformat->flags & AVFMT_NOFILE))
            check(avio_open(&fmt_->pb, cfg_.output.c_str(), AVIO_FLAG_WRITE), ("open " + cfg_.output).c_str());
        check(avformat_write_header(fmt_, nullptr), "write header");
    }
}

Encoder::Surface Encoder::acquire() {
    Surface s;
    AVFrame* f = nullptr;
    if (arrayPool_) {
        f = av_frame_alloc();
        if (av_hwframe_get_buffer(d3dFrames_, f, 0) < 0) {
            av_frame_free(&f);
            return s;
        }
    } else {
        // A surface is free when only our pool still references it.
        for (AVFrame* p : ownPool_) {
            if (av_buffer_get_ref_count(p->buf[0]) == 1) {
                f = av_frame_clone(p);
                break;
            }
        }
        if (!f) return s;
    }
    s.frame = f;
    s.texture = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    s.arrayIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    return s;
}

void Encoder::submit(Surface s, std::int64_t pts, std::int64_t submitQpc) {
    AVFrame* frame = s.frame;
    if (!ctx_) {
        av_frame_free(&frame);
        return;
    }
    frame->pts = pts;
    if (qsvFrames_) {
        AVFrame* mapped = av_frame_alloc();
        mapped->format = AV_PIX_FMT_QSV;
        mapped->hw_frames_ctx = av_buffer_ref(qsvFrames_);
        const int r = av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        av_frame_free(&frame);
        if (r < 0) {
            av_frame_free(&mapped);
            throw std::runtime_error("map d3d11 -> qsv: " + avErr(r));
        }
        mapped->pts = pts;
        frame = mapped;
    }
    submitQpc_[static_cast<size_t>(pts) % submitQpc_.size()] = submitQpc;
    const int r = avcodec_send_frame(ctx_, frame);
    av_frame_free(&frame);
    check(r, "avcodec_send_frame");
    drain();
}

void Encoder::drain() {
    for (;;) {
        const int r = avcodec_receive_packet(ctx_, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
        check(r, "avcodec_receive_packet");

        ++stats_.packets;
        stats_.bytes += static_cast<std::uint64_t>(pkt_->size);
        if (pkt_->flags & AV_PKT_FLAG_KEY) ++stats_.keyframes;
        if (pkt_->pts != AV_NOPTS_VALUE) {
            const auto submitted = submitQpc_[static_cast<size_t>(pkt_->pts) % submitQpc_.size()];
            if (submitted) {
                const double ms = double(qpcNow() - submitted) * 1000.0 / double(qpcFreq_);
                stats_.latencySumMs += ms;
                stats_.latencyCount++;
                if (ms > stats_.latencyMaxMs) stats_.latencyMaxMs = ms;
            }
        }
        if (fmt_) {
            av_packet_rescale_ts(pkt_, ctx_->time_base, stream_->time_base);
            pkt_->stream_index = stream_->index;
            check(av_interleaved_write_frame(fmt_, pkt_), "write packet");
        }
        av_packet_unref(pkt_);
    }
}

void Encoder::finish() {
    if (finished_) return;
    finished_ = true;
    if (ctx_) {
        check(avcodec_send_frame(ctx_, nullptr), "flush encoder");
        drain();
    }
    if (fmt_ && stream_) check(av_write_trailer(fmt_), "write trailer");
}

std::string Encoder::description() const {
    return cfg_.codec + " (" + inputMode_ + (arrayPool_ ? "" : ", one texture per surface") + ")";
}

std::vector<std::string> availableHwEncoders() {
    std::vector<std::string> names;
    void* it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it)) {
        if (!av_codec_is_encoder(c) || c->type != AVMEDIA_TYPE_VIDEO) continue;
        if (accepts(c, AV_PIX_FMT_D3D11) || accepts(c, AV_PIX_FMT_QSV)) names.emplace_back(c->name);
    }
    return names;
}

std::string ffmpegVersion() { return av_version_info(); }

}  // namespace dc::capbench
