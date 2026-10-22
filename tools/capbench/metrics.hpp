// Load measurement: this process's CPU time, whole-system CPU, and per-engine GPU utilisation (the same "GPU Engine"
// performance counters Task Manager shows).
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dc::capbench {

struct LoadSample {
    double processCpuPct = 0;  // of the whole machine (all logical cores = 100 %)
    double processCpuCorePct = 0;  // of one core
    double systemCpuPct = 0;
    // GPU engine utilisation caused by this process, per engine type (%), -1 if the counters are unavailable.
    double gpu3dPct = -1;
    double gpuEncodePct = -1;
    double gpuVideoProcessingPct = -1;
    double gpuCopyPct = -1;
    double gpuEncodeAllPct = -1;  // VideoEncode engine, all processes (NVENC sessions of other apps show here)
    std::uint64_t workingSetBytes = 0;
};

class LoadMeter {
public:
    LoadMeter();
    ~LoadMeter();

    // Resets the interval start.
    void begin();
    // Load since the previous begin()/sample() call; starts the next interval.
    LoadSample sample();

    int logicalCores() const { return cores_; }

private:
    struct Gpu;
    int cores_ = 1;
    std::int64_t lastWallQpc_ = 0;
    std::int64_t qpcFreq_ = 1;
    std::uint64_t lastProcTime_ = 0;
    std::uint64_t lastSysIdle_ = 0, lastSysTotal_ = 0;
    std::unique_ptr<Gpu> gpu_;
};

}  // namespace dc::capbench
