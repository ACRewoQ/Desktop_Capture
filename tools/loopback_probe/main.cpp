// dc-loopback-probe: P1 verification V1 (per-application audio capture with WASAPI process loopback).
//
//   dc-loopback-probe info                         OS build and whether process loopback can be activated
//   dc-loopback-probe list                         processes with an audio session on the default output
//   dc-loopback-probe capture (--pid N | --name x.exe) [--exclude] [--seconds S] [--wav out.wav]
//   dc-loopback-probe capture --device [--seconds S] [--wav out.wav]
//   dc-loopback-probe selftest [--extra N]         automated check with test tones in child processes
//   dc-loopback-probe multi --pids a,b,c [--exclude]  open several captures in one process, report each
//
// Internal (used by selftest): tone, tree.

#include "loopback_capture.hpp"
#include "tone.hpp"
#include "wasapi_util.hpp"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace v1;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = LoopbackCapture::kRate;

// ---------- arguments ----------

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;  // --key value, or --flag -> "1"

    bool has(const std::string& k) const { return opt.count(k) != 0; }
    std::string str(const std::string& k, const std::string& d = {}) const {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
    double num(const std::string& k, double d) const { return has(k) ? std::atof(str(k).c_str()) : d; }
};

Args parseArgs(int argc, char** argv) {
    static const std::set<std::string> flags = {"--exclude", "--device"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            if (flags.count(s) || i + 1 >= argc) a.opt[s] = "1";
            else a.opt[s] = argv[++i];
        } else
            a.pos.push_back(s);
    }
    return a;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring selfPath() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

double db(double v) { return v > 1e-9 ? 20.0 * std::log10(v) : -180.0; }

// ---------- recording and analysis ----------

struct Recording {
    std::vector<float> mono;
    std::vector<float> stereo;  // only kept when a WAV is requested
    bool keepStereo = false;
    struct Packet { size_t index; std::uint64_t qpc; };
    std::vector<Packet> packets;

    void push(const float* data, std::uint32_t frames, std::uint64_t qpc) {
        packets.push_back({mono.size(), qpc});
        for (std::uint32_t i = 0; i < frames; ++i) mono.push_back(0.5f * (data[2 * i] + data[2 * i + 1]));
        if (keepStereo) stereo.insert(stereo.end(), data, data + size_t(frames) * 2);
    }

    // QPC (100 ns) of a sample index, from the packet it belongs to.
    std::uint64_t qpcOf(size_t index) const {
        auto it = std::upper_bound(packets.begin(), packets.end(), index,
                                   [](size_t i, const Packet& p) { return i < p.index; });
        if (it == packets.begin()) return 0;
        --it;
        return it->qpc + std::uint64_t(index - it->index) * 10'000'000ull / kRate;
    }

    // Sine amplitude at `freq` over mono[start, start + n) (Goertzel).
    double goertzel(double freq, size_t start, size_t n) const {
        const double coeff = 2.0 * std::cos(2.0 * kPi * freq / kRate);
        double s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; ++i) {
            const double s0 = mono[start + i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt(std::max(power, 0.0)) / double(n);
    }

    // First sample of the first 10 ms block (1 ms hop) where the tone at `freq` exceeds `threshold`.
    // Tone specific, so other applications playing at the same time do not trigger it.
    size_t toneOnset(double freq, double threshold) const {
        constexpr size_t kBlock = kRate / 100, kHop = kRate / 1000;
        for (size_t start = 0; start + kBlock <= mono.size(); start += kHop)
            if (goertzel(freq, start, kBlock) > threshold) return start;
        return SIZE_MAX;
    }

    float peakBefore(std::uint64_t qpc) const {
        float p = 0;
        for (size_t i = 0; i < mono.size() && qpcOf(i) < qpc; ++i) p = std::max(p, std::fabs(mono[i]));
        return p;
    }

    size_t framesAfter(std::uint64_t qpc) const {
        size_t n = 0;
        for (const auto& pk : packets)
            if (pk.qpc >= qpc) { n = mono.size() - pk.index; break; }
        return n;
    }

    // Largest sine amplitude at `freq` over 2 s windows (0.5 Hz resolution; the test tones last 2.5 s and fall
    // on exact bins). The long window makes the check immune to other applications that happen to play
    // broadband sound (voice, music) at the same time, which a short window would count as the tone.
    double toneAmplitude(double freq) const {
        constexpr size_t kWindow = kRate * 2, kHop = kRate / 10;
        double best = 0;
        for (size_t start = 0; start + kWindow <= mono.size(); start += kHop)
            best = std::max(best, goertzel(freq, start, kWindow));
        return best;
    }
};

bool writeWav(const std::string& path, const std::vector<float>& stereo) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::uint32_t dataBytes = std::uint32_t(stereo.size() * sizeof(float));
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3 /* IEEE float */); u16(2); u32(kRate); u32(kRate * 8); u16(8); u16(32);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(stereo.data()), dataBytes);
    return bool(f);
}

// ---------- process helpers ----------

struct Child {
    PROCESS_INFORMATION pi{};
    DWORD pid() const { return pi.dwProcessId; }
    bool exited() const { return WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0; }
    ~Child() {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
};

bool spawn(const std::wstring& args, HANDLE job, Child& child) {
    std::wstring cmd = L"\"" + selfPath() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &child.pi))
        return false;
    if (job) AssignProcessToJobObject(job, child.pi.hProcess);
    ResumeThread(child.pi.hThread);
    return true;
}

DWORD findProcessRoot(const std::string& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, DWORD> parentOf;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    const std::wstring want = widen(exeName);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) parentOf[pe.th32ProcessID] = pe.th32ParentProcessID;
    CloseHandle(snap);
    // The root of a same-named tree (e.g. the main chrome.exe) is the one whose parent is not in the set.
    for (const auto& [pid, parent] : parentOf)
        if (!parentOf.count(parent)) return pid;
    return parentOf.empty() ? 0 : parentOf.begin()->first;
}

double cpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return double((std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
    return v(k) + v(u);
}

// ---------- commands ----------

int cmdInfo() {
    const OsVersion os = osVersion();
    std::cout << "os: Windows " << os.major << "." << os.minor << " build " << os.build << "." << os.ubr
              << (os.displayVersion.empty() ? "" : " (" + os.displayVersion + ")") << "\n";
    std::cout << "sdk note: process loopback is documented for build 20348 and later\n";

    LoopbackCapture cap;
    std::string where;
    const HRESULT hr = cap.open(LoopbackMode::IncludeProcessTree, GetCurrentProcessId(), &where);
    if (SUCCEEDED(hr))
        std::cout << "process loopback: available (activation " << cap.activationMs() << " ms"
                  << (cap.usedPcm16() ? ", PCM16 fallback" : ", float32") << ")\n";
    else
        std::cout << "process loopback: NOT available (" << where << ": " << hrText(hr) << ")\n";

    LoopbackCapture dev;
    const HRESULT hd = dev.open(LoopbackMode::DefaultDevice, 0, &where);
    std::cout << "device loopback:  " << (SUCCEEDED(hd) ? "available" : "NOT available (" + where + ": " + hrText(hd) + ")")
              << "\n";
    return SUCCEEDED(hr) ? 0 : 1;
}

int cmdList() {
    Com<IMMDeviceEnumerator> enumerator;
    Com<IMMDevice> device;
    Com<IAudioSessionManager2> manager;
    Com<IAudioSessionEnumerator> sessions;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  enumerator.putVoid());
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid());
    if (SUCCEEDED(hr)) hr = manager->GetSessionEnumerator(sessions.put());
    if (FAILED(hr)) {
        std::cerr << "session enumeration failed: " << hrText(hr) << "\n";
        return 1;
    }
    int count = 0;
    sessions->GetCount(&count);
    std::printf("%-8s %-8s %-8s %s\n", "pid", "root", "state", "process");
    for (int i = 0; i < count; ++i) {
        Com<IAudioSessionControl> ctl;
        Com<IAudioSessionControl2> ctl2;
        if (FAILED(sessions->GetSession(i, ctl.put()))) continue;
        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), ctl2.putVoid()))) continue;
        DWORD pid = 0;
        ctl2->GetProcessId(&pid);
        if (ctl2->IsSystemSoundsSession() == S_OK) continue;
        AudioSessionState state = AudioSessionStateInactive;
        ctl->GetState(&state);
        const std::string name = processName(pid);
        const DWORD root = findProcessRoot(name);
        const char* st = state == AudioSessionStateActive ? "active" : state == AudioSessionStateExpired ? "expired" : "idle";
        std::printf("%-8lu %-8lu %-8s %s\n", static_cast<unsigned long>(pid), static_cast<unsigned long>(root), st,
                    name.c_str());
    }
    std::cout << "(capture the 'root' pid with --pid to get the whole process tree, e.g. all browser tabs)\n";
    return 0;
}

int cmdCapture(const Args& a) {
    LoopbackMode mode = a.has("--device") ? LoopbackMode::DefaultDevice
                        : a.has("--exclude") ? LoopbackMode::ExcludeProcessTree
                                             : LoopbackMode::IncludeProcessTree;
    DWORD pid = 0;
    if (mode != LoopbackMode::DefaultDevice) {
        pid = a.has("--pid") ? DWORD(std::atol(a.str("--pid").c_str())) : findProcessRoot(a.str("--name"));
        if (pid == 0) {
            std::cerr << "give --pid N or --name process.exe (see 'list')\n";
            return 2;
        }
        std::cout << "target pid " << pid << " (" << processName(pid) << "), mode " << modeName(mode) << "\n";
    }
    const double seconds = a.num("--seconds", 10);

    Recording rec;
    rec.keepStereo = a.has("--wav");
    LoopbackCapture cap;
    std::string where;
    HRESULT hr = cap.open(mode, pid, &where);
    if (FAILED(hr)) {
        std::cerr << "open failed at " << where << ": " << hrText(hr) << "\n";
        return 1;
    }
    std::mutex m;
    hr = cap.start([&](const float* d, std::uint32_t n, std::uint64_t q) {
        std::lock_guard lock(m);
        rec.push(d, n, q);
    });
    if (FAILED(hr)) {
        std::cerr << "start failed: " << hrText(hr) << "\n";
        return 1;
    }
    std::cout << "capturing " << seconds << " s (activation " << cap.activationMs() << " ms)\n";
    size_t shown = 0;
    for (double t = 0; t < seconds; t += 0.5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::lock_guard lock(m);
        float peak = 0;
        for (size_t i = shown; i < rec.mono.size(); ++i) peak = std::max(peak, std::fabs(rec.mono[i]));
        std::printf("  %5.1f s  frames %8zu  peak %6.1f dBFS\n", t + 0.5, rec.mono.size() - shown, db(peak));
        shown = rec.mono.size();
    }
    cap.stop();
    const CaptureStats st = cap.stats();
    std::printf("frames %llu (%.1f%% of wall clock), packets %llu, silent %llu, discontinuities %llu, "
                "max gap %.1f ms, max packet %.1f ms, error %s\n",
                static_cast<unsigned long long>(st.frames), 100.0 * double(st.frames) / (seconds * kRate),
                static_cast<unsigned long long>(st.packets), static_cast<unsigned long long>(st.silentPackets),
                static_cast<unsigned long long>(st.discontinuities), st.maxGapMs, st.maxPacketMs,
                hrText(st.lastError).c_str());
    if (rec.keepStereo) {
        const std::string path = a.str("--wav");
        std::cout << (writeWav(path, rec.stereo) ? "wrote " : "could not write ") << path << "\n";
    }
    return 0;
}

// Opens several process loopback captures inside this one process and reports each result.
// e.g. "multi --pids 100,100,100" (same target) or "multi --pids 100,200,300" (different targets).
int cmdMulti(const Args& a) {
    std::vector<DWORD> pids;
    const std::string list = a.str("--pids");
    for (size_t pos = 0; pos < list.size();) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        pids.push_back(DWORD(std::atol(list.substr(pos, comma - pos).c_str())));
        pos = comma + 1;
    }
    const LoopbackMode mode = a.has("--exclude") ? LoopbackMode::ExcludeProcessTree : LoopbackMode::IncludeProcessTree;
    std::vector<std::unique_ptr<LoopbackCapture>> caps;
    int opened = 0;
    for (size_t i = 0; i < pids.size(); ++i) {
        auto cap = std::make_unique<LoopbackCapture>();
        std::string where;
        HRESULT hr = cap->open(mode, pids[i], &where);
        if (SUCCEEDED(hr)) {
            hr = cap->start({});
            if (FAILED(hr)) where = "Start";
        }
        std::printf("  #%zu pid %lu (%s) %s: %s%s\n", i + 1, static_cast<unsigned long>(pids[i]),
                    processName(pids[i]).c_str(), modeName(mode), SUCCEEDED(hr) ? "ok" : (where + ": ").c_str(),
                    SUCCEEDED(hr) ? "" : hrText(hr).c_str());
        if (SUCCEEDED(hr)) ++opened;
        caps.push_back(std::move(cap));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (auto& c : caps) c->stop();
    std::cout << opened << " of " << pids.size() << " opened\n";
    return opened == int(pids.size()) ? 0 : 1;
}

// Child process: waits for the parent's start event, then plays a tone.
int cmdTone(const Args& a) {
    if (a.has("--start-event")) {
        HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, widen(a.str("--start-event")).c_str());
        if (ev) {
            WaitForSingleObject(ev, 15000);
            CloseHandle(ev);
        }
    }
    ToneOptions t;
    t.freq = a.num("--freq", 1000);
    t.amplitude = a.num("--amp", 0.1);
    t.seconds = a.num("--seconds", 2);
    t.delayMs = int(a.num("--delay-ms", 0));
    t.reportPath = a.str("--report");
    return playTone(t);
}

// Child process that plays nothing itself but starts a tone grandchild: checks INCLUDE_TARGET_PROCESS_TREE.
int cmdTree(int argc, char** argv) {
    std::wstring args = L"tone";
    for (int i = 2; i < argc; ++i) args += L" " + widen(argv[i]);
    Child grandchild;
    if (!spawn(args, nullptr, grandchild)) return 3;  // inherits the parent's job automatically
    WaitForSingleObject(grandchild.pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(grandchild.pi.hProcess, &code);
    return int(code);
}

struct Probe {
    std::string label;
    LoopbackMode mode;
    DWORD pid;
    LoopbackCapture cap;
    Recording rec;
    HRESULT openHr = E_FAIL;
    std::string where;
};

int cmdSelftest(const Args& a) {
    const int extra = int(a.num("--extra", 30));
    const OsVersion os = osVersion();
    std::cout << "V1 self-test on Windows " << os.major << "." << os.minor << " build " << os.build << "." << os.ubr
              << (os.displayVersion.empty() ? "" : " (" + os.displayVersion + ")") << "\n";
    std::cout << "A short quiet beep (three tones at -20 dBFS, about 2.5 s) plays on the default output.\n\n";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);

    const std::wstring evName = L"Local\\DcV1Start_" + std::to_wstring(GetCurrentProcessId());
    HANDLE startEv = CreateEventW(nullptr, TRUE, FALSE, evName.c_str());
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring reportA = std::wstring(tmp) + L"dc_v1_tone_a.txt";
    DeleteFileW(reportA.c_str());

    const std::wstring common = L" --start-event " + evName + L" --delay-ms 300 --seconds 2.5 --amp 0.1";
    Child childA, childB, childTree;
    const bool spawned = spawn(L"tone --freq 1000 --report \"" + reportA + L"\"" + common, job, childA) &&
                         spawn(L"tone --freq 1500" + common, job, childB) &&
                         spawn(L"tree --freq 700" + common, job, childTree);
    if (!spawned) {
        std::cerr << "could not start the tone processes\n";
        return 1;
    }

    std::vector<std::unique_ptr<Probe>> probes;
    auto add = [&](std::string label, LoopbackMode mode, DWORD pid) {
        auto p = std::make_unique<Probe>();
        p->label = std::move(label);
        p->mode = mode;
        p->pid = pid;
        probes.push_back(std::move(p));
    };
    add("A include (1000 Hz)", LoopbackMode::IncludeProcessTree, childA.pid());
    add("B include (1500 Hz)", LoopbackMode::IncludeProcessTree, childB.pid());
    add("tree include (700 Hz in grandchild)", LoopbackMode::IncludeProcessTree, childTree.pid());
    add("A exclude", LoopbackMode::ExcludeProcessTree, childA.pid());
    add("device loopback", LoopbackMode::DefaultDevice, 0);
    for (int i = 0; i < extra; ++i) add("extra A include #" + std::to_string(i + 1), LoopbackMode::IncludeProcessTree, childA.pid());

    double activationTotal = 0, activationMax = 0;
    for (auto& p : probes) {
        p->openHr = p->cap.open(p->mode, p->pid, &p->where);
        if (SUCCEEDED(p->openHr)) {
            activationTotal += p->cap.activationMs();
            activationMax = std::max(activationMax, p->cap.activationMs());
            Probe* raw = p.get();
            p->openHr = p->cap.start([raw](const float* d, std::uint32_t n, std::uint64_t q) { raw->rec.push(d, n, q); });
            if (FAILED(p->openHr)) p->where = "Start";
        }
    }

    const double cpu0 = cpuSeconds();
    const std::uint64_t wall0 = nowHns();
    SetEvent(startEv);

    // Tones run 0.3 s .. 2.8 s after the start event; keep capturing 1.2 s past the children's exit.
    std::uint64_t exitA = 0;
    while (double(nowHns() - wall0) / 1e7 < 4.5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!exitA && childA.exited()) exitA = nowHns();
    }
    for (auto& p : probes) p->cap.stop();
    const double wallSec = double(nowHns() - wall0) / 1e7;
    const double cpuPct = 100.0 * (cpuSeconds() - cpu0) / wallSec;

    std::uint64_t toneStartA = 0;
    std::ifstream(std::filesystem::path(reportA)) >> toneStartA;

    // ----- report -----
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::cout << (ok ? "  OK   " : "  FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    std::cout << "captures (amplitude at 700 / 1000 / 1500 Hz, linear; tone amplitude is 0.100):\n";
    for (auto& p : probes) {
        if (p->label.rfind("extra", 0) == 0) continue;
        if (FAILED(p->openHr)) {
            std::cout << "  " << p->label << ": open failed at " << p->where << ": " << hrText(p->openHr) << "\n";
            continue;
        }
        const CaptureStats st = p->cap.stats();
        std::printf("  %-38s %.3f / %.3f / %.3f   frames %.0f%% of wall, packets %llu, silent %llu, "
                    "discont %llu, max gap %.1f ms, max packet %.1f ms, err %s\n",
                    p->label.c_str(), p->rec.toneAmplitude(700), p->rec.toneAmplitude(1000), p->rec.toneAmplitude(1500),
                    100.0 * double(st.frames) / (wallSec * kRate), static_cast<unsigned long long>(st.packets),
                    static_cast<unsigned long long>(st.silentPackets), static_cast<unsigned long long>(st.discontinuities),
                    st.maxGapMs, st.maxPacketMs, hrText(st.lastError).c_str());
    }
    std::cout << "\nchecks:\n";

    const bool processOk = SUCCEEDED(probes[0]->openHr) && SUCCEEDED(probes[1]->openHr);
    check(processOk, "process loopback can be activated and started");
    check(SUCCEEDED(probes[4]->openHr), "device loopback can be activated and started (Windows 10 fallback)");

    auto amp = [&](size_t i, double f) { return SUCCEEDED(probes[i]->openHr) ? probes[i]->rec.toneAmplitude(f) : 0.0; };
    const double kHeard = 0.02;  // -34 dBFS: clearly present (the tone is -20 dBFS)
    auto isolated = [&](size_t i, double f, double o1, double o2) {
        const double a0 = amp(i, f);
        return a0 > kHeard && amp(i, o1) < a0 / 30 && amp(i, o2) < a0 / 30;  // other tones 30 dB below
    };
    if (processOk) {
        check(isolated(0, 1000, 700, 1500), "A capture holds only A's tone (1000 Hz), others >30 dB down");
        check(isolated(1, 1500, 700, 1000), "B capture holds only B's tone (1500 Hz), others >30 dB down");
        check(isolated(2, 700, 1000, 1500), "process-tree capture holds the grandchild's tone (700 Hz) only");
        check(SUCCEEDED(probes[3]->openHr) && amp(3, 1000) < amp(0, 1000) / 30 && amp(3, 1500) > kHeard &&
                  amp(3, 700) > kHeard,
              "exclude capture drops A but keeps B and the tree");
    }
    if (SUCCEEDED(probes[4]->openHr))
        check(amp(4, 700) > kHeard && amp(4, 1000) > kHeard && amp(4, 1500) > kHeard,
              "device loopback holds all three tones");

    int extraOk = 0, extraOpened = 0;
    std::string extraError;
    for (size_t i = 5; i < probes.size(); ++i) {
        if (FAILED(probes[i]->openHr)) {
            if (extraError.empty()) extraError = " first failure at " + probes[i]->where + ": " + hrText(probes[i]->openHr);
            continue;
        }
        ++extraOpened;
        if (isolated(i, 1000, 700, 1500)) ++extraOk;
    }
    // Windows 10 (19045) refuses a second include capture of the same pid inside one process
    // (IAudioClient::Initialize -> E_UNEXPECTED); Windows 11 allows it. Not a failure: the engine opens
    // each pid once and fans the audio out. Every capture that did open must carry the tone.
    if (extra > 0) {
        check(extraOk == extraOpened, std::to_string(extraOk) + " of " + std::to_string(extraOpened) +
                                          " opened extra captures of A got A's tone");
        std::cout << "  INFO same-pid captures in one process: " << extraOpened << " of " << extra
                  << " extra opened" << (extraOpened == extra ? " (supported)" : " (not supported:" + extraError + ")")
                  << "\n";
    }

    bool noErrors = true;
    for (auto& p : probes)
        if (SUCCEEDED(p->openHr) && p->cap.stats().lastError != S_OK) noErrors = false;
    check(noErrors, "no capture failed while running, including after the tone processes exited");

    std::cout << "\nmeasurements:\n";
    size_t opened = 0;
    for (auto& p : probes) opened += SUCCEEDED(p->openHr) ? 1 : 0;
    std::printf("  activation: %.1f ms average, %.1f ms max over %zu opened captures\n",
                activationTotal / double(std::max<size_t>(1, opened)), activationMax, opened);
    std::printf("  CPU of this process with %zu captures running: %.2f%% of one core\n", probes.size(), cpuPct);
    if (processOk && toneStartA) {
        const Recording& r = probes[0]->rec;
        const size_t onset = r.toneOnset(1000, 0.03);
        if (onset != SIZE_MAX) {
            const double latencyMs = (double(r.qpcOf(onset)) - double(toneStartA)) / 10000.0;
            std::printf("  A: tone submitted -> seen in capture (QPC of first sample): %.1f ms\n", latencyMs);
        }
        std::printf("  A: peak before the tone started: %.1f dBFS\n", db(r.peakBefore(toneStartA)));
        if (SUCCEEDED(probes[4]->openHr)) {
            const size_t d = probes[4]->rec.toneOnset(1000, 0.03);
            if (d != SIZE_MAX)
                std::printf("  device: tone submitted -> seen in capture: %.1f ms\n",
                            (double(probes[4]->rec.qpcOf(d)) - double(toneStartA)) / 10000.0);
        }
    }
    if (processOk && exitA) {
        const CaptureStats st = probes[0]->cap.stats();
        std::printf("  A: frames delivered after A exited: %zu (%.0f ms); capture of A delivered %.0f%% of wall-clock frames\n",
                    probes[0]->rec.framesAfter(exitA), 1000.0 * double(probes[0]->rec.framesAfter(exitA)) / kRate,
                    100.0 * double(st.frames) / (wallSec * kRate));
    }

    CloseHandle(startEv);
    CloseHandle(job);
    std::cout << "\n" << (failures == 0 ? "V1 SELFTEST PASSED" : std::to_string(failures) + " CHECK(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}

void usage() {
    std::cout << "usage: dc-loopback-probe info | list | selftest [--extra N]\n"
                 "       dc-loopback-probe capture (--pid N | --name x.exe) [--exclude] [--seconds S] [--wav f.wav]\n"
                 "       dc-loopback-probe capture --device [--seconds S] [--wav f.wav]\n";
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Args a = parseArgs(argc, argv);
    const std::string cmd = a.pos.empty() ? "" : a.pos[0];
    int rc = 2;
    if (cmd == "info") rc = cmdInfo();
    else if (cmd == "list") rc = cmdList();
    else if (cmd == "capture") rc = cmdCapture(a);
    else if (cmd == "selftest") rc = cmdSelftest(a);
    else if (cmd == "multi") rc = cmdMulti(a);
    else if (cmd == "tone") rc = cmdTone(a);
    else if (cmd == "tree") rc = cmdTree(argc, argv);
    else usage();
    CoUninitialize();
    return rc;
}
