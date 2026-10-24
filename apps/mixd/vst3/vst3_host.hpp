#pragma once

// VST3 hosting for mixd through JUCE 9 (built only with DC_WITH_JUCE). This header has no JUCE types,
// so main.cpp and the engine core compile without JUCE's headers.
//
// Threads: JUCE needs a message thread for creating plugins and showing their windows. mixd runs
// the JUCE message loop on its main thread (runWithMessageLoop) and everything else on other threads.

#include "fx/plugin_host.hpp"

#include <functional>
#include <memory>
#include <string>

namespace dc::mixd::vst3 {

struct HostOptions {
    std::string cacheFile;  // scan results; "" = %APPDATA%\DesktopCapture\vst3-cache.json
    std::string scannerExe; // program run as "<exe> --scan-plugin <file> --out <json>"; "" = this executable
    int scanTimeoutSeconds = 60;
    bool verbose = true;
};

std::unique_ptr<PluginHost> makeHost(const HostOptions& options);

// Initialises JUCE, runs body on a new thread and the JUCE message loop on the calling thread until
// body returns. Returns body's result.
int runWithMessageLoop(const std::function<int()>& body);

// The scanner child process: loads every plugin type in file, checks it can run on a mono or stereo
// strip, and writes the result as JSON to out. Exit code 0 when the file was read.
int scanChildMain(const std::string& file, const std::string& out);

} // namespace dc::mixd::vst3
