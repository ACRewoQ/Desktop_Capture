// Recording settings as capd uses them, and their conversion from the engine's parameter tree
// (/capture/*, /video/mix/#/*). Portable: no Windows or FFmpeg types.
#pragma once

#include "dc/protocol/param_tree.hpp"

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace dc::capd {

struct VideoSettings {
    std::string encoder = "auto";  // auto | nvenc | amf | qsv | mf | x264
    std::string codec = "h264";    // h264 | hevc | av1
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::string rateControl = "vbr";  // cbr | vbr | cqp
    int bitrateKbps = 20000;
    int quality = 23;  // cqp (x264: crf)
    int gopFrames = 0;  // frames between keyframes; 0 = 2 seconds
    bool lowLatency = false;  // preview streams: the encoders' low-latency tuning instead of quality tuning
};

struct AudioStreamSettings {
    std::uint32_t slot = 0;      // shared-memory slot: 0 = master, n = track n
    std::string label;           // "master", "track/3"
    std::string codec = "aac";   // aac | opus | pcm
    int bitrateKbps = 192;
};

// One video mix with one scene. P2 has a single layer (a whole monitor); scenes and layers come in P3 (B1).
struct MixSettings {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::string source = "wgc";  // wgc | dda
    int monitor = -1;            // index from "capd list"; -1 = primary
};

struct RecordSettings {
    std::string container = "mkv";  // mkv | mp4
    std::string directory;          // "" = the user's Videos folder
    std::string filenamePattern = "%Y-%m-%d_%H-%M-%S";
    VideoSettings video;
    std::vector<AudioStreamSettings> audio;  // in stream order; empty entries are left out
    MixSettings mix;
    bool withVideo = true;  // false = audio only (Linux tests, or no mix assigned)
    double forceKeyframeSeconds = 0;  // tests ("capd record --force-keyframes"): request a keyframe this often
};

// Parses "master" / "track/<n>" into a shared-memory slot. "" and anything else give nullopt.
std::optional<std::uint32_t> slotForSource(const std::string& source);

// Reads the recording settings from the engine's tree. Missing values keep the defaults above.
// Returns nullopt with a reason when the tree asks for something capd cannot record (no mix assigned).
std::optional<RecordSettings> settingsFromTree(const proto::ParamTree& tree, std::string* error);

// Folder for recordings when /capture/directory is "": the Videos known folder (Windows) or ~/Videos.
std::string defaultVideoDirectory();

// directory + strftime(pattern, time) + "." + container. Characters Windows forbids in file names are replaced.
std::string recordingPath(const RecordSettings& s, std::time_t when);

// Free space a recording keeps in reserve on its drive: 30 seconds at the recording's total bitrate, at least
// 256 MB. Below it the recording stops on its own and finalizes the file (an MP4 is only playable once its
// index is written at the end, so it must not run into a full disk). Starting needs twice the reserve.
std::uint64_t diskReserveBytes(const RecordSettings& s);

// The H.264 encoder on Reo's RTX 5090 uses 57% of NVENC at 4K60 (V3). Above 2560x1440, H.264 is switched to HEVC.
// Returns the codec to use and sets *changed when it differs from the request.
std::string effectiveCodec(const VideoSettings& v, bool* changed = nullptr);

}  // namespace dc::capd
