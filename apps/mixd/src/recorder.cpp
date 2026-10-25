#include "recorder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace dc::mixd {

std::shared_ptr<RecorderTake> RecorderTake::create(const std::filesystem::path& folder, AudioFileFormat format,
                                                   std::vector<RecordStreamSpec> specs, std::string* error) {
    std::shared_ptr<RecorderTake> take(new RecorderTake());
    take->folder_ = folder;
    const auto capacity = static_cast<std::size_t>(kFifoSeconds * 48000);
    for (const auto& s : specs) {
        auto st = std::make_unique<Stream>(s.channels, capacity);
        st->file = createAudioFile(folder / pathFromUtf8(s.fileName), format, s.channels, error);
        if (!st->file) {
            take->streams_.clear(); // closes the files created so far
            return nullptr;
        }
        take->streams_.push_back(std::move(st));
    }
    take->specs_ = std::move(specs);
    take->thread_ = std::thread([t = take.get()] { t->run(); });
    return take;
}

RecorderTake::~RecorderTake() { finish(); }

void RecorderTake::push(int stream, const float* interleaved, int frames) {
    auto& s = *streams_[static_cast<std::size_t>(stream)];
    const auto n = static_cast<std::size_t>(frames);
    if (s.fifo.capacity() - s.fifo.available() < n) {
        s.lost.fetch_add(n, std::memory_order_relaxed); // keep whole blocks together
        return;
    }
    s.fifo.push(interleaved, n);
}

void RecorderTake::pushSilence(int stream, int frames) {
    static const std::array<float, kMaxBlockFrames * 2> zeros{};
    push(stream, zeros.data(), std::min(frames, kMaxBlockFrames));
}

std::uint64_t RecorderTake::dropped() const {
    std::uint64_t most = 0;
    for (const auto& s : streams_) most = std::max(most, s->dropped.load(std::memory_order_relaxed));
    return most;
}

std::uint64_t RecorderTake::fileFrames() const {
    std::uint64_t most = 0;
    for (const auto& s : streams_) most = std::max(most, s->file->frames());
    return most;
}

std::string RecorderTake::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

void RecorderTake::fail(const std::string& message) {
    {
        std::lock_guard lock(errorMutex_);
        if (error_.empty()) error_ = message;
    }
    failed_.store(true, std::memory_order_release);
}

bool RecorderTake::drain(std::vector<float>& scratch) {
    bool any = false;
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        auto& s = *streams_[i];
        const auto chunk = scratch.size() / static_cast<std::size_t>(s.fifo.channels());
        for (;;) {
            const auto n = s.fifo.pop(scratch.data(), chunk);
            if (n == 0) break;
            any = true;
            if (!s.failed && !s.file->write(scratch.data(), n)) {
                s.failed = true;
                fail(specs_[i].fileName + ": " + s.file->error());
            }
        }
        // what did not fit into the FIFO comes after what is in it (approximately: the position is
        // known to within the FIFO's fill level when it overflowed); silence keeps the length
        if (const auto lost = s.lost.exchange(0, std::memory_order_relaxed); lost > 0) {
            any = true;
            s.dropped.fetch_add(lost, std::memory_order_relaxed);
            if (!s.failed && !s.file->writeSilence(static_cast<std::size_t>(lost))) {
                s.failed = true;
                fail(specs_[i].fileName + ": " + s.file->error());
            }
        }
    }
    return any;
}

void RecorderTake::run() {
    std::vector<float> scratch(8192 * 2);
    while (!stop_.load(std::memory_order_acquire)) {
        if (!drain(scratch)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    while (drain(scratch)) {
    }
}

bool RecorderTake::finish() {
    if (finished_) return finishOk_;
    finished_ = true;
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        auto& s = *streams_[i];
        if (!s.file->close() && !s.failed) {
            s.failed = true;
            fail(specs_[i].fileName + ": " + s.file->error());
        }
    }
    finishOk_ = !failed();
    return finishOk_;
}

std::string defaultRecorderDirectory() {
#ifdef _WIN32
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    if (out.empty()) out = ".";
    return pathToUtf8(out / "Desktop Capture");
#else
    const char* home = std::getenv("HOME");
    return (home ? std::string(home) : std::string(".")) + "/Music/Desktop Capture";
#endif
}

std::string sanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        const bool bad = u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                         c == '?' || c == '*';
        out += bad ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back(); // Windows drops these
    return out;
}

} // namespace dc::mixd
