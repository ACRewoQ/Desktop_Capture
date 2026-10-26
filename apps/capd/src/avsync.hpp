#pragma once

// A/V sync check of a recording (G2): dc-av-flash turns a corner of the screen white and mixd
// --test-pulse sounds a 50 ms burst at the same multiples of QPC time. Both land in the recording;
// the offset of each burst from its flash is how far the audio sits from the picture.
//
// Detection is in plain functions (tested without files); analyzeAvSync() decodes a file with FFmpeg.

#include <cstddef>
#include <string>
#include <vector>

namespace dc::capd {

struct AvSyncOptions {
    // Part of the picture that flashes, as fractions of the frame (dc-av-flash covers the top-left
    // quarter of the monitor; this stays inside it even with a small letterbox).
    double regionX = 0.06, regionY = 0.06, regionW = 0.10, regionH = 0.10;
    int audioStream = 0;    // n-th audio stream of the file (0 = first, usually the master)
    double maxPairSec = 0.5; // a flash and a burst further apart than this are not a pair
};

struct AvSyncPair {
    double videoSec = 0;
    double audioSec = 0;
    double offsetMs() const { return (audioSec - videoSec) * 1000.0; } // > 0: audio later than picture
};

struct AvSyncStats {
    int pairs = 0;
    double meanMs = 0, medianMs = 0, minMs = 0, maxMs = 0;
    // Median of the last tenth of the pairs minus the median of the first tenth (change over the file).
    double driftMs = 0;
};

struct AvSyncReport {
    std::string error; // empty when the file could be read
    double durationSec = 0;
    int videoFrames = 0;
    std::vector<double> flashes; // seconds (presentation time)
    std::vector<double> pulses;
    std::vector<AvSyncPair> pairs;
    AvSyncStats stats;
};

// Rising edges of the region's brightness: the time of the first frame above the midpoint between the
// darkest and brightest frames. Needs a contrast of at least minContrast (0..1 of full scale).
std::vector<double> findFlashes(const std::vector<double>& times, const std::vector<float>& brightness,
                                double minContrast = 0.2);

// Burst onsets in one audio channel: the first sample above 30% of the loudest sample after at least
// quietSec of quiet.
std::vector<double> findPulses(const std::vector<float>& samples, double startSec, int sampleRate,
                               double quietSec = 0.5);

// Nearest burst to each flash, within maxPairSec; each burst is used once.
std::vector<AvSyncPair> pairEvents(const std::vector<double>& flashes, const std::vector<double>& pulses,
                                   double maxPairSec);

AvSyncStats computeStats(const std::vector<AvSyncPair>& pairs);

AvSyncReport analyzeAvSync(const std::string& file, const AvSyncOptions& options = {});

// Audio streams of a recording, one entry per stream (G3: separate tracks really hold different audio).
// The file is cut into 1 s blocks of channel 0; a block counts as "tone" when the given frequency is at
// most 20 dB below the block's level and the block is not silent, and as "silent" below -80 dBFS.
struct AudioStreamLevels {
    int index = 0;        // n-th audio stream (0 = first)
    std::string title;    // stream title (the engine's track name)
    double seconds = 0;   // decoded audio
    double rmsDb = -200;  // whole stream
    double toneDb = -200; // level of the tone frequency over the whole stream (sine amplitude, dBFS)
    int blocks = 0, toneBlocks = 0, silentBlocks = 0;
};

// Level of a sine at hz in samples (Goertzel over the whole range), as dBFS of its amplitude.
double toneLevelDb(const float* samples, std::size_t count, int sampleRate, double hz);
double rmsDb(const float* samples, std::size_t count);

std::vector<AudioStreamLevels> analyzeAudioStreams(const std::string& file, double toneHz, std::string* error);

} // namespace dc::capd
