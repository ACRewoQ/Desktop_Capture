#include "video_encoder.hpp"

#include "muxer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include <stdexcept>

namespace dc::capd {

namespace {

constexpr int kPoolSize = 16;

void check(int err, const std::string& what) {
    if (err < 0) throw std::runtime_error(what + ": " + avError(err));
}

bool accepts(const AVCodec* codec, AVPixelFormat f) {
    const void* list = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &count) < 0 || !list)
        return false;
    const auto* fmts = static_cast<const AVPixelFormat*>(list);
    for (int i = 0; i < count; ++i)
        if (fmts[i] == f) return true;
    return false;
}

bool has(const std::string& name, const char* family) { return name.find(family) != std::string::npos; }

// Encoder private options for the family, the settings' rate control included.
std::vector<std::pair<std::string, std::string>> familyOptions(const std::string& name, const VideoSettings& v) {
    const std::string q = std::to_string(v.quality);
    std::vector<std::pair<std::string, std::string>> o;
    if (has(name, "_nvenc")) {
        // forced-idr: a frame submitted as a keyframe becomes an IDR (a preview receiver can start from it).
        o = {{"preset", v.lowLatency ? "p2" : "p4"}, {"tune", v.lowLatency ? "ull" : "hq"}, {"delay", "0"},
             {"forced-idr", "1"}};
        if (v.rateControl == "cqp") o.insert(o.end(), {{"rc", "constqp"}, {"qp", q}});
        else o.emplace_back("rc", v.rateControl == "cbr" ? "cbr" : "vbr");
    } else if (has(name, "_amf")) {
        o = {{"usage", v.lowLatency ? "ultralowlatency" : "transcoding"}, {"quality", v.lowLatency ? "speed" : "balanced"}};
        if (v.rateControl == "cqp") o.insert(o.end(), {{"rc", "cqp"}, {"qp_i", q}, {"qp_p", q}});
        else o.emplace_back("rc", v.rateControl == "cbr" ? "cbr" : "vbr_peak");
    } else if (has(name, "_qsv")) {
        o = {{"preset", v.lowLatency ? "veryfast" : "medium"}, {"forced_idr", "1"}};
        if (v.lowLatency) o.insert(o.end(), {{"async_depth", "1"}, {"look_ahead", "0"}});
    } else if (has(name, "_mf")) {
        o = {{"hw_encoding", "1"}, {"scenario", "display_remoting"}};
        if (v.rateControl == "cqp") {
            // MF quality is 0..100 (higher = better); map the QP scale 0..51 onto it.
            o.insert(o.end(), {{"rate_control", "quality"}, {"quality", std::to_string((51 - v.quality) * 100 / 51)}});
        } else {
            o.emplace_back("rate_control", v.rateControl == "cbr" ? "cbr" : "pc_vbr");
        }
    } else if (name == "libx264") {
        // veryfast: about one P-core per 1080p60 stream on a recent CPU; x264 is the fallback when no GPU encoder opens.
        o = {{"preset", "veryfast"}, {"forced-idr", "1"}};
        if (v.lowLatency) o.emplace_back("tune", "zerolatency");
        if (v.rateControl == "cqp") o.emplace_back("crf", q);
        else if (v.rateControl == "cbr") o.emplace_back("nal-hrd", "cbr");
    }
    return o;
}

}  // namespace

VideoEncoder::VideoEncoder(const Device& dev, const std::string& codecName, const VideoSettings& v,
                           bool globalHeader)
    : dev_(dev), settings_(v), codecName_(codecName) {
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName_.c_str());
    if (!codec) throw std::runtime_error("not in this FFmpeg build");
    software_ = codecName_ == "libx264";

    // FFmpeg D3D11 device wrapping our own device: capture, composition and encoding share one GPU.
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
    frames->width = v.width;
    frames->height = v.height;
    frames->initial_pool_size = 0;  // one texture per surface (see the header)
    static_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    check(av_hwframe_ctx_init(d3dFrames_), "av_hwframe_ctx_init(d3d11 nv12)");
    for (int i = 0; i < kPoolSize; ++i) {
        AVFrame* surface = av_frame_alloc();
        ownPool_.push_back(surface);
        check(av_hwframe_get_buffer(d3dFrames_, surface, 0), "allocate nv12 surface");
    }

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = v.width;
    ctx_->height = v.height;
    ctx_->time_base = AVRational{1, v.fps};
    ctx_->framerate = AVRational{v.fps, 1};
    ctx_->sample_aspect_ratio = AVRational{1, 1};
    ctx_->gop_size = v.gopFrames > 0 ? v.gopFrames : v.fps * 2;
    ctx_->max_b_frames = 0;
    ctx_->color_primaries = AVCOL_PRI_BT709;
    ctx_->color_trc = AVCOL_TRC_BT709;
    ctx_->colorspace = AVCOL_SPC_BT709;
    ctx_->color_range = AVCOL_RANGE_MPEG;
    if (globalHeader) ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const std::int64_t bitrate = std::int64_t{v.bitrateKbps} * 1000;
    if (v.rateControl == "cqp") {
        if (has(codecName_, "_qsv")) {
            ctx_->flags |= AV_CODEC_FLAG_QSCALE;
            ctx_->global_quality = v.quality * FF_QP2LAMBDA;
        }
    } else {
        ctx_->bit_rate = bitrate;
        ctx_->rc_max_rate = v.rateControl == "cbr" ? bitrate : bitrate * 3 / 2;
        ctx_->rc_buffer_size = static_cast<int>(v.rateControl == "cbr" ? bitrate : bitrate * 2);
    }

    if (software_) {
        // The converter still renders into the GPU pool; the encoding thread downloads each surface
        // (av_hwframe_transfer_data: staging copy + map under FFmpeg's device lock, about 1 ms at 1080p).
        if (!accepts(codec, AV_PIX_FMT_NV12)) throw std::runtime_error("does not take NV12");
        ctx_->pix_fmt = AV_PIX_FMT_NV12;
        ctx_->thread_count = 0;  // x264 picks its own thread count
        inputMode_ = "d3d11 downloaded to cpu";
    } else if (accepts(codec, AV_PIX_FMT_D3D11)) {
        ctx_->pix_fmt = AV_PIX_FMT_D3D11;
        ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
        ctx_->hw_frames_ctx = av_buffer_ref(d3dFrames_);
        inputMode_ = "d3d11";
    } else if (accepts(codec, AV_PIX_FMT_QSV)) {
        // Intel: QSV device and frame pool derived from ours; frames are mapped, not copied.
        check(av_hwdevice_ctx_create_derived(&qsvDevice_, AV_HWDEVICE_TYPE_QSV, d3dDevice_, 0), "derive QSV device");
        check(av_hwframe_ctx_create_derived(&qsvFrames_, AV_PIX_FMT_QSV, qsvDevice_, d3dFrames_, 0),
              "derive QSV frames");
        ctx_->pix_fmt = AV_PIX_FMT_QSV;
        ctx_->sw_pix_fmt = AV_PIX_FMT_NV12;
        ctx_->hw_frames_ctx = av_buffer_ref(qsvFrames_);
        inputMode_ = "d3d11 mapped to qsv";
    } else {
        throw std::runtime_error("takes neither D3D11 nor QSV frames");
    }

    for (const auto& [k, val] : familyOptions(codecName_, v)) {
        const int r = av_opt_set(ctx_->priv_data, k.c_str(), val.c_str(), 0);
        // Option sets differ a little between FFmpeg versions and codecs; a missing one is not fatal.
        if (r < 0 && r != AVERROR_OPTION_NOT_FOUND) check(r, "option " + k + "=" + val);
    }
    check(avcodec_open2(ctx_, codec, nullptr), "open");
    pkt_ = av_packet_alloc();
}

VideoEncoder::~VideoEncoder() {
    try {
        finish();
    } catch (...) {
    }
    for (AVFrame* f : queue_) av_frame_free(&f);
    av_packet_free(&pkt_);
    avcodec_free_context(&ctx_);
    for (AVFrame*& f : ownPool_) av_frame_free(&f);
    av_buffer_unref(&qsvFrames_);
    av_buffer_unref(&qsvDevice_);
    av_buffer_unref(&d3dFrames_);
    av_buffer_unref(&d3dDevice_);
}

std::string VideoEncoder::description() const {
    std::string rc = settings_.rateControl == "cqp" ? "cqp " + std::to_string(settings_.quality)
                                                    : settings_.rateControl + " " +
                                                          std::to_string(settings_.bitrateKbps) + " kbps";
    return codecName_ + " " + std::to_string(settings_.width) + "x" + std::to_string(settings_.height) + "@" +
           std::to_string(settings_.fps) + " " + rc + " (" + inputMode_ + ")";
}

void VideoEncoder::start(PacketSink sink) {
    sink_ = std::move(sink);
    thread_ = std::thread([this] { run(); });
}

VideoEncoder::Surface VideoEncoder::acquire() {
    Surface s;
    for (AVFrame* p : ownPool_) {
        // A surface is free when only our pool still references it.
        if (av_buffer_get_ref_count(p->buf[0]) == 1) {
            s.frame = av_frame_clone(p);
            break;
        }
    }
    if (!s.frame) {
        std::lock_guard lock(mutex_);
        ++stats_.poolEmpty;
        return s;
    }
    s.texture = reinterpret_cast<ID3D11Texture2D*>(s.frame->data[0]);
    s.arrayIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(s.frame->data[1]));
    return s;
}

void VideoEncoder::submit(Surface s, std::int64_t pts, bool keyframe) {
    s.frame->pts = pts;
    s.frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    {
        std::lock_guard lock(mutex_);
        if (closing_ || !error_.empty()) {
            av_frame_free(&s.frame);
            return;
        }
        queue_.push_back(s.frame);
        ++stats_.submitted;
    }
    cv_.notify_one();
}

void VideoEncoder::run() {
    for (;;) {
        AVFrame* frame = nullptr;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) break;
            frame = queue_.front();
            queue_.pop_front();
        }
        try {
            encode(frame);
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            if (error_.empty()) error_ = codecName_ + ": " + e.what();
        }
    }
    try {
        encode(nullptr);  // flush
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = codecName_ + " flush: " + e.what();
    }
}

void VideoEncoder::encode(AVFrame* frame) {
    if (frame && software_) {
        AVFrame* cpu = av_frame_alloc();
        cpu->format = AV_PIX_FMT_NV12;
        const int r = av_hwframe_transfer_data(cpu, frame, 0);
        cpu->pts = frame->pts;
        cpu->pict_type = frame->pict_type;
        av_frame_free(&frame);
        if (r < 0) {
            av_frame_free(&cpu);
            check(r, "download nv12 surface");
        }
        frame = cpu;
    } else if (frame && qsvFrames_) {
        AVFrame* mapped = av_frame_alloc();
        mapped->format = AV_PIX_FMT_QSV;
        mapped->hw_frames_ctx = av_buffer_ref(qsvFrames_);
        const int r = av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        mapped->pts = frame->pts;
        mapped->pict_type = frame->pict_type;
        av_frame_free(&frame);
        if (r < 0) {
            av_frame_free(&mapped);
            check(r, "map d3d11 -> qsv");
        }
        frame = mapped;
    }
    {
        std::lock_guard lock(mutex_);
        if (!error_.empty()) {  // after an error, only release the surfaces
            av_frame_free(&frame);
            return;
        }
    }
    for (;;) {
        const int sent = avcodec_send_frame(ctx_, frame);
        if (sent != AVERROR(EAGAIN)) {
            av_frame_free(&frame);
            if (sent < 0 && sent != AVERROR_EOF) check(sent, "send frame");
        }
        for (;;) {
            const int r = avcodec_receive_packet(ctx_, pkt_);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            check(r, "receive packet");
            {
                std::lock_guard lock(mutex_);
                ++stats_.packets;
                stats_.bytes += static_cast<std::uint64_t>(pkt_->size);
            }
            if (sink_) sink_(pkt_);
            av_packet_unref(pkt_);
        }
        if (sent != AVERROR(EAGAIN)) return;
    }
}

void VideoEncoder::finish() {
    {
        std::lock_guard lock(mutex_);
        if (finished_) return;
        finished_ = true;
        closing_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

std::string VideoEncoder::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

VideoEncoder::Stats VideoEncoder::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::unique_ptr<VideoEncoder> openVideoEncoder(const Device& dev, const VideoSettings& v, bool globalHeader,
                                               std::vector<std::string>* log) {
    // auto: the GPU encoders first, x264 last (the design doc: x264 when there is no GPU encoder).
    std::vector<std::string> families;
    if (v.encoder == "auto") families = {"nvenc", "amf", "qsv", "mf", "x264"};
    else families = {v.encoder};

    std::string errors;
    for (const auto& family : families) {
        if (family == "x264" && v.codec != "h264") {
            if (log) log->push_back("libx264: only encodes h264");
            errors += std::string(errors.empty() ? "" : "; ") + "libx264: only encodes h264";
            continue;
        }
        const std::string name = family == "x264" ? std::string("libx264") : v.codec + "_" + family;
        try {
            auto enc = std::make_unique<VideoEncoder>(dev, name, v, globalHeader);
            if (log) log->push_back(name + ": opened");
            return enc;
        } catch (const std::exception& e) {
            if (log) log->push_back(name + ": " + e.what());
            errors += (errors.empty() ? "" : "; ") + name + ": " + e.what();
        }
    }
    throw std::runtime_error("no " + v.codec + " encoder could be opened (" + errors + ")");
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

}  // namespace dc::capd
