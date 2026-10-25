#include "mock_plugins.hpp"

#include "dc/protocol/fx.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace dc::mock {

using mixd::PluginInfo;
using mixd::PluginInstance;
using mixd::PluginParamInfo;

namespace {

struct MockParam {
    std::uint32_t id;
    const char* name;
    const char* unit;
    double lo, hi; // the value range shown in the text
    double def;    // normalized
    int steps;
};

// A channel strip with IDs that are not contiguous, like real plugins.
const MockParam kStrip[] = {
    {1001, "Input", "dB", -24, 24, 0.5, 0},       {1002, "Drive", "%", 0, 100, 0.0, 0},
    {1003, "Low", "dB", -15, 15, 0.5, 0},         {1004, "Low Freq", "Hz", 30, 450, 0.3, 0},
    {1005, "Mid", "dB", -15, 15, 0.5, 0},         {1006, "Mid Freq", "Hz", 200, 7000, 0.4, 0},
    {1007, "High", "dB", -15, 15, 0.5, 0},        {1008, "High Freq", "Hz", 1500, 16000, 0.5, 0},
    {2001, "Threshold", "dB", -40, 0, 0.6, 0},    {2002, "Ratio", "", 1, 10, 0.3, 0},
    {2003, "Attack", "ms", 0.1, 100, 0.2, 0},     {2004, "Release", "ms", 50, 1200, 0.3, 0},
    {2005, "Mode", "", 0, 2, 0.0, 3},             {3001, "Output", "dB", -24, 24, 0.5, 0},
    {3002, "Mix", "%", 0, 100, 1.0, 0},           {3003, "Oversampling", "", 0, 1, 0.0, 2},
};

class MockPlugin final : public PluginInstance {
public:
    MockPlugin(std::string name, int extraParams) : name_(std::move(name)), extra_(extraParams) {
        for (std::size_t i = 0; i < std::size(kStrip); ++i) values_[i] = kStrip[i].def;
    }
    std::string name() const override { return name_; }
    void setListener(Listener l) override {
        std::lock_guard lock(mutex_);
        listener_ = std::move(l);
    }
    int paramCount() const override { return static_cast<int>(std::size(kStrip)) + extra_; }
    std::vector<PluginParamInfo> params(int offset, int count) const override {
        std::vector<PluginParamInfo> out;
        for (int i = offset; i < offset + count && i < paramCount(); ++i) out.push_back(info(i));
        return out;
    }
    std::optional<PluginParamInfo> param(const std::string& key) const override {
        const auto id = proto::vst3ParamId(key);
        if (!id) return std::nullopt;
        const int i = indexOf(*id);
        if (i < 0) return std::nullopt;
        return info(i);
    }
    bool hasEditor() const override { return true; }
    void setEditorOpen(bool) override {} // nothing to show in the mock
    void setParam(std::string_view key, double value) override {
        if (const auto id = proto::vst3ParamId(key)) {
            const int i = indexOf(*id);
            if (i >= 0 && i < static_cast<int>(values_.size())) values_[static_cast<std::size_t>(i)] = value;
        }
    }
    void process(float* const*, int, int) override {}
    void reset() override {}

private:
    // params after the strip are "Macro N" with IDs 5000 + N
    int indexOf(std::uint32_t id) const {
        for (std::size_t i = 0; i < std::size(kStrip); ++i)
            if (kStrip[i].id == id) return static_cast<int>(i);
        if (id >= 5000 && id < 5000u + static_cast<std::uint32_t>(extra_)) return static_cast<int>(std::size(kStrip) + (id - 5000));
        return -1;
    }
    PluginParamInfo info(int i) const {
        PluginParamInfo p;
        char text[48];
        if (i < static_cast<int>(std::size(kStrip))) {
            const auto& m = kStrip[i];
            const double v = values_[static_cast<std::size_t>(i)].load();
            p.key = proto::vst3ParamKey(m.id);
            p.name = m.name;
            p.unit = m.unit;
            p.value = v;
            p.defaultValue = m.def;
            p.steps = m.steps;
            if (m.steps > 1) std::snprintf(text, sizeof text, "%d", static_cast<int>(v * (m.steps - 1) + 0.5));
            else std::snprintf(text, sizeof text, "%.1f %s", m.lo + (m.hi - m.lo) * v, m.unit);
        } else {
            const int n = i - static_cast<int>(std::size(kStrip));
            p.key = proto::vst3ParamKey(5000u + static_cast<std::uint32_t>(n));
            p.name = "Macro " + std::to_string(n + 1);
            std::snprintf(text, sizeof text, "%.0f%%", 0.0);
        }
        p.text = text;
        return p;
    }

    std::string name_;
    int extra_;
    std::array<std::atomic<double>, std::size(kStrip)> values_;
    std::mutex mutex_;
    Listener listener_;
};

} // namespace

std::vector<PluginInfo> MockPluginHost::plugins() const {
    return {
        {"vst3:6d6f6301", "Mock Channel Strip", "Mock Audio", "Fx|EQ|Dynamics", "C:/Program Files/Common Files/VST3/MockStrip.vst3",
         false, "ok", ""},
        {"vst3:6d6f6302", "Mock Big Synth", "Mock Audio", "Instrument|Synth", "C:/Program Files/Common Files/VST3/MockSynth.vst3",
         true, "ok", ""},
        {"vst3:6d6f6303", "Mock Broken", "Mock Audio", "Fx", "C:/Program Files/Common Files/VST3/MockBroken.vst3", false,
         "crashed", "child died: access violation during [create instance]"},
    };
}

bool MockPluginHost::scan(bool, const std::vector<std::string>&, std::function<void(ScanProgress)> progress,
                          std::function<void()> done) {
    const auto list = plugins();
    for (std::size_t i = 0; i < list.size(); ++i) progress({static_cast<double>(i) / static_cast<double>(list.size()), list[i].path});
    done();
    return true;
}

void MockPluginHost::create(const std::string& id, std::function<void(std::shared_ptr<PluginInstance>, std::string)> done) {
    if (id == "vst3:6d6f6301") return done(std::make_shared<MockPlugin>("Mock Channel Strip", 0), "");
    if (id == "vst3:6d6f6302") return done(std::make_shared<MockPlugin>("Mock Big Synth", 2000), ""); // many parameters
    done(nullptr, "the plugin could not be loaded");
}

} // namespace dc::mock
