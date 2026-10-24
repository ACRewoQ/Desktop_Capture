#pragma once

// Turns the parameter tree (the engine's settings) and the currently open ports into a MixPlan, plus
// what the rest of the engine derives from the same settings: which devices to open, per-track input
// state, shared memory slots. Pure functions of their arguments.

#include "plan.hpp"

#include "dc/protocol/param_tree.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

struct PortTable {
    std::map<std::string, std::shared_ptr<InputPort>> inputs;   // open input streams by device id
    std::map<std::string, std::shared_ptr<OutputPort>> outputs; // open output streams by device id
};

// Devices the settings refer to, for the device manager.
struct DeviceDemand {
    std::vector<std::string> inputs;  // sorted, unique
    std::vector<std::string> outputs; // clock preference order: clock_device, master outputs, the rest
    std::string clockDevice;          // /system/clock_device ("" = automatic)
    std::string mode = "shared";      // /system/audio_mode
    int blockFrames = 256;            // /system/block_size
    bool operator==(const DeviceDemand&) const = default;
};

DeviceDemand deviceDemand(const proto::ParamTree& tree);

struct BuildResult {
    std::unique_ptr<MixPlan> plan;
    std::map<int, std::string> inputState; // track -> none / ok / disconnected
    bool soloActive = false;
    std::array<std::uint32_t, kMaxStrips> shmChannels{}; // per slot: 0 = unused, 1 or 2
    std::array<bool, kMaxStrips> shmDisconnected{};
};

struct BuildOptions {
    float testToneGain = 0;
    double testToneHz = 1000;
};

// Engine-side objects a plan carries besides the settings: the recording in progress (its streams
// decide which strips are recorded, frozen for the take) and the sampler pads.
struct PlanResources {
    std::shared_ptr<RecorderTake> take;
    std::shared_ptr<const SamplerTable> sampler;
};

BuildResult buildPlan(const proto::ParamTree& tree, const PortTable& ports, std::uint64_t generation,
                      const BuildOptions& options = {}, const PlanResources& resources = {});

// Track numbers in screen order (by /track/N/position).
std::vector<int> tracksByPosition(const proto::ParamTree& tree);

} // namespace dc::mixd
