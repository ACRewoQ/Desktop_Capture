// Command line options shared by dc-v6-bench and dc-v6-preview.
#pragma once

#include "pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace v6 {

struct CliOptions {
    PipelineOptions pipe;
    double seconds = 0;      // 0 = run until closed (preview) / 20 s (bench)
    int warmup = 120;        // frames excluded from the report
    std::string sendTo;      // host:port -> network sender
    int listenPort = 0;      // network receiver
    std::string csv;
    std::string label;
    bool fullscreen = false;
    bool help = false;
};

inline const char* kCommonUsage =
    "  --size WxH          frame size (default 1920x1080)\n"
    "  --fps N             frame rate (default 60)\n"
    "  --kbps N            encoder bitrate (default 8000)\n"
    "  --encoder auto|hw|sw  H.264 encoder MFT (default auto: hardware, else software)\n"
    "  --decoder hw|sw     hw = D3D11/DXVA decoding (default), sw = software decoding\n"
    "  --seconds N         stop after N seconds and print the report\n"
    "  --warmup N          frames excluded from the report (default 120)\n"
    "  --csv FILE          per-frame stage durations\n"
    "  --label TEXT        title for the report\n";

// Returns false on a bad option (message printed).
inline bool parseCli(int argc, char** argv, CliOptions& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                return nullptr;
            }
            return argv[++i];
        };
        const char* v = nullptr;
        if (a == "--help" || a == "-h") {
            o.help = true;
        } else if (a == "--size") {
            if (!(v = next()) || std::sscanf(v, "%dx%d", &o.pipe.width, &o.pipe.height) != 2) return false;
            o.pipe.width &= ~1;
            o.pipe.height &= ~1;
        } else if (a == "--fps") {
            if (!(v = next())) return false;
            o.pipe.fps = std::atoi(v);
        } else if (a == "--kbps") {
            if (!(v = next())) return false;
            o.pipe.kbps = std::atoi(v);
        } else if (a == "--encoder") {
            if (!(v = next())) return false;
            const std::string s = v;
            o.pipe.encoder = s == "hw" ? CodecKind::Hardware : s == "sw" ? CodecKind::Software : CodecKind::Auto;
        } else if (a == "--decoder") {
            if (!(v = next())) return false;
            o.pipe.hwDecode = std::strcmp(v, "sw") != 0;
        } else if (a == "--seconds") {
            if (!(v = next())) return false;
            o.seconds = std::atof(v);
        } else if (a == "--warmup") {
            if (!(v = next())) return false;
            o.warmup = std::atoi(v);
        } else if (a == "--send") {
            if (!(v = next())) return false;
            o.sendTo = v;
        } else if (a == "--listen") {
            if (!(v = next())) return false;
            o.listenPort = std::atoi(v);
        } else if (a == "--csv") {
            if (!(v = next())) return false;
            o.csv = v;
        } else if (a == "--label") {
            if (!(v = next())) return false;
            o.label = v;
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    if (o.pipe.fps <= 0 || o.pipe.width < 64 || o.pipe.height < 64 || o.pipe.kbps <= 0) {
        std::fprintf(stderr, "bad size / fps / kbps\n");
        return false;
    }
    return true;
}

inline bool splitHostPort(const std::string& s, std::string& host, int& port) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    port = std::atoi(s.c_str() + colon + 1);
    return !host.empty() && port > 0;
}

}  // namespace v6
