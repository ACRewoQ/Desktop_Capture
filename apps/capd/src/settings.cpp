#include "settings.hpp"

#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace dc::capd {

namespace {

template <typename T>
void read(const proto::ParamTree& tree, const std::string& path, T& out) {
    if (auto v = tree.getAs<T>(path)) out = *v;
}

void readInt(const proto::ParamTree& tree, const std::string& path, int& out) {
    if (auto v = tree.getAs<std::int64_t>(path)) out = static_cast<int>(*v);
}

}  // namespace

std::optional<std::uint32_t> slotForSource(const std::string& source) {
    if (source == "master") return 0u;
    const std::string prefix = "track/";
    if (source.rfind(prefix, 0) != 0 || source.size() == prefix.size()) return std::nullopt;
    std::uint32_t n = 0;
    for (std::size_t i = prefix.size(); i < source.size(); ++i) {
        const char c = source[i];
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<std::uint32_t>(c - '0');
        if (n > 64) return std::nullopt;
    }
    if (n < 1 || source[prefix.size()] == '0') return std::nullopt;
    return n;
}

std::optional<RecordSettings> settingsFromTree(const proto::ParamTree& tree, std::string* error) {
    RecordSettings s;
    read(tree, "/capture/container", s.container);
    read(tree, "/capture/directory", s.directory);
    read(tree, "/capture/filename", s.filenamePattern);
    if (!s.directory.empty() &&
        !std::filesystem::path(std::u8string(s.directory.begin(), s.directory.end())).is_absolute()) {
        if (error) *error = "the recording folder must be an absolute path: " + s.directory;
        return std::nullopt;
    }

    VideoSettings& v = s.video;
    read(tree, "/capture/video/encoder", v.encoder);
    read(tree, "/capture/video/codec", v.codec);
    readInt(tree, "/capture/video/width", v.width);
    readInt(tree, "/capture/video/height", v.height);
    readInt(tree, "/capture/video/fps", v.fps);
    read(tree, "/capture/video/rate_control", v.rateControl);
    readInt(tree, "/capture/video/bitrate_kbps", v.bitrateKbps);
    readInt(tree, "/capture/video/quality", v.quality);
    v.width &= ~1;
    v.height &= ~1;

    for (int i = 1; i <= proto::kMaxCaptureAudioStreams; ++i) {
        const std::string base = "/capture/audio/" + std::to_string(i);
        std::string source;
        read(tree, base + "/source", source);
        const auto slot = slotForSource(source);
        if (!slot) continue;
        AudioStreamSettings a;
        a.slot = *slot;
        a.label = source;
        // Players list audio tracks by title: use the mixer's name for it ("Master", "Mic", "Game"), if it has one.
        read(tree, *slot == 0 ? std::string("/master/name") : "/track/" + std::to_string(*slot) + "/name", a.title);
        read(tree, base + "/codec", a.codec);
        readInt(tree, base + "/bitrate_kbps", a.bitrateKbps);
        s.audio.push_back(a);
    }

    int mix = 1;
    readInt(tree, "/capture/mix", mix);
    if (mix < 1 || mix > proto::kMaxVideoMixes) {
        if (error) *error = "no video mix assigned to the capture output (/capture/mix = 0)";
        return std::nullopt;
    }
    const std::string mb = "/video/mix/" + std::to_string(mix);
    readInt(tree, mb + "/width", s.mix.width);
    readInt(tree, mb + "/height", s.mix.height);
    readInt(tree, mb + "/fps", s.mix.fps);
    s.mix.width &= ~1;
    s.mix.height &= ~1;
    return s;
}

std::string defaultVideoDirectory() {
#ifdef _WIN32
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &path))) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            out.resize(static_cast<std::size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
        }
    }
    CoTaskMemFree(path);
    return out.empty() ? std::string(".") : out;
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Videos" : std::string(".");
#endif
}

std::string recordingPath(const RecordSettings& s, std::time_t when) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    char name[256] = {};
    std::string pattern = s.filenamePattern.empty() ? "%Y-%m-%d_%H-%M-%S" : s.filenamePattern;
    if (std::strftime(name, sizeof name, pattern.c_str(), &tm) == 0) std::strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", &tm);
    std::string file = name;
    for (char& c : file) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    std::string dir = s.directory.empty() ? defaultVideoDirectory() : s.directory;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir + file + "." + (s.container == "mp4" ? "mp4" : "mkv");
}

std::uint64_t diskReserveBytes(const RecordSettings& s) {
    std::uint64_t kbps = 0;
    if (s.withVideo) {
        // CQP has no bitrate: assume a generous one for the size (about 0.2 bit per pixel and frame).
        kbps += s.video.rateControl == "cqp"
                    ? static_cast<std::uint64_t>(double(s.video.width) * s.video.height * s.video.fps * 0.2 / 1000)
                    : static_cast<std::uint64_t>(std::max(0, s.video.bitrateKbps)) * 3 / 2;  // VBR peaks
    }
    for (const auto& a : s.audio) kbps += a.codec == "pcm" ? 2304 : static_cast<std::uint64_t>(std::max(0, a.bitrateKbps));
    const std::uint64_t thirtySeconds = kbps * 1000 / 8 * 30;
    return std::max<std::uint64_t>(thirtySeconds, 256ull << 20);
}

std::string effectiveCodec(const VideoSettings& v, bool* changed) {
    std::string codec = v.codec;
    if (codec == "h264" && static_cast<std::int64_t>(v.width) * v.height > 2560LL * 1440LL) codec = "hevc";
    if (changed) *changed = codec != v.codec;
    return codec;
}

}  // namespace dc::capd
