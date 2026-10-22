// V4 prototype: test-picture producer, standing in for capd.
//
//   vcam-feed [--target mf|ds|both] [--width 1920] [--height 1080] [--fps 30] [--seconds N]
//
// mf: writes into the bridge section created by the MF source (retries until a camera app has
//     opened the camera, because the section only exists while the Frame Server runs our source)
// ds: loads vcam_ds.dll from its own folder and uses the softcam sender API
// Every frame carries the stamp from common/pattern.hpp so vcam-probe can measure it.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/bridge.hpp"
#include "../common/pattern.hpp"

namespace {

volatile bool g_stop = false;
BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

int64_t qpc_now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

class MfBridgeWriter {
public:
    ~MfBridgeWriter() { close(); }

    bool attached() const { return header_ != nullptr; }

    void try_open() {
        if (header_) return;
        for (const wchar_t* name : {dc::vcam::kBridgeGlobalName, dc::vcam::kBridgeLocalName}) {
            map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (map_) {
                name_ = name;
                break;
            }
        }
        if (!map_) return;
        header_ = static_cast<dc::vcam::BridgeHeader*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!header_ || header_->magic != dc::vcam::kBridgeMagic || header_->version != dc::vcam::kBridgeVersion) {
            close();
            return;
        }
        header_->producer_pid.store(GetCurrentProcessId());
        std::printf("mf: attached to %ls\n", name_.c_str());
    }

    void write(const uint8_t* bgra, int w, int h, const dc::vcam::Stamp& s) {
        if (!header_) return;
        const uint32_t slot = (header_->latest.load() + 1) & 1u;
        auto& sl = header_->slots[slot];
        sl.seq.fetch_add(1);  // odd: writing
        std::memcpy(dc::vcam::bridge_slot_pixels(header_, slot), bgra, size_t(w) * h * 4);
        sl.width = uint32_t(w);
        sl.height = uint32_t(h);
        sl.qpc = s.qpc;
        sl.frame_index = s.frame_index;
        sl.seq.fetch_add(1);  // even: complete
        header_->latest.store(slot, std::memory_order_release);
        header_->producer_heartbeat_qpc.store(qpc_now());
        header_->producer_pid.store(GetCurrentProcessId());
    }

    uint64_t consumer_frames() const { return header_ ? header_->consumer_frames.load() : 0; }

    void close() {
        if (header_) {
            header_->producer_pid.store(0);
            UnmapViewOfFile(header_);
        }
        if (map_) CloseHandle(map_);
        header_ = nullptr;
        map_ = nullptr;
    }

private:
    HANDLE map_ = nullptr;
    dc::vcam::BridgeHeader* header_ = nullptr;
    std::wstring name_;
};

class DsSender {
public:
    using CreateFn = void*(__cdecl*)(int, int, float);
    using DeleteFn = void(__cdecl*)(void*);
    using SendFn = void(__cdecl*)(void*, const void*);
    using ConnectedFn = bool(__cdecl*)(void*);

    bool start(int w, int h) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dll(path);
        dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"vcam_ds.dll";
        mod_ = LoadLibraryW(dll.c_str());
        if (!mod_) {
            std::printf("ds: cannot load %ls (error %lu)\n", dll.c_str(), GetLastError());
            return false;
        }
        create_ = reinterpret_cast<CreateFn>(GetProcAddress(mod_, "scCreateCamera"));
        delete_ = reinterpret_cast<DeleteFn>(GetProcAddress(mod_, "scDeleteCamera"));
        send_ = reinterpret_cast<SendFn>(GetProcAddress(mod_, "scSendFrame"));
        connected_ = reinterpret_cast<ConnectedFn>(GetProcAddress(mod_, "scIsConnected"));
        if (!create_ || !delete_ || !send_ || !connected_) return false;
        cam_ = create_(w, h, 0.0f);  // 0: we pace frames ourselves
        if (!cam_) {
            std::printf("ds: scCreateCamera failed (another producer running?)\n");
            return false;
        }
        bgr_.resize(size_t(w) * h * 3);
        std::printf("ds: sender ready %dx%d\n", w, h);
        return true;
    }

    void write(const uint8_t* bgra, int w, int h) {
        if (!cam_) return;
        for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
            bgr_[i * 3 + 0] = bgra[i * 4 + 0];
            bgr_[i * 3 + 1] = bgra[i * 4 + 1];
            bgr_[i * 3 + 2] = bgra[i * 4 + 2];
        }
        send_(cam_, bgr_.data());
    }

    bool connected() const { return cam_ && connected_(cam_); }

    ~DsSender() {
        if (cam_) delete_(cam_);
    }

private:
    HMODULE mod_ = nullptr;
    CreateFn create_ = nullptr;
    DeleteFn delete_ = nullptr;
    SendFn send_ = nullptr;
    ConnectedFn connected_ = nullptr;
    void* cam_ = nullptr;
    std::vector<uint8_t> bgr_;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring target = L"both";
    int w = 1920, h = 1080;
    double fps = 30, seconds = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--target") target = argv[i + 1];
        else if (k == L"--width") w = _wtoi(argv[i + 1]);
        else if (k == L"--height") h = _wtoi(argv[i + 1]);
        else if (k == L"--fps") fps = _wtof(argv[i + 1]);
        else if (k == L"--seconds") seconds = _wtof(argv[i + 1]);
    }
    if (w <= 0 || h <= 0 || w > int(dc::vcam::kBridgeMaxWidth) || h > int(dc::vcam::kBridgeMaxHeight) || w % 4 ||
        h % 4 || fps <= 0) {
        std::printf("bad size/fps (max %ux%u, multiples of 4)\n", dc::vcam::kBridgeMaxWidth, dc::vcam::kBridgeMaxHeight);
        return 2;
    }
    const bool use_mf = target == L"mf" || target == L"both";
    const bool use_ds = target == L"ds" || target == L"both";
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    timeBeginPeriod(1);

    MfBridgeWriter mf;
    DsSender ds;
    if (use_ds && !ds.start(w, h) && !use_mf) return 1;

    std::vector<uint8_t> frame(size_t(w) * h * 4);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const int64_t period = int64_t(double(f.QuadPart) / fps);
    const int64_t t0 = qpc_now();
    int64_t next = t0, last_report = t0, last_open_try = 0;
    uint32_t index = 0;
    std::printf("feeding %dx%d @ %.0f fps (%ls)%s\n", w, h, fps, target.c_str(),
                seconds > 0 ? "" : ", Ctrl+C to stop");
    std::fflush(stdout);
    while (!g_stop && (seconds <= 0 || qpc_now() - t0 < int64_t(seconds * double(f.QuadPart)))) {
        const int64_t now = qpc_now();
        if (use_mf && !mf.attached() && now - last_open_try > f.QuadPart / 2) {
            mf.try_open();
            last_open_try = now;
        }
        const dc::vcam::Stamp stamp{now, index++};
        dc::vcam::draw_test_pattern_bgra(frame.data(), w, h, w * 4, stamp);
        if (use_mf) mf.write(frame.data(), w, h, stamp);
        if (use_ds) ds.write(frame.data(), w, h);
        if (now - last_report >= f.QuadPart) {
            std::printf("frame %u | mf: %s (source delivered %llu) | ds: %s\n", index,
                        use_mf ? (mf.attached() ? "attached" : "waiting for camera") : "off", mf.consumer_frames(),
                        use_ds ? (ds.connected() ? "app connected" : "no app") : "off");
            std::fflush(stdout);
            last_report = now;
        }
        next += period;
        const int64_t wait = next - qpc_now();
        if (wait > 0) Sleep(DWORD(wait * 1000 / f.QuadPart));
        else if (wait < -period) next = qpc_now();
    }
    timeEndPeriod(1);
    std::printf("stopped after %u frames\n", index);
    return 0;
}
