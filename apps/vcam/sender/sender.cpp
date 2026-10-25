#include "dc/vcam/sender.hpp"

#include "dc/vcam/ids.hpp"
#include "dc/vcam/pixels.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace dc::vcam {

namespace {

std::int64_t qpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

std::int64_t qpcFreq() {
    static const std::int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

// RtlGetVersion: GetVersionEx lies to processes without a compatibility manifest.
unsigned long windowsBuild() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            if (fn(&v) == 0) return v.dwBuildNumber;
        }
    }
    return 0;
}

// Folder of the installed camera DLLs, from %ProgramData%\DesktopCapture\vcam\current.txt (vcam-register.ps1).
std::wstring installFolder() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::ifstream f(std::filesystem::path(std::wstring(buf) + L"\\DesktopCapture\\vcam\\current.txt"));
    std::string line;
    if (!std::getline(f, line)) return {};
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) line.pop_back();
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // UTF-8 BOM
    const int wn = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), int(line.size()), w.data(), wn);
    return w;
}

}  // namespace

CameraKind systemCameraKind() {
    return windowsBuild() >= kFirstMfVirtualCameraBuild ? CameraKind::MediaFoundation : CameraKind::DirectShow;
}

// --- Media Foundation: bridge section --------------------------------------------------------------------------

struct Sender::Mf {
    HANDLE map = nullptr;
    HANDLE event = nullptr;
    BridgeHeader* header = nullptr;
    std::int64_t nextOpenQpc = 0;
    std::uint64_t index = 0;

    ~Mf() { close(); }

    bool attach() {
        if (header) return true;
        const std::int64_t now = qpcNow();
        if (now < nextOpenQpc) return false;
        nextOpenQpc = now + qpcFreq() / 2;
        std::wstring name;
        for (const wchar_t* n : {kBridgeGlobalName, kBridgeLocalName}) {
            map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, n);
            if (map) {
                name = n;
                break;
            }
        }
        if (!map) return false;  // no camera app has the camera open
        header = static_cast<BridgeHeader*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!header || header->magic != kBridgeMagic || header->version != kBridgeVersion) {
            close();
            return false;
        }
        header->producer_pid.store(GetCurrentProcessId());
        event = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + kBridgeEventSuffix).c_str());
        return true;
    }

    // fill(dst) writes the frame into the slot.
    template <class Fill>
    void publish(PixelFormat format, int w, int h, std::int64_t qpc, Fill&& fill) {
        if (!attach()) return;
        if (w <= 0 || h <= 0 || std::uint32_t(w) > kBridgeMaxWidth || std::uint32_t(h) > kBridgeMaxHeight) return;
        const std::uint32_t slot = (header->latest.load() + 1) & 1u;
        auto& sl = header->slots[slot];
        sl.seq.fetch_add(1);  // odd: writing
        fill(bridge_slot_pixels(header, slot));
        sl.width = std::uint32_t(w);
        sl.height = std::uint32_t(h);
        sl.format = std::uint32_t(format);
        sl.qpc = qpc;
        sl.frame_index = index++;
        sl.seq.fetch_add(1);  // even: complete
        header->latest.store(slot, std::memory_order_release);
        header->producer_heartbeat_qpc.store(qpcNow());
        header->producer_pid.store(GetCurrentProcessId());
        // When the last app closes the camera the Frame Server unloads the source, but our handles keep the
        // section and the event alive, and the next source instance opens them again by name.
        if (event) SetEvent(event);
    }

    bool consumer() const {
        if (!header) return false;
        const std::int64_t age = qpcNow() - header->consumer_heartbeat_qpc.load();
        return header->consumer_frames.load() > 0 && age >= 0 && age < qpcFreq() / 2;
    }

    void close() {
        if (header) {
            header->producer_pid.store(0);
            UnmapViewOfFile(header);
        }
        if (map) CloseHandle(map);
        if (event) CloseHandle(event);
        header = nullptr;
        map = nullptr;
        event = nullptr;
    }
};

// --- DirectShow: softcam sender API in vcam_ds.dll ----------------------------------------------------------------

struct Sender::Ds {
    using CreateFn = void*(__cdecl*)(int, int, float);
    using DeleteFn = void(__cdecl*)(void*);
    using SendFn = void(__cdecl*)(void*, const void*);
    using ConnectedFn = bool(__cdecl*)(void*);

    HMODULE module = nullptr;
    CreateFn create = nullptr;
    DeleteFn destroy = nullptr;
    SendFn send = nullptr;
    ConnectedFn connected = nullptr;
    void* camera = nullptr;
    std::vector<std::uint8_t> bgr;
    std::string error;
    std::int64_t nextTryQpc = 0;

    ~Ds() {
        if (camera) destroy(camera);
        // The module stays loaded: softcam's watchdog thread may still run inside it.
    }

    bool ready() {
        if (camera) return true;
        const std::int64_t now = qpcNow();
        if (now < nextTryQpc) return false;
        nextTryQpc = now + 2 * qpcFreq();
        if (!module) {
            const std::wstring dir = installFolder();
            if (dir.empty()) {
                error = "virtual camera not installed (run scripts\\vcam-register.ps1)";
                return false;
            }
            module = LoadLibraryW((dir + L"\\vcam_ds.dll").c_str());
            if (!module) {
                error = "cannot load vcam_ds.dll from the install folder (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "scCreateCamera")));
            destroy = reinterpret_cast<DeleteFn>(reinterpret_cast<void*>(GetProcAddress(module, "scDeleteCamera")));
            send = reinterpret_cast<SendFn>(reinterpret_cast<void*>(GetProcAddress(module, "scSendFrame")));
            connected = reinterpret_cast<ConnectedFn>(reinterpret_cast<void*>(GetProcAddress(module, "scIsConnected")));
            if (!create || !destroy || !send || !connected) {
                error = "vcam_ds.dll has no sender API";
                FreeLibrary(module);
                module = nullptr;
                return false;
            }
        }
        camera = create(kDsWidth, kDsHeight, 0.0f);  // 0: we pace the frames ourselves
        if (!camera) {
            error = "the DirectShow camera is fed by another program";
            return false;
        }
        error.clear();
        bgr.resize(std::size_t(kDsWidth) * kDsHeight * 3);
        return true;
    }
};

// ------------------------------------------------------------------------------------------------------------------

Sender::Sender(CameraKind kind) : kind_(kind) {
    if (kind_ == CameraKind::MediaFoundation) mf_ = std::make_unique<Mf>();
    else ds_ = std::make_unique<Ds>();
}

Sender::~Sender() = default;

void Sender::sendNv12(const std::uint8_t* y, int yPitch, const std::uint8_t* uv, int uvPitch, int width, int height,
                      std::int64_t qpc) {
    if (!mf_ || (width | height) & 1) return;
    mf_->publish(PixelFormat::Nv12, width, height, qpc, [&](std::uint8_t* dst) {
        for (int r = 0; r < height; ++r) std::memcpy(dst + std::ptrdiff_t(r) * width, y + std::ptrdiff_t(r) * yPitch, std::size_t(width));
        std::uint8_t* duv = dst + std::ptrdiff_t(width) * height;
        for (int r = 0; r < height / 2; ++r)
            std::memcpy(duv + std::ptrdiff_t(r) * width, uv + std::ptrdiff_t(r) * uvPitch, std::size_t(width));
    });
}

void Sender::sendBgra(const std::uint8_t* bgra, int pitch, int width, int height, std::int64_t qpc) {
    if (mf_) {
        mf_->publish(PixelFormat::Bgra, width, height, qpc, [&](std::uint8_t* dst) {
            for (int r = 0; r < height; ++r)
                std::memcpy(dst + std::ptrdiff_t(r) * width * 4, bgra + std::ptrdiff_t(r) * pitch, std::size_t(width) * 4);
        });
        return;
    }
    if (!ds_->ready()) return;
    if (width != kDsWidth || height != kDsHeight) {
        ds_->error = "the DirectShow camera takes 1920x1080 only";
        return;
    }
    bgra_to_bgr24(bgra, pitch, width, height, ds_->bgr.data());
    ds_->send(ds_->camera, ds_->bgr.data());
}

void Sender::detach() {
    if (mf_) mf_->close();
    if (ds_ && ds_->camera) {
        ds_->destroy(ds_->camera);
        ds_->camera = nullptr;
    }
}

Sender::Status Sender::status() const {
    Status s;
    if (mf_) {
        s.installed = true;
        s.attached = mf_->header != nullptr;
        s.consumer = mf_->consumer();
        s.sent = mf_->index;
    } else {
        s.installed = ds_->module != nullptr;
        s.attached = ds_->camera != nullptr;
        s.consumer = ds_->camera && ds_->connected(ds_->camera);
        s.error = ds_->error;
    }
    return s;
}

}  // namespace dc::vcam
