// dc-audio-check: reads audio files the way the sampler does (WAV / FLAC / MP3 / Ogg Vorbis, to 48 kHz
// stereo) and prints one line per file, for the lane A3 checks on a real PC.
//
//   dc-audio-check [--tone HZ] FILE...
//
// Output: file=<path> frames=<n> seconds=<s> peak=<linear> [glitches=<blocks> min_sinad_db=<dB>]
// With --tone, channel 0 must carry one continuous sine of HZ (the test tone of mixd --test-tone):
// 100 ms blocks are sine-fitted and a block far below the normal SINAD counts as a glitch (a dropped
// or repeated piece of audio), listed on the following lines. The first and last block are skipped
// (fade-in, partial block).

#include "audio_file.hpp"

#include "dc/clocksync/tone_analyzer.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    double tone = 0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--tone" && i + 1 < argc) tone = std::stod(argv[++i]);
        else files.push_back(a);
    }
    if (files.empty()) {
        std::cerr << "usage: dc-audio-check [--tone HZ] FILE...\n";
        return 2;
    }
    int rc = 0;
    for (const auto& f : files) {
        dc::mixd::DecodedAudio audio;
        std::string err;
        if (!dc::mixd::decodeAudioFile(dc::mixd::pathFromUtf8(f), audio, &err, 24 * 3600.0)) {
            std::cout << "file=" << f << " error=\"" << err << "\"" << std::endl;
            rc = 1;
            continue;
        }
        float peak = 0;
        for (float v : audio.samples) peak = std::max(peak, std::fabs(v));
        char line[256];
        std::snprintf(line, sizeof line, " frames=%zu seconds=%.3f peak=%.4f", audio.frames,
                      static_cast<double>(audio.frames) / 48000.0, static_cast<double>(peak));
        std::cout << "file=" << f << line;
        if (tone > 0) {
            dc::clocksync::ToneAnalyzer::Config cfg;
            cfg.toneHz = tone;
            dc::clocksync::ToneAnalyzer an(cfg);
            const std::size_t block = cfg.blockFrames;
            if (audio.frames > 3 * block) {
                an.feed(audio.samples.data(), block, 2); // warm-up block
                an.resetSummary();
                const std::size_t usable = (audio.frames / block - 2) * block;
                an.feed(audio.samples.data() + 2 * block, usable, 2);
            }
            const auto& s = an.summary();
            std::snprintf(line, sizeof line, " glitches=%llu min_sinad_db=%.1f", static_cast<unsigned long long>(s.glitchBlocks),
                          s.blocks ? s.minSinadDb : 0.0);
            std::cout << line;
            for (const auto& e : s.events)
                std::cout << "\n  glitch at " << static_cast<double>(e.startFrame + block) / 48000.0 << " s, SINAD "
                          << e.sinadDb << " dB";
        }
        std::cout << std::endl;
    }
    return rc;
}
