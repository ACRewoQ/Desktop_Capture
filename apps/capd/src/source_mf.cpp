// Cameras / capture boards and video files through the Media Foundation source reader. The reader runs on a thread
// of its own (ReadSample blocks); it decodes and converts to RGB32 on the GPU when the driver allows it (DXGI device
// manager with our device), otherwise in system memory. The video thread copies the newest frame into the source's
// texture in poll(). Audio of video files is not used (the audio side belongs to the engine).

#include "video_source.hpp"

#include "text_renderer.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace dc::capd {

namespace {

std::string hrText(const char* what, HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, " (hr=0x%08lX)", static_cast<unsigned long>(hr));
    return std::string(what) + buf;
}

std::string allocatedString(IMFAttributes* a, REFGUID key) {
    WCHAR* s = nullptr;
    UINT32 len = 0;
    std::string out;
    if (SUCCEEDED(a->GetAllocatedString(key, &s, &len))) {
        out = narrow(s);
        CoTaskMemFree(s);
    }
    return out;
}

struct CameraEntry {
    CameraInfo info;
    ComPtr<IMFActivate> activate;
};

std::vector<CameraEntry> enumerateCameras() {
    std::vector<CameraEntry> out;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        CameraEntry e;
        e.activate.Attach(devices[i]);  // takes the reference
        e.info.id = allocatedString(e.activate.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        e.info.name = allocatedString(e.activate.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        out.push_back(std::move(e));
    }
    CoTaskMemFree(devices);
    return out;
}

// Thrown while opening: state "disconnected" (target missing) or "error".
struct OpenError {
    std::string state;
    std::string message;
};

class MfSource final : public VideoSource {
public:
    MfSource(const SourceContext& ctx, const SourceConfig& cfg, bool camera)
        : dev_(*ctx.device), cfg_(cfg), camera_(camera) {
        thread_ = std::thread([this] { run(); });
    }

    ~MfSource() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool poll() override {
        ComPtr<IMFSample> sample;
        int w = 0, h = 0;
        LONG stride = 0;
        {
            std::lock_guard lock(mutex_);
            sample.Swap(pending_);
            w = frameW_;
            h = frameH_;
            stride = stride_;
        }
        if (!sample || w <= 0 || h <= 0) return false;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buffer.As(&dxgi))) {
            ComPtr<ID3D11Texture2D> src;
            UINT sub = 0;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&src))) || FAILED(dxgi->GetSubresourceIndex(&sub))) return false;
            D3D11_TEXTURE2D_DESC desc{};
            src->GetDesc(&desc);
            const UINT cw = std::min<UINT>(UINT(w), desc.Width), ch = std::min<UINT>(UINT(h), desc.Height);
            // Same format as the decoder output (B8G8R8X8 or B8G8R8A8): copies need matching formats.
            slot_.ensure(dev_.device.Get(), cw, ch, desc.Format, D3D11_BIND_SHADER_RESOURCE);
            const D3D11_BOX box{0, 0, 0, cw, ch, 1};
            dev_.context->CopySubresourceRegion(slot_.texture(), 0, 0, 0, 0, src.Get(), sub, &box);
            width_ = int(cw);
            height_ = int(ch);
        } else {
            // System memory RGB32 (BGRX in memory).
            slot_.ensure(dev_.device.Get(), UINT(w), UINT(h), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            ComPtr<IMF2DBuffer> buf2d;
            BYTE* data = nullptr;
            LONG pitch = 0;
            bool locked2d = false;
            DWORD len = 0;
            if (SUCCEEDED(buffer.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&data, &pitch))) {
                locked2d = true;
            } else if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
                pitch = stride != 0 ? stride : LONG(w) * 4;
                if (pitch < 0) data += size_t(-pitch) * size_t(h - 1);  // bottom-up: start at the top row
            } else {
                return false;
            }
            if (pitch > 0) {
                dev_.context->UpdateSubresource(slot_.texture(), 0, nullptr, data, UINT(pitch), 0);
            } else {
                // Bottom-up rows: copy them into top-down order first.
                flip_.resize(size_t(w) * 4 * size_t(h));
                for (int y = 0; y < h; ++y)
                    std::memcpy(flip_.data() + size_t(y) * size_t(w) * 4, data + std::ptrdiff_t(y) * pitch, size_t(w) * 4);
                dev_.context->UpdateSubresource(slot_.texture(), 0, nullptr, flip_.data(), UINT(w) * 4, 0);
            }
            if (locked2d) buf2d->Unlock2D();
            else buffer->Unlock();
            width_ = w;
            height_ = h;
        }
        return true;
    }

    ID3D11ShaderResourceView* view() override { return slot_.view(dev_.device.Get()); }
    int width() const override { return slot_.texture() ? width_ : 0; }
    int height() const override { return slot_.texture() ? height_ : 0; }

    SourceStatus status() const override {
        std::lock_guard lock(mutex_);
        return status_;
    }
    std::string description() const override {
        std::lock_guard lock(mutex_);
        return (camera_ ? "camera " : "media ") + name_ + (gpu_ ? " (gpu)" : " (cpu)");
    }
    std::uint64_t recoveries() const override { return reopens_; }

private:
    void setStatus(SourceStatus s) {
        std::lock_guard lock(mutex_);
        status_ = std::move(s);
    }

    bool sleepFor(int ms) {
        for (int i = 0; i < ms / 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return !stop_;
    }

    void run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_) {
            ComPtr<IMFSourceReader> reader;
            try {
                reader = open();
            } catch (const OpenError& e) {
                setStatus({e.state, e.state == "error" ? e.message : std::string(), 0, 0});
                if (!sleepFor(2000)) break;
                continue;
            }
            readLoop(reader.Get());
            reader.Reset();
            if (!stop_) {
                ++reopens_;
                setStatus({"disconnected", "", 0, 0});
                sleepFor(1000);
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    ComPtr<IMFSourceReader> open() {
        // Try the GPU path first; some drivers / codecs only work with system memory output.
        for (const bool gpu : {true, false}) {
            ComPtr<IMFAttributes> attrs;
            MFCreateAttributes(&attrs, 4);
            ComPtr<IMFDXGIDeviceManager> manager;
            if (gpu) {
                UINT token = 0;
                if (FAILED(MFCreateDXGIDeviceManager(&token, &manager)) ||
                    FAILED(manager->ResetDevice(dev_.device.Get(), token)))
                    continue;
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.Get());
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            } else {
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            }

            ComPtr<IMFSourceReader> reader;
            HRESULT hr = S_OK;
            if (camera_) {
                ComPtr<IMFMediaSource> source = activateCamera();
                hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
                if (FAILED(hr)) source->Shutdown();
            } else {
                if (cfg_.file.empty()) throw OpenError{"error", "no file chosen"};
                if (GetFileAttributesW(widen(cfg_.file).c_str()) == INVALID_FILE_ATTRIBUTES)
                    throw OpenError{"error", "cannot open " + cfg_.file};
                hr = MFCreateSourceReaderFromURL(widen(cfg_.file).c_str(), attrs.Get(), &reader);
                std::lock_guard lock(mutex_);
                name_ = cfg_.file;
            }
            if (FAILED(hr)) continue;
            if (!configure(reader.Get())) continue;
            gpu_ = gpu;
            return reader;
        }
        throw OpenError{"error", camera_ ? "the camera does not deliver a usable video format"
                                         : "cannot decode " + cfg_.file};
    }

    ComPtr<IMFMediaSource> activateCamera() {
        auto cams = enumerateCameras();
        CameraEntry* pick = nullptr;
        for (auto& c : cams)
            if (!cfg_.cameraId.empty() && c.info.id == cfg_.cameraId) pick = &c;
        if (!pick)
            for (auto& c : cams)
                if (!cfg_.cameraName.empty() && c.info.name == cfg_.cameraName) pick = &c;  // the ID changed (other USB port)
        if (!pick && cfg_.cameraId.empty() && cfg_.cameraName.empty() && !cams.empty()) pick = &cams.front();
        if (!pick) throw OpenError{"disconnected", ""};
        {
            std::lock_guard lock(mutex_);
            name_ = pick->info.name;
        }
        ComPtr<IMFMediaSource> source;
        const HRESULT hr = pick->activate->ActivateObject(IID_PPV_ARGS(&source));
        if (hr == E_ACCESSDENIED) throw OpenError{"error", "camera access is turned off in the Windows privacy settings"};
        if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == MF_E_HW_MFT_FAILED_START_STREAMING ||
            hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
            throw OpenError{"error", "the camera is in use by another application"};
        if (FAILED(hr)) throw OpenError{"error", hrText("cannot open the camera", hr)};
        return source;
    }

    // Picks the native format (cameras) and asks for RGB32 output.
    bool configure(IMFSourceReader* reader) {
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(stream, TRUE);
        if (camera_) {
            ComPtr<IMFMediaType> best;
            double bestScore = -1e18;
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> t;
                if (FAILED(reader->GetNativeMediaType(stream, i, &t))) break;
                UINT32 w = 0, h = 0, num = 0, den = 1;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
                const double fps = den ? double(num) / den : 0;
                double score;
                if (cfg_.cameraWidth > 0 || cfg_.cameraHeight > 0 || cfg_.cameraFps > 0) {
                    // closest to the requested size and rate
                    score = 0;
                    if (cfg_.cameraWidth > 0) score -= std::abs(double(w) - cfg_.cameraWidth);
                    if (cfg_.cameraHeight > 0) score -= std::abs(double(h) - cfg_.cameraHeight);
                    if (cfg_.cameraFps > 0) score -= std::abs(fps - cfg_.cameraFps) * 10;
                } else {
                    // automatic: the largest size up to 1920x1080, then the highest rate up to 60
                    const bool fits = w <= 1920 && h <= 1080;
                    score = (fits ? 1e12 : 0) + (fits ? double(w) * h : -double(w) * h) * 1000 + std::min(fps, 60.0);
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = t;
                }
            }
            if (best) reader->SetCurrentMediaType(stream, nullptr, best.Get());
        }
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader->SetCurrentMediaType(stream, nullptr, out.Get()))) return false;
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader->GetCurrentMediaType(stream, &cur))) return false;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w == 0 || h == 0) return false;
        std::lock_guard lock(mutex_);
        frameW_ = int(w);
        frameH_ = int(h);
        stride_ = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
        return true;
    }

    void readLoop(IMFSourceReader* reader) {
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto nowUs = [&] {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return double(c.QuadPart) * 1e6 / double(f.QuadPart);
        };
        double clockStartUs = nowUs();
        LONGLONG firstTs = -1;
        bool gotFrame = false;
        while (!stop_) {
            DWORD actual = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader->ReadSample(stream, 0, &actual, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return;  // unplugged / decoder failure: reopen
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(reader->GetCurrentMediaType(stream, &cur))) {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                    std::lock_guard lock(mutex_);
                    frameW_ = int(w);
                    frameH_ = int(h);
                    stride_ = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (camera_) return;
                if (!gotFrame) {
                    setStatus({"error", "no video in " + cfg_.file, 0, 0});
                    sleepFor(5000);
                    return;
                }
                if (!cfg_.mediaLoop) {
                    // keep showing the last frame until the settings change
                    while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return;
                }
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pos);
                firstTs = -1;
                continue;
            }
            if (!sample) continue;
            if (!camera_) {
                // play files in real time: wait until the frame's time has come
                if (firstTs < 0) {
                    firstTs = ts;
                    clockStartUs = nowUs();
                }
                const double dueUs = clockStartUs + double(ts - firstTs) / 10.0;
                while (!stop_ && nowUs() < dueUs) {
                    const double waitMs = (dueUs - nowUs()) / 1000.0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<int>(int(waitMs), 1, 20)));
                }
            }
            gotFrame = true;
            std::lock_guard lock(mutex_);
            pending_ = sample;
            status_ = {"ok", "", frameW_, frameH_};
        }
    }

    Device dev_;
    SourceConfig cfg_;
    bool camera_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> reopens_{0};
    std::atomic<bool> gpu_{false};

    mutable std::mutex mutex_;  // guards the fields below
    ComPtr<IMFSample> pending_;
    int frameW_ = 0, frameH_ = 0;
    LONG stride_ = 0;
    SourceStatus status_;
    std::string name_;

    // video thread only
    TextureSlot slot_;
    int width_ = 0, height_ = 0;
    std::vector<std::uint8_t> flip_;
};

}  // namespace

std::unique_ptr<VideoSource> createCameraSource(const SourceContext& ctx, const SourceConfig& cfg) {
    return std::make_unique<MfSource>(ctx, cfg, true);
}

std::unique_ptr<VideoSource> createMediaSource(const SourceContext& ctx, const SourceConfig& cfg) {
    return std::make_unique<MfSource>(ctx, cfg, false);
}

std::vector<CameraInfo> listCameras() {
    std::vector<CameraInfo> out;
    for (auto& e : enumerateCameras()) out.push_back(e.info);
    return out;
}

}  // namespace dc::capd
