#include "recorder.hpp"

#include "audio_capture.hpp"
#include "audio_encoder.hpp"
#include "muxer.hpp"

#include "dc/shmaudio/clock.hpp"

#ifdef _WIN32
#include "converter.hpp"
#include "video_encoder.hpp"
#include "video_mix.hpp"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace dc::capd {

namespace {

std::optional<std::uint64_t> filesystemFreeSpace(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path();
    if (p.empty()) p = ".";
    const auto info = std::filesystem::space(p, ec);
    if (ec) return std::nullopt;
    return static_cast<std::uint64_t>(info.available);
}

std::string megabytes(std::uint64_t bytes) { return std::to_string(bytes >> 20) + " MB"; }

}  // namespace

#ifdef _WIN32
Recorder::Recorder(const RecordSettings& s, VideoMix* mix, std::string path, std::string shmSuffix)
    : settings_(s), path_(std::move(path)), shmSuffix_(std::move(shmSuffix)), mix_(mix) {}
#else
Recorder::Recorder(const RecordSettings& s, std::string path, std::string shmSuffix)
    : settings_(s), path_(std::move(path)), shmSuffix_(std::move(shmSuffix)) {}
#endif

Recorder::~Recorder() {
    if (running_) {
        try {
            stop();
        } catch (...) {
        }
    }
}

void Recorder::start() {
    if (running_) throw std::runtime_error("already recording");
    bool withVideo = false;
#ifdef _WIN32
    withVideo = settings_.withVideo && mix_ != nullptr;
#endif
    if (!withVideo && settings_.audio.empty()) throw std::runtime_error("nothing to record (no video mix, no audio)");

    std::error_code ec;
    const auto dir = std::filesystem::path(std::u8string(path_.begin(), path_.end())).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    if (ec) {
        const auto u8 = dir.u8string();
        throw std::runtime_error("cannot create folder " + std::string(u8.begin(), u8.end()) + ": " + ec.message());
    }

    if (!freeSpace_) freeSpace_ = filesystemFreeSpace;
    reserveBytes_ = diskReserveBytes(settings_);
    if (const auto free = freeSpace_(path_); free && *free < 2 * reserveBytes_) {
        throw std::runtime_error("not enough free disk space for recording (" + megabytes(*free) + " free, " +
                                 megabytes(2 * reserveBytes_) + " needed)");
    }

    muxer_ = std::make_unique<Muxer>(path_, settings_.container);
    const bool globalHeader = muxer_->needsGlobalHeader();

#ifdef _WIN32
    if (withVideo) {
        VideoSettings v = settings_.video;
        bool changed = false;
        v.codec = effectiveCodec(v, &changed);
        if (changed) log_.push_back("H.264 above 2560x1440 uses most of the encoder; recording in HEVC instead");
        videoEncoder_ = openVideoEncoder(mix_->device(), v, globalHeader, &log_);
        converter_ = std::make_unique<Converter>(mix_->device(), static_cast<UINT>(v.width),
                                                 static_cast<UINT>(v.height), static_cast<UINT>(v.fps));
        videoStream_ = muxer_->addStream(videoEncoder_->context(), "video");
        log_.push_back("video: " + videoEncoder_->description() + ", mix " + std::to_string(mix_->settings().width) +
                       "x" + std::to_string(mix_->settings().height) + "@" + std::to_string(mix_->settings().fps) +
                       " from " + mix_->sourceDescription());
    }
#endif

    std::vector<std::uint32_t> slots;
    for (const auto& a : settings_.audio) {
        auto enc = std::make_unique<AudioEncoder>(a, globalHeader);
        audioStreams_.push_back(muxer_->addStream(enc->context(), a.label.c_str()));
        log_.push_back("audio " + std::to_string(audioStreams_.size()) + ": " + a.label + " -> " + enc->description());
        audioEncoders_.push_back(std::move(enc));
        slots.push_back(a.slot);
    }
    for (std::size_t i = 0; i < audioEncoders_.size(); ++i) {
        const int stream = audioStreams_[i];
        audioEncoders_[i]->setSink([this, stream](AVPacket* p) { muxer_->write(stream, p); });
    }

    muxer_->start();
    startQpc_ = shm::nowQpc();
    running_ = true;

#ifdef _WIN32
    if (withVideo) {
        videoEncoder_->start([this](AVPacket* p) { muxer_->write(videoStream_, p); });
        const std::int64_t freq = shm::qpcFrequency();
        const std::int64_t fps = settings_.video.fps;
        const auto keyEvery = static_cast<std::int64_t>(settings_.forceKeyframeSeconds * double(fps));
        consumerId_ = mix_->addConsumer([this, freq, fps, keyEvery](ID3D11Texture2D* canvas, std::int64_t,
                                                                    std::int64_t qpc) {
            if (qpc < startQpc_) return;
            // Frame number on the recording's time line; a mix faster than the recording skips ticks.
            const auto pts = static_cast<std::int64_t>(
                std::floor(double(qpc - startQpc_) * double(fps) / double(freq) + 0.5));
            if (pts <= lastPts_) return;
            auto surface = videoEncoder_->acquire();
            if (!surface.frame) return;  // encoder behind: this frame is dropped (counted by the encoder)
            converter_->convert(canvas, surface.texture, surface.arrayIndex);
            // Forced keyframes land between the regular ones (GOP 2 s) so "capd probe" can count them.
            const bool key = keyEvery > 0 && pts > 0 && lastPts_ / keyEvery != pts / keyEvery;
            videoEncoder_->submit(surface, pts, key);
            lastPts_ = pts;
            ++videoFrames_;
        });
    }
#endif

    if (!slots.empty()) {
        audio_ = std::make_unique<AudioCapture>(slots, shmSuffix_);
        audio_->start(startQpc_, [this](const std::vector<std::span<const float>>& streams, std::uint32_t frames) {
            onAudio(streams, frames);
        });
    }
}

void Recorder::onAudio(const std::vector<std::span<const float>>& streams, std::uint32_t frames) {
    try {
        for (std::size_t i = 0; i < streams.size() && i < audioEncoders_.size(); ++i)
            audioEncoders_[i]->push(streams[i], frames);
    } catch (const std::exception& e) {
        std::lock_guard lock(audioErrorMutex_);
        if (audioError_.empty()) audioError_ = std::string("audio: ") + e.what();
    }
}

Recorder::Status Recorder::stop() {
    if (!running_) return status();
    stopQpc_ = shm::nowQpc();
    std::string finishError;
#ifdef _WIN32
    if (consumerId_) {
        mix_->removeConsumer(consumerId_);
        consumerId_ = 0;
    }
    if (videoEncoder_) videoEncoder_->finish();
#endif
    if (audio_) audio_->stop(stopQpc_);
    for (auto& enc : audioEncoders_) {
        try {
            enc->finish();
        } catch (const std::exception& e) {
            if (finishError.empty()) finishError = std::string("audio: ") + e.what();
        }
    }
    if (muxer_) muxer_->finish();
    running_ = false;
    Status s = status();
    if (s.error.empty()) s.error = finishError;
    return s;
}

Recorder::Status Recorder::status() const {
    Status s;
    s.file = path_;
    const std::int64_t end = running_ ? shm::nowQpc() : stopQpc_;
    if (startQpc_) s.elapsedMs = (end - startQpc_) * 1000 / shm::qpcFrequency();
    if (muxer_) {
        s.bytes = muxer_->bytesWritten();
        if (muxer_->failed()) s.error = muxer_->error();
    }
#ifdef _WIN32
    s.videoFrames = videoFrames_.load();
    if (videoEncoder_) {
        s.videoDropped = videoEncoder_->stats().poolEmpty;
        if (s.error.empty()) s.error = videoEncoder_->error();
    }
    if (mix_ && s.error.empty()) s.error = mix_->error();
#endif
    if (audio_) {
        const auto a = audio_->stats();
        s.audioFrames = a.frames;
        s.audioSilenceFrames = a.silenceFrames;
        s.engineConnected = a.engineConnected;
        s.audioDriftMs = a.driftMs;
    }
    if (s.error.empty()) {
        std::lock_guard lock(audioErrorMutex_);
        s.error = audioError_;
    }
    {
        std::lock_guard lock(diskMutex_);
        const std::int64_t now = shm::nowQpc();
        if (running_ && freeSpace_ && now >= nextDiskCheckQpc_) {
            nextDiskCheckQpc_ = now + shm::qpcFrequency();
            if (const auto free = freeSpace_(path_)) {
                diskFree_ = *free;
                if (*free < reserveBytes_ && diskError_.empty())
                    diskError_ = "disk almost full (" + megabytes(*free) + " free): recording stopped";
            }
        }
        s.diskFreeBytes = diskFree_;
        if (s.error.empty()) s.error = diskError_;
    }
    return s;
}

}  // namespace dc::capd
