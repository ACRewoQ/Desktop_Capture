// Writes encoded video and audio packets into one MKV/MP4 file on its own thread, so a slow disk never stalls the
// encoders. A write error (disk full, drive removed) puts the muxer into the failed state; the recorder then stops
// the recording safely (design doc: "保存先のディスク").
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVPacket;

namespace dc::capd {

class Muxer {
public:
    // container: "mkv" or "mp4". Nothing is written until start().
    Muxer(std::string path, const std::string& container);
    ~Muxer();
    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // Whether encoders must put codec headers in extradata (AV_CODEC_FLAG_GLOBAL_HEADER).
    bool needsGlobalHeader() const;

    // Adds a stream for an opened encoder. Call before start(). Returns the stream index.
    int addStream(const AVCodecContext* encoder, const char* title = nullptr);

    // Opens the file, writes the header and starts the writing thread. Throws on failure.
    void start();

    // Queues a packet (its data is moved out; pkt stays valid and empty). Timestamps are in the encoder's
    // time base given to addStream(). Ignored after a failure.
    void write(int stream, AVPacket* pkt);

    // Writes everything queued, the trailer, and closes the file. Safe to call more than once.
    void finish();

    bool failed() const { return failed_.load(); }
    std::string error() const;
    std::uint64_t bytesWritten() const { return bytes_.load(); }
    const std::string& path() const { return path_; }

private:
    void run();
    bool isMp4() const;
    void fail(const std::string& what);

    std::string path_;
    AVFormatContext* fmt_ = nullptr;
    struct StreamInfo {
        int encNum = 1, encDen = 1;
    };
    std::deque<StreamInfo> streams_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<int, AVPacket*>> queue_;
    bool closing_ = false;
    bool started_ = false;
    bool finished_ = false;
    std::thread thread_;

    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> bytes_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

// FFmpeg's text for an error code.
std::string avError(int err);

}  // namespace dc::capd
