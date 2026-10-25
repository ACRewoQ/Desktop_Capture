#include "avsync.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace dc::capd {

std::vector<double> findFlashes(const std::vector<double>& times, const std::vector<float>& brightness,
                                double minContrast) {
    std::vector<double> out;
    const std::size_t n = std::min(times.size(), brightness.size());
    if (n < 2) return out;
    const auto [lo, hi] = std::minmax_element(brightness.begin(), brightness.begin() + static_cast<std::ptrdiff_t>(n));
    if (*hi - *lo < minContrast) return out;
    const float mid = (*lo + *hi) / 2;
    bool above = brightness[0] > mid;
    for (std::size_t i = 1; i < n; ++i) {
        const bool now = brightness[i] > mid;
        if (now && !above) out.push_back(times[i]);
        above = now;
    }
    return out;
}

std::vector<double> findPulses(const std::vector<float>& samples, double startSec, int sampleRate, double quietSec) {
    std::vector<double> out;
    float peak = 0;
    for (float v : samples) peak = std::max(peak, std::fabs(v));
    if (peak <= 1e-4f) return out;
    const float threshold = 0.3f * peak;
    const auto quiet = static_cast<std::size_t>(quietSec * sampleRate);
    std::size_t sinceLoud = 0; // a burst cut by the start of the file does not count
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (std::fabs(samples[i]) > threshold) {
            if (sinceLoud >= quiet) out.push_back(startSec + static_cast<double>(i) / sampleRate);
            sinceLoud = 0;
        } else {
            ++sinceLoud;
        }
    }
    return out;
}

std::vector<AvSyncPair> pairEvents(const std::vector<double>& flashes, const std::vector<double>& pulses,
                                   double maxPairSec) {
    std::vector<AvSyncPair> out;
    std::vector<bool> used(pulses.size(), false);
    for (double f : flashes) {
        // pulses are in time order: look around the first one at or after the flash
        const auto it = std::lower_bound(pulses.begin(), pulses.end(), f);
        std::ptrdiff_t best = -1;
        double bestDist = maxPairSec;
        for (auto j = it - pulses.begin() - 1; j <= it - pulses.begin(); ++j) {
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(pulses.size()) || used[static_cast<std::size_t>(j)]) continue;
            const double d = std::fabs(pulses[static_cast<std::size_t>(j)] - f);
            if (d <= bestDist) {
                bestDist = d;
                best = j;
            }
        }
        if (best < 0) continue;
        used[static_cast<std::size_t>(best)] = true;
        out.push_back({f, pulses[static_cast<std::size_t>(best)]});
    }
    return out;
}

namespace {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

} // namespace

AvSyncStats computeStats(const std::vector<AvSyncPair>& pairs) {
    AvSyncStats s;
    s.pairs = static_cast<int>(pairs.size());
    if (pairs.empty()) return s;
    std::vector<double> ms;
    ms.reserve(pairs.size());
    for (const auto& p : pairs) ms.push_back(p.offsetMs());
    double sum = 0;
    for (double v : ms) sum += v;
    s.meanMs = sum / static_cast<double>(ms.size());
    s.medianMs = median(ms);
    const auto [lo, hi] = std::minmax_element(ms.begin(), ms.end());
    s.minMs = *lo;
    s.maxMs = *hi;
    const std::size_t tenth = std::max<std::size_t>(1, ms.size() / 10);
    s.driftMs = median({ms.end() - static_cast<std::ptrdiff_t>(tenth), ms.end()}) -
                median({ms.begin(), ms.begin() + static_cast<std::ptrdiff_t>(tenth)});
    return s;
}

// --- reading a file ------------------------------------------------------------------------------

namespace {

struct FormatCloser {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
struct CodecFreer {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameFreer {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketFreer {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};

std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

std::unique_ptr<AVCodecContext, CodecFreer> openDecoder(const AVStream* st, std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        error = std::string("no decoder for ") + avcodec_get_name(st->codecpar->codec_id);
        return nullptr;
    }
    std::unique_ptr<AVCodecContext, CodecFreer> ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (const int e = avcodec_open2(ctx.get(), codec, nullptr); e < 0) {
        error = std::string("cannot open decoder ") + codec->name + ": " + averr(e);
        return nullptr;
    }
    return ctx;
}

// Mean of the luma plane over the region, 0..1.
float regionBrightness(const AVFrame* f, const AvSyncOptions& o) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
    if (!d || (d->flags & AV_PIX_FMT_FLAG_RGB) || !f->data[0]) return 0;
    const int depth = d->comp[0].depth;
    const int step = d->comp[0].step; // bytes per luma sample in plane 0 (1, or 2 for 10-bit)
    const int x0 = static_cast<int>(o.regionX * f->width), y0 = static_cast<int>(o.regionY * f->height);
    const int x1 = std::max(x0 + 1, static_cast<int>((o.regionX + o.regionW) * f->width));
    const int y1 = std::max(y0 + 1, static_cast<int>((o.regionY + o.regionH) * f->height));
    double sum = 0;
    std::int64_t count = 0;
    for (int y = y0; y < std::min(y1, f->height); y += 2) {
        const std::uint8_t* row = f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0];
        for (int x = x0; x < std::min(x1, f->width); x += 2) {
            const std::uint8_t* p = row + static_cast<std::ptrdiff_t>(x) * step;
            unsigned v = p[0];
            if (step == 2) {
                std::uint16_t w;
                std::memcpy(&w, p, 2);
                v = static_cast<unsigned>(w >> d->comp[0].shift);
            }
            sum += v;
            ++count;
        }
    }
    return count ? static_cast<float>(sum / static_cast<double>(count) / ((1 << depth) - 1)) : 0.0f;
}

// Channel 0 of a decoded audio frame as float.
void appendChannel0(const AVFrame* f, std::vector<float>& out) {
    const int n = f->nb_samples;
    const int channels = f->ch_layout.nb_channels;
    const auto fmt = static_cast<AVSampleFormat>(f->format);
    const bool planar = av_sample_fmt_is_planar(fmt);
    const int stride = planar ? 1 : channels;
    const std::uint8_t* p = f->data[0];
    for (int i = 0; i < n; ++i) {
        float v = 0;
        const int k = i * stride;
        switch (av_get_packed_sample_fmt(fmt)) {
        case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float*>(p)[k]; break;
        case AV_SAMPLE_FMT_DBL: v = static_cast<float>(reinterpret_cast<const double*>(p)[k]); break;
        case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const std::int16_t*>(p)[k] / 32768.0f; break;
        case AV_SAMPLE_FMT_S32: v = static_cast<float>(reinterpret_cast<const std::int32_t*>(p)[k] / 2147483648.0); break;
        case AV_SAMPLE_FMT_U8: v = (p[k] - 128) / 128.0f; break;
        default: break;
        }
        out.push_back(v);
    }
}

} // namespace

AvSyncReport analyzeAvSync(const std::string& file, const AvSyncOptions& o) {
    AvSyncReport r;
    AVFormatContext* raw = nullptr;
    if (const int e = avformat_open_input(&raw, file.c_str(), nullptr, nullptr); e < 0) {
        r.error = "cannot open " + file + ": " + averr(e);
        return r;
    }
    std::unique_ptr<AVFormatContext, FormatCloser> fmt(raw);
    avformat_find_stream_info(fmt.get(), nullptr);

    int videoIndex = -1, audioIndex = -1, audioSeen = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const auto type = fmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoIndex < 0) videoIndex = static_cast<int>(i);
        if (type == AVMEDIA_TYPE_AUDIO && audioIndex < 0 && audioSeen++ == o.audioStream) audioIndex = static_cast<int>(i);
    }
    if (videoIndex < 0 || audioIndex < 0) {
        r.error = videoIndex < 0 ? "no video stream" : "no audio stream " + std::to_string(o.audioStream);
        return r;
    }
    const AVStream* vs = fmt->streams[videoIndex];
    const AVStream* as = fmt->streams[audioIndex];
    auto vdec = openDecoder(vs, r.error);
    if (!vdec) return r;
    auto adec = openDecoder(as, r.error);
    if (!adec) return r;
    if (fmt->duration > 0) r.durationSec = static_cast<double>(fmt->duration) / AV_TIME_BASE;

    std::vector<double> vTimes;
    std::vector<float> vBright;
    // Audio is kept as runs that start at a frame's timestamp; a gap in timestamps starts a new run.
    // Matroska stores milliseconds, so frames up to 3 ms off the run's sample count still join it.
    struct Run {
        double start = 0;
        std::vector<float> samples;
    };
    std::vector<Run> runs;
    int audioRate = adec->sample_rate > 0 ? adec->sample_rate : 48000;

    std::unique_ptr<AVPacket, PacketFreer> pkt(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameFreer> frame(av_frame_alloc());
    auto drain = [&](AVCodecContext* ctx, bool video) {
        while (avcodec_receive_frame(ctx, frame.get()) == 0) {
            const std::int64_t ts = frame->best_effort_timestamp;
            const AVRational tb = video ? vs->time_base : as->time_base;
            if (ts == AV_NOPTS_VALUE) {
                av_frame_unref(frame.get());
                continue;
            }
            const double t = static_cast<double>(ts) * av_q2d(tb);
            if (video) {
                vTimes.push_back(t);
                vBright.push_back(regionBrightness(frame.get(), o));
                ++r.videoFrames;
            } else {
                audioRate = frame->sample_rate > 0 ? frame->sample_rate : audioRate;
                const bool joins = !runs.empty() &&
                                   std::fabs(runs.back().start + static_cast<double>(runs.back().samples.size()) / audioRate - t) <
                                       0.003;
                if (!joins) runs.push_back({t, {}});
                appendChannel0(frame.get(), runs.back().samples);
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == videoIndex) {
            if (avcodec_send_packet(vdec.get(), pkt.get()) == 0) drain(vdec.get(), true);
        } else if (pkt->stream_index == audioIndex) {
            if (avcodec_send_packet(adec.get(), pkt.get()) == 0) drain(adec.get(), false);
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(vdec.get(), nullptr);
    drain(vdec.get(), true);
    avcodec_send_packet(adec.get(), nullptr);
    drain(adec.get(), false);

    // Frames can come out of the decoder in presentation order only after reordering: sort by time.
    std::vector<std::size_t> order(vTimes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return vTimes[a] < vTimes[b]; });
    std::vector<double> t2;
    std::vector<float> b2;
    for (std::size_t i : order) {
        t2.push_back(vTimes[i]);
        b2.push_back(vBright[i]);
    }
    r.flashes = findFlashes(t2, b2);
    for (const auto& run : runs) {
        auto p = findPulses(run.samples, run.start, audioRate);
        r.pulses.insert(r.pulses.end(), p.begin(), p.end());
    }
    std::sort(r.pulses.begin(), r.pulses.end());
    r.pairs = pairEvents(r.flashes, r.pulses, o.maxPairSec);
    r.stats = computeStats(r.pairs);
    if (r.durationSec <= 0 && !t2.empty()) r.durationSec = t2.back();
    return r;
}

} // namespace dc::capd
