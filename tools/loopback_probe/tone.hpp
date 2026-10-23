// Test tone source: plays a sine through the default render device in shared mode.
// The self-test runs it in child processes so each tone belongs to a known PID.
#pragma once

#include <cstdint>
#include <string>

namespace v1 {

struct ToneOptions {
    double freq = 1000.0;
    double amplitude = 0.1;   // -20 dBFS
    double seconds = 2.0;
    int delayMs = 0;          // silence before the tone starts (the stream is already running)
    std::string reportPath;   // if set, the QPC (100 ns) of the first submitted tone sample is written here
};

// Returns 0 on success; prints the failing step to stderr otherwise.
int playTone(const ToneOptions& opt);

}  // namespace v1
