// capd: capture process. Runs the video mixer (sources, scenes, layers, up to 4 mixes; lane B1) from the engine's
// tree and records a mix together with the engine's audio from shared memory into MKV/MP4.
//
//   capd [serve] [--host H] [--port N]   normal mode: follow the engine's /capture settings and record start/stop
//   capd --version
//   capd list                     monitors, windows, cameras and the hardware encoders that open on this PC (Windows)
//   capd record [options]         record until Ctrl+C or --seconds
//   capd mixtest [--out DIR] [--media FILE]   video mixer self-test (Windows)
//   capd probe FILE               streams of a recording: codec, packets, first/last time (checks A/V alignment)
//

#include "src/engine_follower.hpp"
#include "src/recorder.hpp"
#include "src/settings.hpp"

#ifdef _WIN32
#include "src/mix_test.hpp"
#include "src/video_encoder.hpp"
#include "src/video_mix.hpp"
#include "src/video_source.hpp"

#include <mfapi.h>
#endif

#include "dc/protocol/defaults.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/shmaudio/layout.hpp"
#include "dc/version.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dc::capd;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

void usage() {
    std::cout << R"(capd - capture process (P2 skeleton)

usage:
  capd [serve] [--host H] [--port N] [--no-video]   follow the engine (default)
  capd --version
  capd list
  capd record [options]
  capd probe FILE
  capd mixtest [--out DIR] [--media FILE]   video mixer self-test: pixels of scenes, transitions, sources

record options:
  --seconds S             stop after S seconds (default: run until Ctrl+C)
  --output FILE           file to write (default: <dir>/<date_time>.<container>)
  --dir DIR               folder (default: the Videos folder)
  --container mkv|mp4     default mkv
  --encoder auto|nvenc|amf|qsv|mf|x264   default auto (NVENC, AMF, QSV, Media Foundation, x264: first that opens)
  --codec h264|hevc|av1   default h264 (above 2560x1440 H.264 is recorded as HEVC)
  --size WxH              recorded size (default 1920x1080)
  --fps N                 recorded frame rate (default 60)
  --rc cbr|vbr|cqp        rate control (default vbr)
  --bitrate KBPS          default 20000
  --quality Q             cqp quality 0-51 (default 23)
  --gop N                 frames between keyframes (default: 2 seconds)
  --low-latency           the encoder's low-latency tuning (as the preview stream uses it)
  --force-keyframes S     request a keyframe every S seconds (tests the preview's keyframe request)
  --mix-size WxH          video mix (canvas) size (default: the recorded size)
  --mix-fps N             video mix frame rate (default: the recorded rate)
  --source wgc|dda        capture API (default wgc; dda = Desktop Duplication, avoids the Win10 yellow border)
  --monitor N             monitor number from "capd list" (default: primary)
  --audio LIST            audio streams, comma separated: master, track/<n> (default master; "none" = no audio)
  --audio-codec aac|opus|pcm   default aac
  --audio-bitrate KBPS    default 192
  --no-video              audio only
  --shm-suffix S          shared-memory name suffix (tests)
)";
}

bool parseSize(const std::string& s, int& w, int& h) {
    return std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) out.push_back(item);
    return out;
}

#ifdef _WIN32
int cmdList() {
    std::cout << "capd " << dc::kVersionString << ", FFmpeg " << ffmpegVersion() << "\n\nmonitors:\n";
    const auto displays = listDisplays();
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        std::cout << "  [" << i << "] " << d.name << "  " << d.gdiName << "  " << d.width << "x" << d.height
                  << (d.primary ? "  primary" : "") << "\n      id " << d.id << "\n";
    }
    std::cout << "\nwindows:\n";
    for (const auto& w : listWindows()) std::cout << "  " << w.exe << "  \"" << w.title << "\"\n";
    std::cout << "\ncameras:\n";
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        for (const auto& c : listCameras()) std::cout << "  " << c.name << "\n      id " << c.id << "\n";
        MFShutdown();
    }
    std::cout << "\nencoders (opened at 1280x720 on the primary monitor's GPU):\n";
    const auto primary = findMonitor("");
    if (!primary) throw std::runtime_error("no monitor attached to the desktop");
    const Device dev = createDevice(primary->adapter.Get());
    auto names = availableHwEncoders();
    names.emplace_back("libx264");
    for (const auto& name : names) {
        VideoSettings v;
        v.width = 1280;
        v.height = 720;
        v.bitrateKbps = 2000;
        std::string status = "works";
        try {
            VideoEncoder enc(dev, name, v, true);
        } catch (const std::exception& e) {
            status = std::string("unavailable: ") + e.what();
        }
        std::cout << "  " << name << "  " << status << "\n";
    }
    return 0;
}
#endif

int cmdProbe(const std::string& file) {
    AVFormatContext* fmt = nullptr;
    int r = avformat_open_input(&fmt, file.c_str(), nullptr, nullptr);
    if (r < 0) throw std::runtime_error("open " + file + " failed");
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0) {
        avformat_close_input(&fmt);
        throw std::runtime_error("no stream info in " + file);
    }
    struct Seen {
        std::uint64_t packets = 0, keyframes = 0, bytes = 0;
        double first = -1, last = -1, lastDur = 0;
    };
    std::vector<Seen> seen(fmt->nb_streams);
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        Seen& s = seen[static_cast<size_t>(pkt->stream_index)];
        const AVRational tb = fmt->streams[pkt->stream_index]->time_base;
        ++s.packets;
        s.bytes += static_cast<std::uint64_t>(pkt->size);
        if (pkt->flags & AV_PKT_FLAG_KEY) ++s.keyframes;
        if (pkt->pts != AV_NOPTS_VALUE) {
            const double t = double(pkt->pts) * av_q2d(tb);
            if (s.first < 0 || t < s.first) s.first = t;
            if (t > s.last) {
                s.last = t;
                s.lastDur = double(pkt->duration) * av_q2d(tb);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    std::printf("%s  container %s  duration %.3f s\n", file.c_str(), fmt->iformat->name,
                fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0.0);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* cp = st->codecpar;
        const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
        if (!title) title = av_dict_get(st->metadata, "handler_name", nullptr, 0);
        const Seen& s = seen[i];
        std::string kind;
        char info[96] = {};
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            kind = "video";
            std::snprintf(info, sizeof info, "%dx%d", cp->width, cp->height);
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            kind = "audio";
            std::snprintf(info, sizeof info, "%d Hz %d ch", cp->sample_rate, cp->ch_layout.nb_channels);
        }
        std::printf("STREAM index=%u type=%s codec=%s %s title=\"%s\" packets=%llu keyframes=%llu "
                    "first=%.3f end=%.3f mbps=%.2f\n",
                    i, kind.c_str(), avcodec_get_name(cp->codec_id), info, title ? title->value : "",
                    static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.keyframes),
                    s.first, s.last + s.lastDur,
                    s.last > s.first ? double(s.bytes) * 8 / 1e6 / (s.last + s.lastDur - s.first) : 0.0);
    }
    avformat_close_input(&fmt);
    return 0;
}

int cmdServe(const std::vector<std::string>& args) {
    EngineFollower::Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error("missing value for " + a);
            return args[++i];
        };
        if (a == "--host") o.host = next();
        else if (a == "--port") o.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--no-video") o.withVideo = false;
        else if (a == "--shm-suffix") o.shmSuffix = next();
        else throw std::runtime_error("unknown option " + a);
    }
    EngineFollower follower(o);
    return follower.run(g_stop);
}

int cmdRecord(const std::vector<std::string>& args) {
    RecordSettings s;
    s.audio.push_back({0, "master", "aac", 192});
    std::string output, shmSuffix, audioCodec = "aac";
    int audioBitrate = 192;
    double seconds = 0;
    bool mixSizeSet = false, mixFpsSet = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error("missing value for " + a);
            return args[++i];
        };
        if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--output") output = next();
        else if (a == "--dir") s.directory = next();
        else if (a == "--container") s.container = next();
        else if (a == "--encoder") s.video.encoder = next();
        else if (a == "--codec") s.video.codec = next();
        else if (a == "--size") {
            if (!parseSize(next(), s.video.width, s.video.height)) throw std::runtime_error("bad --size");
        } else if (a == "--fps") s.video.fps = std::max(1, std::atoi(next().c_str()));
        else if (a == "--rc") s.video.rateControl = next();
        else if (a == "--bitrate") s.video.bitrateKbps = std::atoi(next().c_str());
        else if (a == "--quality") s.video.quality = std::atoi(next().c_str());
        else if (a == "--gop") s.video.gopFrames = std::atoi(next().c_str());
        else if (a == "--low-latency") s.video.lowLatency = true;
        else if (a == "--force-keyframes") s.forceKeyframeSeconds = std::atof(next().c_str());
        else if (a == "--mix-size") {
            if (!parseSize(next(), s.mix.width, s.mix.height)) throw std::runtime_error("bad --mix-size");
            mixSizeSet = true;
        } else if (a == "--mix-fps") {
            s.mix.fps = std::max(1, std::atoi(next().c_str()));
            mixFpsSet = true;
        } else if (a == "--source") s.mix.source = next();
        else if (a == "--monitor") s.mix.monitor = std::atoi(next().c_str());
        else if (a == "--audio") {
            s.audio.clear();
            const std::string list = next();
            if (list != "none") {
                for (const auto& src : split(list, ',')) {
                    const auto slot = slotForSource(src);
                    if (!slot) throw std::runtime_error("bad audio source " + src + " (master or track/<n>)");
                    s.audio.push_back({*slot, src, "aac", 192});
                }
            }
        } else if (a == "--audio-codec") audioCodec = next();
        else if (a == "--audio-bitrate") audioBitrate = std::atoi(next().c_str());
        else if (a == "--no-video") s.withVideo = false;
        else if (a == "--shm-suffix") shmSuffix = next();
        else throw std::runtime_error("unknown option " + a);
    }
    for (auto& a : s.audio) {
        a.codec = audioCodec;
        a.bitrateKbps = audioBitrate;
    }
    s.video.width &= ~1;
    s.video.height &= ~1;
    if (!mixSizeSet) {
        s.mix.width = s.video.width;
        s.mix.height = s.video.height;
    }
    if (!mixFpsSet) s.mix.fps = s.video.fps;
    if (s.mix.source != "wgc" && s.mix.source != "dda") throw std::runtime_error("--source must be wgc or dda");
#ifndef _WIN32
    s.withVideo = false;  // video capture is Windows only
#endif

    const std::string path = output.empty() ? recordingPath(s, std::time(nullptr)) : output;

#ifdef _WIN32
    // The video mixer with the start tree (mix 1 shows one monitor), adjusted by the options.
    std::unique_ptr<VideoEngine> video;
    std::shared_ptr<VideoMix> mix;
    if (s.withVideo) {
        dc::proto::ParamTree tree;
        for (const auto& c : dc::proto::globalDefaults()) tree.set(c.path, c.value);
        tree.set("/video/source/1/display/method", s.mix.source);
        if (s.mix.monitor >= 0) {
            const auto displays = listDisplays();
            if (s.mix.monitor >= static_cast<int>(displays.size())) throw std::runtime_error("no such monitor number");
            tree.set("/video/source/1/display/id", displays[static_cast<size_t>(s.mix.monitor)].id);
        }
        tree.set("/video/mix/1/width", std::int64_t{s.mix.width});
        tree.set("/video/mix/1/height", std::int64_t{s.mix.height});
        tree.set("/video/mix/1/fps", std::int64_t{s.mix.fps});
        video = std::make_unique<VideoEngine>();
        video->update(tree);
        mix = video->acquire(1);
    }
    Recorder rec(s, mix.get(), path, shmSuffix);
#else
    Recorder rec(s, path, shmSuffix);
#endif

    rec.start();
    for (const auto& line : rec.log()) std::cout << line << "\n";
    std::cout << "recording " << path << (seconds > 0 ? "" : " (Ctrl+C to stop)") << "\n" << std::flush;

    const auto t0 = std::chrono::steady_clock::now();
    auto nextPrint = t0 + std::chrono::seconds(1);
    std::string error;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        if (seconds > 0 && now - t0 >= std::chrono::duration<double>(seconds)) break;
        const auto st = rec.status();
        if (!st.error.empty()) {
            error = st.error;
            break;
        }
        if (now >= nextPrint) {
            nextPrint += std::chrono::seconds(1);
            char line[300];
            std::snprintf(line, sizeof line,
                          "%6.1fs  video %llu frames, dropped %llu  |  audio %s, silence %.2fs, drift %+.1f ms  |  %.1f MB",
                          double(st.elapsedMs) / 1000.0, static_cast<unsigned long long>(st.videoFrames),
                          static_cast<unsigned long long>(st.videoDropped),
                          s.audio.empty() ? "off" : (st.engineConnected ? "engine ok" : "NO ENGINE"),
                          double(st.audioSilenceFrames) / 48000.0, st.audioDriftMs, double(st.bytes) / 1e6);
            std::string extra;
#ifdef _WIN32
            if (mix) {
                const auto ms = mix->stats();
                char m[160];
                std::snprintf(m, sizeof m, "  |  mix captured %llu, late %llu, tick %.2f/%.2f ms",
                              static_cast<unsigned long long>(ms.captured),
                              static_cast<unsigned long long>(ms.lateTicks), ms.tickMsAvg, ms.tickMsMax);
                extra = m;
            }
#endif
            std::cout << line << extra << "\n" << std::flush;
        }
    }

    const auto fin = rec.stop();
#ifdef _WIN32
    mix.reset();
    video.reset();
#endif
    if (error.empty()) error = fin.error;
    char res[600];
    std::snprintf(res, sizeof res,
                  "RESULT file=\"%s\" seconds=%.2f video_frames=%llu video_dropped=%llu audio_frames=%llu "
                  "audio_silence_frames=%llu bytes=%llu error=\"%s\"",
                  fin.file.c_str(), double(fin.elapsedMs) / 1000.0, static_cast<unsigned long long>(fin.videoFrames),
                  static_cast<unsigned long long>(fin.videoDropped), static_cast<unsigned long long>(fin.audioFrames),
                  static_cast<unsigned long long>(fin.audioSilenceFrames), static_cast<unsigned long long>(fin.bytes),
                  error.c_str());
    std::cout << res << "\n";
    return error.empty() ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "--version") {
        std::cout << "capd " << dc::kVersionString << " (protocol " << dc::proto::kProtocolVersion << ", shm audio v"
                  << dc::shm::kVersion << ")\n";
        return 0;
    }
    if (!args.empty() && (args[0] == "-h" || args[0] == "--help" || args[0] == "help")) {
        usage();
        return 0;
    }
    if (args.empty() || args[0].rfind("--", 0) == 0) args.insert(args.begin(), "serve");

    av_log_set_level(AV_LOG_WARNING);  // FFmpeg's per-encoder info lines are noise in the status output
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    try {
        const std::string cmd = args[0];
        args.erase(args.begin());
#ifdef _WIN32
        if (cmd == "list") return cmdList();
#endif
        if (cmd == "record") return cmdRecord(args);
#ifdef _WIN32
        if (cmd == "mixtest") return cmdMixTest(args);
#endif
        if (cmd == "serve") return cmdServe(args);
        if (cmd == "probe" && args.size() == 1) return cmdProbe(args[0]);
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
