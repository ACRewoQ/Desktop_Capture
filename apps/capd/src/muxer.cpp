#include "muxer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <stdexcept>

namespace dc::capd {

std::string avError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

Muxer::Muxer(std::string path, const std::string& container) : path_(std::move(path)) {
    const char* format = container == "mp4" ? "mp4" : "matroska";
    const int r = avformat_alloc_output_context2(&fmt_, nullptr, format, path_.c_str());
    if (r < 0 || !fmt_) throw std::runtime_error("output format " + container + ": " + avError(r));
}

Muxer::~Muxer() {
    try {
        finish();
    } catch (...) {
    }
    if (fmt_) {
        if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
    }
    for (auto& [s, p] : queue_) av_packet_free(&p);
}

bool Muxer::needsGlobalHeader() const { return (fmt_->oformat->flags & AVFMT_GLOBALHEADER) != 0; }

int Muxer::addStream(const AVCodecContext* encoder, const char* title) {
    AVStream* st = avformat_new_stream(fmt_, nullptr);
    if (!st) throw std::runtime_error("avformat_new_stream failed");
    const int r = avcodec_parameters_from_context(st->codecpar, encoder);
    if (r < 0) throw std::runtime_error("codec parameters: " + avError(r));
    st->time_base = encoder->time_base;
    if (encoder->codec_type == AVMEDIA_TYPE_VIDEO) st->avg_frame_rate = encoder->framerate;
    if (title) av_dict_set(&st->metadata, "title", title, 0);
    streams_.push_back({encoder->time_base.num, encoder->time_base.den});
    return st->index;
}

void Muxer::start() {
    if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
        const int r = avio_open(&fmt_->pb, path_.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) throw std::runtime_error("open " + path_ + ": " + avError(r));
    }
    AVDictionary* opts = nullptr;
    // MP4: write the index at the front when finishing (not crash-safe, unlike MKV).
    if (std::string(fmt_->oformat->name) == "mp4") av_dict_set(&opts, "movflags", "+faststart", 0);
    const int r = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (r < 0) throw std::runtime_error("write header: " + avError(r));
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void Muxer::write(int stream, AVPacket* pkt) {
    if (failed_ || stream < 0 || stream >= static_cast<int>(streams_.size())) {
        av_packet_unref(pkt);
        return;
    }
    AVPacket* copy = av_packet_alloc();
    av_packet_move_ref(copy, pkt);
    {
        std::lock_guard lock(mutex_);
        if (closing_) {
            av_packet_free(&copy);
            return;
        }
        queue_.emplace_back(stream, copy);
    }
    cv_.notify_one();
}

void Muxer::run() {
    for (;;) {
        std::pair<int, AVPacket*> item{-1, nullptr};
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) return;  // closing and drained
            item = queue_.front();
            queue_.pop_front();
        }
        auto [stream, pkt] = item;
        if (!failed_) {
            const StreamInfo& si = streams_[static_cast<std::size_t>(stream)];
            av_packet_rescale_ts(pkt, AVRational{si.encNum, si.encDen}, fmt_->streams[stream]->time_base);
            pkt->stream_index = stream;
            const int size = pkt->size;
            const int r = av_interleaved_write_frame(fmt_, pkt);
            if (r < 0) fail("write packet: " + avError(r));
            else if (fmt_->pb && fmt_->pb->error < 0) fail("write: " + avError(fmt_->pb->error));
            else bytes_ += static_cast<std::uint64_t>(size);
        }
        av_packet_free(&pkt);
    }
}

void Muxer::fail(const std::string& what) {
    {
        std::lock_guard lock(errorMutex_);
        if (error_.empty()) error_ = what;
    }
    failed_ = true;
}

std::string Muxer::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

void Muxer::finish() {
    if (finished_) return;
    finished_ = true;
    if (!started_) return;
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    const int r = av_write_trailer(fmt_);
    if (r < 0) fail("write trailer: " + avError(r));
    if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) {
        avio_flush(fmt_->pb);
        if (fmt_->pb->error < 0) fail("close: " + avError(fmt_->pb->error));
        avio_closep(&fmt_->pb);
    }
}

}  // namespace dc::capd
