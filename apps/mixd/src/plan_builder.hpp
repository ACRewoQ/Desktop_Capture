#pragma once

// Turns the parameter tree (the engine's settings) and the currently open ports into a MixPlan, plus
// what the rest of the engine derives from the same settings: which devices to open, per-track input
// state, shared memory slots. Pure functions of their arguments.

#include "device.hpp"
#include "plan.hpp"

#include "dc/protocol/param_tree.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

struct PortTable {
    // open input streams by device id, process captures by processPortKey()
    std::map<std::string, std::shared_ptr<InputPort>> inputs;
    std::map<std::string, std::shared_ptr<OutputPort>> outputs; // open output streams by device id
};

// Devices the settings refer to, for the device manager.
struct DeviceDemand {
    std::vector<std::string> inputs;  // sorted, unique
    std::vector<std::string> processes; // executable names of process inputs, lower case, sorted, unique
    std::vector<std::string> outputs; // clock preference order: clock_device, master outputs, the rest
    std::string clockDevice;          // /system/clock_device ("" = automatic)
    std::string mode = "shared";      // /system/audio_mode
    int blockFrames = 256;            // /system/block_size
    bool operator==(const DeviceDemand&) const = default;
};

DeviceDemand deviceDemand(const proto::ParamTree& tree);

// "chrome.exe" / "Chrome.EXE" -> "chrome.exe": executable names compare case-insensitively.
std::string lowerExe(const std::string& exe);
// PortTable::inputs key of the capture of an application: "process:<lower case exe>".
inline std::string processPortKey(const std::string& exe) { return "process:" + lowerExe(exe); }

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
    double testPulseSeconds = 0; // > 0: the tone only sounds 50 ms at each multiple of this QPC time
};

// Engine-side objects a plan carries besides the settings: the recording in progress (its streams
// decide which strips are recorded, frozen for the take) and the sampler pads.
struct PlanResources {
    std::shared_ptr<RecorderTake> take;
    std::shared_ptr<const SamplerTable> sampler;
};

BuildResult buildPlan(const proto::ParamTree& tree, const PortTable& ports, std::uint64_t generation,
                      const BuildOptions& options = {}, const PlanResources& resources = {});

// Devices whose ID changed (Windows re-registers a device after a sample rate change or on another
// USB port): settings that point at an ID that is gone are moved to the one connected device of the
// same direction and name that nothing else uses yet (protocol.md 4.4).
struct KnownDevice {
    std::string name;
    bool input = false;
};
struct Rematch {
    std::string oldId, newId, name;
};
struct RematchResult {
    std::vector<proto::Change> changes; // the settings rewritten to the new IDs
    std::vector<Rematch> moved;
};
RematchResult rematchDevices(const proto::ParamTree& tree, const std::vector<DeviceInfo>& devices,
                             const std::map<std::string, KnownDevice>& known);
// The name without the number Windows puts in front when it registers a device again:
// "Line (2- Steinberg UR22)" -> "Line (Steinberg UR22)", "2- USB Speaker" -> "USB Speaker".
std::string deviceBaseName(const std::string& name);

// Track numbers in screen order (by /track/N/position).
std::vector<int> tracksByPosition(const proto::ParamTree& tree);

} // namespace dc::mixd
