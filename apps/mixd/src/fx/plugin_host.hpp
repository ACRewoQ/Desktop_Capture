#pragma once

// Interface between the engine and a plugin host (VST3 through JUCE in mixd's Windows build, a fake in
// the tests). The engine core does not depend on JUCE: it only sees these classes.
//
// Threads: create(), scan() and every PluginInstance method except process()/reset() may be called
// from the engine's network thread; callbacks arrive on the host's own thread and must be posted on.

#include "effect.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dc::mixd {

struct PluginParamInfo {
    std::string key;  // "p<ParamID>"
    std::string name;
    std::string unit; // the plugin's label
    std::string text; // the plugin's text for the current value
    double value = 0; // normalized
    double defaultValue = 0;
    int steps = 0;    // 0 = continuous
};

struct PluginInfo {
    std::string id; // "vst3:<uid>", the effect type
    std::string name;
    std::string vendor;
    std::string category;
    std::string path;
    bool instrument = false;
    std::string status = "ok"; // ok / failed / crashed / timeout / unsupported
    std::string error;
    bool operator==(const PluginInfo&) const = default;
};

class PluginInstance : public Effect {
public:
    struct Listener {
        // a parameter changed inside the plugin (its window, a preset); normalized value
        std::function<void(const std::string& key, double value)> onParam;
        // the user closed the plugin window
        std::function<void()> onEditorClosed;
    };

    virtual std::string name() const = 0;
    virtual void setListener(Listener listener) = 0;

    // Visible parameters (no placeholders, no bypass) in the plugin's order.
    virtual int paramCount() const = 0;
    virtual std::vector<PluginParamInfo> params(int offset, int count) const = 0;
    virtual std::optional<PluginParamInfo> param(const std::string& key) const = 0;

    virtual bool hasEditor() const = 0;
    virtual void setEditorOpen(bool open) = 0;
};

class PluginHost {
public:
    virtual ~PluginHost() = default;

    // The plugins known from the last scan (loaded from the cache at start).
    virtual std::vector<PluginInfo> plugins() const = 0;

    struct ScanProgress {
        double fraction = 0;
        std::string current;
    };
    // Scans in the background (one child process per plugin file). progress and done run on a host
    // thread. Returns false when a scan is already running.
    virtual bool scan(bool rescan, const std::vector<std::string>& extraDirs, std::function<void(ScanProgress)> progress,
                      std::function<void()> done) = 0;

    // Loads plugin id asynchronously; done(instance, error) runs on a host thread. The instance is
    // prepared for 48 kHz, kFxMaxFrames, stereo (mono strips are fed both channels).
    virtual void create(const std::string& id,
                        std::function<void(std::shared_ptr<PluginInstance>, std::string error)> done) = 0;
};

} // namespace dc::mixd
