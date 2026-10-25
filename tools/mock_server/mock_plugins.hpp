#pragma once

// Pretend VST3 plugins for the mock server, so UIs can build the plugin list, the parameter list and
// the loading states without real plugins. No audio: the plugins pass their input through.

#include "fx/plugin_host.hpp"

namespace dc::mock {

class MockPluginHost final : public mixd::PluginHost {
public:
    std::vector<mixd::PluginInfo> plugins() const override;
    bool scan(bool rescan, const std::vector<std::string>& extraDirs, std::function<void(ScanProgress)> progress,
              std::function<void()> done) override;
    void create(const std::string& id,
                std::function<void(std::shared_ptr<mixd::PluginInstance>, std::string error)> done) override;
};

} // namespace dc::mock
