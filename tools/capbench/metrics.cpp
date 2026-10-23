#include "metrics.hpp"

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <string>
#include <vector>

namespace dc::capbench {

namespace {

std::uint64_t toU64(const FILETIME& ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::uint64_t processTime100ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return toU64(k) + toU64(u);
}

std::int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

}  // namespace

// "\GPU Engine(*)\Utilization Percentage" instances are named like
// "pid_1234_luid_0x00000000_0x0000D1B2_phys_0_eng_3_engtype_VideoEncode".
struct LoadMeter::Gpu {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::wstring pidTag;
    bool ok = false;

    Gpu() {
        pidTag = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS)
            return;
        ok = PdhCollectQueryData(query) == ERROR_SUCCESS;
    }
    ~Gpu() {
        if (query) PdhCloseQuery(query);
    }

    void sample(LoadSample& s) {
        if (!ok || PdhCollectQueryData(query) != ERROR_SUCCESS) return;
        DWORD bytes = 0, count = 0;
        const PDH_STATUS sizing = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (sizing != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return;
        std::vector<unsigned char> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;

        s.gpu3dPct = s.gpuEncodePct = s.gpuVideoProcessingPct = s.gpuCopyPct = s.gpuEncodeAllPct = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            const std::wstring name = items[i].szName;
            const double v = items[i].FmtValue.doubleValue;
            const bool mine = name.rfind(pidTag, 0) == 0;
            auto is = [&](const wchar_t* type) { return name.find(type) != std::wstring::npos; };
            if (is(L"engtype_VideoEncode")) {
                s.gpuEncodeAllPct += v;
                if (mine) s.gpuEncodePct += v;
            } else if (!mine) {
                continue;
            } else if (is(L"engtype_3D")) {
                s.gpu3dPct += v;
            } else if (is(L"engtype_VideoProcessing")) {
                s.gpuVideoProcessingPct += v;
            } else if (is(L"engtype_Copy")) {
                s.gpuCopyPct += v;
            }
        }
    }
};

LoadMeter::LoadMeter() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores_ = static_cast<int>(si.dwNumberOfProcessors);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;
    gpu_ = std::make_unique<Gpu>();
    begin();
}

LoadMeter::~LoadMeter() = default;

void LoadMeter::begin() {
    lastWallQpc_ = qpcNow();
    lastProcTime_ = processTime100ns();
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    lastSysIdle_ = toU64(idle);
    lastSysTotal_ = toU64(kernel) + toU64(user);  // kernel time includes idle time
}

LoadSample LoadMeter::sample() {
    LoadSample s;
    const std::int64_t now = qpcNow();
    const double wall100ns = double(now - lastWallQpc_) * 1e7 / double(qpcFreq_);
    const std::uint64_t proc = processTime100ns();
    if (wall100ns > 0) {
        s.processCpuCorePct = 100.0 * double(proc - lastProcTime_) / wall100ns;
        s.processCpuPct = s.processCpuCorePct / cores_;
    }

    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    const std::uint64_t sysIdle = toU64(idle), sysTotal = toU64(kernel) + toU64(user);
    if (sysTotal > lastSysTotal_)
        s.systemCpuPct = 100.0 * (1.0 - double(sysIdle - lastSysIdle_) / double(sysTotal - lastSysTotal_));

    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) s.workingSetBytes = pmc.WorkingSetSize;

    gpu_->sample(s);

    lastWallQpc_ = now;
    lastProcTime_ = proc;
    lastSysIdle_ = sysIdle;
    lastSysTotal_ = sysTotal;
    return s;
}

}  // namespace dc::capbench
