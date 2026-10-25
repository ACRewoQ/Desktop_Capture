// Media Foundation virtual camera (Windows 11, MFCreateVirtualCamera). From the V4 verification, made for P3:
// the bridge carries NV12 from capd, which the source copies as is when the app asks for that size.
//
// The Frame Server service loads this DLL, creates VCamActivate (CLSID below) and activates a
// VCamSource from it. The source exposes one colour stream (NV12 / RGB32, 1080p30 / 720p30).
// Frames come from the shared-memory bridge (dc/vcam/bridge.hpp) written by the producer; when no
// producer is attached the stream keeps running with a "no signal" picture, so camera apps never
// see the device stall (design: "仮想カメラは信号なし画像を出し続ける").
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <wrl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dc/vcam/bridge.hpp"
#include "dc/vcam/ids.hpp"
#include "dc/vcam/log.hpp"
#include "dc/vcam/pixels.hpp"

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

void LOG(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dc::vcam::vlog(L"vcam-mf", fmt, ap);
    va_end(ap);
}

#define RETURN_IF_FAILED_LOG(expr)                                       \
    do {                                                                 \
        const HRESULT hr_ = (expr);                                      \
        if (FAILED(hr_)) {                                               \
            LOG("%s failed 0x%08lx (line %d)", #expr, hr_, __LINE__);    \
            return hr_;                                                  \
        }                                                                \
    } while (0)

// Undocumented attribute the Frame Server sets on the activate: PID of the camera app (as used by
// the VCamSample reference implementation). Only for the log.
constexpr GUID kFrameServerClientPid = {0x5f8d322e, 0x0fe4, 0x43e4, {0x9e, 0x50, 0xd8, 0x3e, 0xcd, 0x9f, 0xc2, 0xb8}};

std::string guid_str(REFGUID g) {
    wchar_t w[64] = {};
    StringFromGUID2(g, w, 64);
    char a[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), nullptr, nullptr);
    return a;
}

std::string process_name(DWORD pid) {
    std::string out = "?";
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            char a[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof(a), nullptr, nullptr);
            out = a;
        }
        CloseHandle(h);
    }
    return out;
}

constexpr UINT32 kFps = 30;
constexpr DWORD kStreamId = 0;

int64_t qpc_now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
int64_t qpc_freq() {
    static const int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

// ---------------------------------------------------------------------------------------------
// Bridge reader (consumer side; the source owns the section, see bridge.hpp)

class BridgeReader {
public:
    ~BridgeReader() {
        if (header_) UnmapViewOfFile(header_);
        if (map_) CloseHandle(map_);
        if (frame_event_) CloseHandle(frame_event_);
    }

    void open() {
        if (header_) return;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        // SYSTEM, LOCAL SERVICE, Administrators and any authenticated user (the producer in the
        // interactive session) get full access to the section.
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;AU)",
                                                                 SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        const auto size = dc::vcam::kBridgeTotalBytes;
        for (const wchar_t* name : {dc::vcam::kBridgeGlobalName, dc::vcam::kBridgeLocalName}) {
            map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(size >> 32), DWORD(size), name);
            if (map_) {
                LOG("bridge: section %ls %s", name, GetLastError() == ERROR_ALREADY_EXISTS ? "opened" : "created");
                // Producer signals every published frame, so delivery follows its clock.
                frame_event_ = CreateEventW(&sa, FALSE, FALSE, (std::wstring(name) + dc::vcam::kBridgeEventSuffix).c_str());
                if (!frame_event_) LOG("bridge: CreateEvent failed, error %lu", GetLastError());
                break;
            }
            LOG("bridge: CreateFileMapping(%ls) failed, error %lu (normal outside the Frame Server)", name, GetLastError());
        }
        if (sd) LocalFree(sd);
        if (!map_) return;
        header_ = static_cast<dc::vcam::BridgeHeader*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!header_) {
            LOG("bridge: MapViewOfFile failed, error %lu", GetLastError());
            return;
        }
        if (header_->magic != dc::vcam::kBridgeMagic) {
            header_->version = dc::vcam::kBridgeVersion;
            header_->header_bytes = dc::vcam::kBridgeHeaderBytes;
            header_->slot_bytes = dc::vcam::kBridgeSlotBytes;
            header_->latest = 0;
            header_->producer_pid = 0;
            header_->magic = dc::vcam::kBridgeMagic;
        }
    }

    bool producer_alive() const {
        if (!header_ || header_->producer_pid.load() == 0) return false;
        const int64_t age = qpc_now() - header_->producer_heartbeat_qpc.load();
        return age >= 0 && age < dc::vcam::kBridgeStaleMs * qpc_freq() / 1000;
    }

    // Calls fn(format, pixels, width, height) with the newest frame. Returns false when there is no usable frame
    // or it changed underneath us (the caller shows it anyway; one torn frame is acceptable here).
    template <class Fn>
    bool with_latest(Fn&& fn) {
        const uint32_t slot = header_->latest.load(std::memory_order_acquire) & 1u;
        auto& s = header_->slots[slot];
        const uint64_t seq = s.seq.load(std::memory_order_acquire);
        const uint32_t w = s.width, h = s.height;
        const auto format = static_cast<dc::vcam::PixelFormat>(s.format);
        if ((seq & 1u) || w == 0 || h == 0 || w > dc::vcam::kBridgeMaxWidth || h > dc::vcam::kBridgeMaxHeight ||
            (w | h) & 1u || (format != dc::vcam::PixelFormat::Bgra && format != dc::vcam::PixelFormat::Nv12))
            return false;
        fn(format, dc::vcam::bridge_slot_pixels(header_, slot), int(w), int(h));
        return s.seq.load(std::memory_order_acquire) == seq;
    }

    // Waits for the producer's next frame. False on timeout (or when there is no event).
    bool wait_frame(DWORD timeout_ms) {
        return frame_event_ && WaitForSingleObject(frame_event_, timeout_ms) == WAIT_OBJECT_0;
    }

    void note_delivered() {
        if (!header_) return;
        header_->consumer_frames.fetch_add(1);
        header_->consumer_heartbeat_qpc.store(qpc_now());
    }

private:
    HANDLE map_ = nullptr;
    HANDLE frame_event_ = nullptr;
    dc::vcam::BridgeHeader* header_ = nullptr;
};

// ---------------------------------------------------------------------------------------------

HRESULT make_video_type(const GUID& subtype, UINT32 w, UINT32 h, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    RETURN_IF_FAILED_LOG(MFCreateMediaType(&t));
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    const bool nv12 = subtype == MFVideoFormat_NV12;
    const UINT32 stride = nv12 ? w : w * 4;
    const UINT32 bytes = nv12 ? w * h * 3 / 2 : w * h * 4;
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, bytes);
    t->SetUINT32(MF_MT_AVG_BITRATE, bytes * 8 * kFps);
    *out = t.Detach();
    return S_OK;
}

class VCamSource;

// ---------------------------------------------------------------------------------------------

class VCamStream
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                          IKsControl> {
public:
    HRESULT Initialize(IMFMediaSource* parent, BridgeReader* bridge) {
        parent_ = parent;
        bridge_ = bridge;
        RETURN_IF_FAILED_LOG(MFCreateEventQueue(&queue_));
        RETURN_IF_FAILED_LOG(MFCreateAttributes(&attributes_, 8));
        attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
        attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

        const struct { GUID sub; UINT32 w, h; } kTypes[] = {
            {MFVideoFormat_NV12, 1920, 1080},
            {MFVideoFormat_NV12, 1280, 720},
            {MFVideoFormat_RGB32, 1920, 1080},
            {MFVideoFormat_RGB32, 1280, 720},
        };
        std::vector<ComPtr<IMFMediaType>> types(std::size(kTypes));
        std::vector<IMFMediaType*> raw;
        for (size_t i = 0; i < std::size(kTypes); ++i) {
            RETURN_IF_FAILED_LOG(make_video_type(kTypes[i].sub, kTypes[i].w, kTypes[i].h, &types[i]));
            raw.push_back(types[i].Get());
        }
        RETURN_IF_FAILED_LOG(MFCreateStreamDescriptor(kStreamId, DWORD(raw.size()), raw.data(), &descriptor_));
        ComPtr<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED_LOG(descriptor_->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED_LOG(handler->SetCurrentMediaType(raw[0]));
        RETURN_IF_FAILED_LOG(attributes_->CopyAllItems(descriptor_.Get()));
        return S_OK;
    }

    IMFAttributes* attributes() const { return attributes_.Get(); }

    HRESULT set_provided_allocator(IUnknown* a) {
        std::lock_guard lock(mutex_);
        provided_allocator_.Reset();
        return a ? a->QueryInterface(IID_PPV_ARGS(&provided_allocator_)) : S_OK;
    }
    IMFStreamDescriptor* descriptor() const { return descriptor_.Get(); }

    HRESULT Start(IMFMediaType* type, const PROPVARIANT* start_pos) {
        std::unique_lock lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stop_worker(lock);
        type->GetGUID(MF_MT_SUBTYPE, &subtype_);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        width_ = int(w), height_ = int(h);
        // Prefer the allocator the Frame Server hands over (IMFSampleAllocatorControl), as the
        // reference implementation does; otherwise use our own system-memory one.
        allocator_.Reset();
        if (provided_allocator_) {
            allocator_ = provided_allocator_;
            if (allocator_) allocator_->UninitializeSampleAllocator();
        } else {
            RETURN_IF_FAILED_LOG(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
        }
        RETURN_IF_FAILED_LOG(allocator_->InitializeSampleAllocator(10, type));
        LOG("stream start: %s %dx%d (%s allocator)", subtype_ == MFVideoFormat_NV12 ? "NV12" : "RGB32", width_, height_,
            provided_allocator_ ? "provided" : "own");
        state_ = MF_STREAM_STATE_RUNNING;
        pending_tokens_.clear();
        pending_ = 0;
        running_ = true;
        worker_ = std::thread([this] { worker(); });
        RETURN_IF_FAILED_LOG(queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, start_pos));
        return S_OK;
    }

    HRESULT Stop() {
        std::unique_lock lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stop_worker(lock);
        state_ = MF_STREAM_STATE_STOPPED;
        LOG("stream stop (delivered %llu frames, %llu from producer)", delivered_, delivered_producer_);
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void Shutdown() {
        std::unique_lock lock(mutex_);
        if (shutdown_) return;
        stop_worker(lock);
        shutdown_ = true;
        if (queue_) queue_->Shutdown();
        allocator_.Reset();
        parent_.Reset();
    }

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(cb, state);
    }
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(r, e);
    }
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        ComPtr<IMFMediaEventQueue> q;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return MF_E_SHUTDOWN;
            q = queue_;
        }
        return q->GetEvent(flags, e);
    }
    IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT st, const PROPVARIANT* v) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(t, ext, st, v);
    }

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return parent_.CopyTo(out);
    }
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return descriptor_.CopyTo(out);
    }
    IFACEMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        pending_tokens_.emplace_back(token);
        ++pending_;
        cv_.notify_all();
        return S_OK;
    }

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE value) override {
        LOG("SetStreamState %d", int(value));
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return MF_E_SHUTDOWN;
            if (value == state_) return S_OK;
        }
        if (value == MF_STREAM_STATE_RUNNING) {
            ComPtr<IMFMediaTypeHandler> h;
            ComPtr<IMFMediaType> t;
            RETURN_IF_FAILED_LOG(descriptor_->GetMediaTypeHandler(&h));
            RETURN_IF_FAILED_LOG(h->GetCurrentMediaType(&t));
            return Start(t.Get(), nullptr);
        }
        if (value == MF_STREAM_STATE_STOPPED) return Stop();
        std::lock_guard lock(mutex_);
        state_ = value;  // PAUSED: keep the worker, it only produces on request anyway
        return S_OK;
    }
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* value) override {
        if (!value) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *value = state_;
        return S_OK;
    }

    // IKsControl: no custom properties
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

private:
    void stop_worker(std::unique_lock<std::mutex>& lock) {
        if (!worker_.joinable()) return;
        running_ = false;
        cv_.notify_all();
        lock.unlock();
        worker_.join();
        lock.lock();
    }

    // Delivers one sample per frame while requests are pending, like a real camera. With a producer
    // attached a sample goes out as soon as the producer publishes a frame (so two 30 fps clocks
    // do not beat into duplicated / skipped frames); without one, or if the producer is late by
    // more than two periods, a free-running 30 fps clock takes over.
    void worker() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const int64_t period = qpc_freq() / kFps;
        int64_t next = qpc_now();
        uint64_t tick = 0;
        for (;;) {
            ComPtr<IUnknown> token;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return !running_ || pending_ > 0; });
                if (!running_) break;
            }
            if (bridge_->producer_alive()) {
                bridge_->wait_frame(DWORD(2000 / kFps));
                next = qpc_now() + period;
            } else {
                const int64_t wait = next - qpc_now();
                if (wait > 0) Sleep(DWORD(wait * 1000 / qpc_freq()));
                next = std::max(next + period, qpc_now() - period);
            }
            std::unique_lock lock(mutex_);
            if (!running_) break;
            if (pending_ == 0) continue;
            --pending_;
            token = pending_tokens_.front();
            pending_tokens_.pop_front();
            ComPtr<IMFSample> sample;
            const HRESULT hr = make_sample(tick++, &sample);
            if (FAILED(hr)) {
                LOG("make_sample failed 0x%08lx", hr);
                queue_->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
                continue;
            }
            if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());
            queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
        }
        CoUninitialize();
    }

    HRESULT make_sample(uint64_t tick, IMFSample** out) {
        ComPtr<IMFSample> sample;
        RETURN_IF_FAILED_LOG(allocator_->AllocateSample(&sample));
        ComPtr<IMFMediaBuffer> buf;
        RETURN_IF_FAILED_LOG(sample->GetBufferByIndex(0, &buf));
        ComPtr<IMF2DBuffer2> buf2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* start = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf.As(&buf2d))) {
            RETURN_IF_FAILED_LOG(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len));
        } else {
            RETURN_IF_FAILED_LOG(buf->Lock(&scan0, nullptr, &len));
            pitch = subtype_ == MFVideoFormat_NV12 ? width_ : width_ * 4;
        }
        const bool nv12 = subtype_ == MFVideoFormat_NV12;
        bool from_producer = false;
        bridge_->open();  // no-op when already open; retries a section that failed at start
        auto put_bgra = [&](const uint8_t* px, int w, int h, int spitch) {
            if (nv12)
                dc::vcam::bgra_to_nv12(px, w, h, spitch, scan0, pitch, scan0 + ptrdiff_t(pitch) * height_, pitch, width_,
                                       height_);
            else
                dc::vcam::bgra_scale(px, w, h, spitch, scan0, pitch, width_, height_);
        };
        auto put = [&](dc::vcam::PixelFormat format, const uint8_t* px, int w, int h) {
            if (format == dc::vcam::PixelFormat::Bgra) {
                put_bgra(px, w, h, w * 4);
                return;
            }
            const uint8_t* uv = px + ptrdiff_t(w) * h;
            if (nv12)  // capd's normal case: a row copy at the size the app asked for
                dc::vcam::nv12_scale(px, w, uv, w, w, h, scan0, pitch, scan0 + ptrdiff_t(pitch) * height_, pitch, width_,
                                     height_);
            else
                dc::vcam::nv12_to_bgra(px, w, uv, w, w, h, scan0, pitch, width_, height_);
        };
        if (bridge_->producer_alive()) {
            bridge_->with_latest(put);
            from_producer = true;
        } else {
            nosignal_.resize(size_t(width_) * height_ * 4);
            dc::vcam::draw_no_signal_bgra(nosignal_.data(), width_, height_, width_ * 4, tick);
            put_bgra(nosignal_.data(), width_, height_, width_ * 4);
        }
        if (buf2d) buf2d->Unlock2D();
        else buf->Unlock();
        buf->SetCurrentLength(nv12 ? DWORD(width_ * height_ * 3 / 2) : DWORD(width_ * height_ * 4));
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10'000'000 / kFps);
        ++delivered_;
        if (from_producer) {
            ++delivered_producer_;
            bridge_->note_delivered();
        }
        if (delivered_ % 300 == 1)
            LOG("delivered %llu frames (%llu from producer, producer %s)", delivered_, delivered_producer_,
                from_producer ? "attached" : "absent");
        *out = sample.Detach();
        return S_OK;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;
    bool running_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    ComPtr<IMFMediaSource> parent_;
    BridgeReader* bridge_ = nullptr;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IMFVideoSampleAllocatorEx> provided_allocator_;
    std::deque<ComPtr<IUnknown>> pending_tokens_;
    size_t pending_ = 0;
    std::thread worker_;
    GUID subtype_ = MFVideoFormat_NV12;
    int width_ = 0, height_ = 0;
    uint64_t delivered_ = 0, delivered_producer_ = 0;
    std::vector<uint8_t> nosignal_;
};

// ---------------------------------------------------------------------------------------------

class VCamSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
                          IKsControl, IMFSampleAllocatorControl> {
public:
    HRESULT Initialize(IMFAttributes* activate_attributes) {
        LOG("source initialize");
        bridge_.open();
        RETURN_IF_FAILED_LOG(MFCreateEventQueue(&queue_));
        RETURN_IF_FAILED_LOG(MFCreateAttributes(&attributes_, 4));
        if (activate_attributes) activate_attributes->CopyAllItems(attributes_.Get());

        // Legacy sensor profile, so apps that filter by camera profile still find the stream.
        ComPtr<IMFSensorProfileCollection> profiles;
        ComPtr<IMFSensorProfile> legacy;
        if (SUCCEEDED(MFCreateSensorProfileCollection(&profiles)) &&
            SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy)) &&
            SUCCEEDED(legacy->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))")) &&
            SUCCEEDED(profiles->AddProfile(legacy.Get())))
            attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());

        stream_ = Make<VCamStream>();
        if (!stream_) return E_OUTOFMEMORY;
        RETURN_IF_FAILED_LOG(stream_->Initialize(this, &bridge_));
        IMFStreamDescriptor* sd = stream_->descriptor();
        RETURN_IF_FAILED_LOG(MFCreatePresentationDescriptor(1, &sd, &pd_));
        RETURN_IF_FAILED_LOG(pd_->SelectStream(0));
        return S_OK;
    }

    ~VCamSource() { LOG("source destroyed"); }

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(cb, state);
    }
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(r, e);
    }
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        ComPtr<IMFMediaEventQueue> q;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return MF_E_SHUTDOWN;
            q = queue_;
        }
        return q->GetEvent(flags, e);
    }
    IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT st, const PROPVARIANT* v) override {
        std::lock_guard lock(mutex_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(t, ext, st, v);
    }

    // IMFMediaSource
    IFACEMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(out);
    }
    IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* start_pos) override {
        LOG("source Start");
        if (!pd) return E_INVALIDARG;
        if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        RETURN_IF_FAILED_LOG(pd->GetStreamDescriptorByIndex(0, &selected, &sd));
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        RETURN_IF_FAILED_LOG(sd->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED_LOG(handler->GetCurrentMediaType(&type));
        if (selected) {
            RETURN_IF_FAILED_LOG(queue_->QueueEventParamUnk(started_once_ ? MEUpdatedStream : MENewStream, GUID_NULL,
                                                            S_OK, static_cast<IMFMediaStream*>(stream_.Get())));
            started_once_ = true;
            RETURN_IF_FAILED_LOG(stream_->Start(type.Get(), start_pos));
        }
        PROPVARIANT now;
        PropVariantInit(&now);
        now.vt = VT_I8;
        now.hVal.QuadPart = MFGetSystemTime();
        return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK,
                                          (start_pos && start_pos->vt != VT_EMPTY) ? start_pos : &now);
    }
    IFACEMETHODIMP Stop() override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stream_->Stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    IFACEMETHODIMP Shutdown() override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        LOG("source shutdown");
        shutdown_ = true;
        if (stream_) stream_->Shutdown();
        queue_->Shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return attributes_.CopyTo(out);
    }
    IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
        *out = stream_->attributes();
        (*out)->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP SetD3DManager(IUnknown* m) override {  // system-memory samples only
        LOG("SetD3DManager %p (ignored)", static_cast<void*>(m));
        return S_OK;
    }

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID sid, REFIID iid, LPVOID*) override {
        LOG("GetService %s %s -> unsupported", guid_str(sid).c_str(), guid_str(iid).c_str());
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IMFSampleAllocatorControl
    IFACEMETHODIMP SetDefaultAllocator(DWORD id, IUnknown* allocator) override {
        LOG("SetDefaultAllocator stream %lu %p", id, static_cast<void*>(allocator));
        if (id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return stream_->set_provided_allocator(allocator);
    }
    IFACEMETHODIMP GetAllocatorUsage(DWORD id, DWORD* input_id, MFSampleAllocatorUsage* usage) override {
        if (!input_id || !usage) return E_POINTER;
        if (id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
        *input_id = id;
        *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
        return S_OK;
    }

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

private:
    std::mutex mutex_;
    bool shutdown_ = false;
    bool started_once_ = false;
    BridgeReader bridge_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<VCamStream> stream_;
};

// ---------------------------------------------------------------------------------------------
// IMFActivate handed to the Frame Server; IMFAttributes calls go to an inner attribute store.

class VCamActivate
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    VCamActivate() {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        MFCreateAttributes(&attr_, 4);
        // Same activate attributes as the reference implementation (VCamSample).
        attr_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);
        attr_->SetGUID(MFT_TRANSFORM_CLSID_Attribute, dc::vcam::kMfClsid);
        LOG("activate created in %s", process_name(GetCurrentProcessId()).c_str());
    }
    ~VCamActivate() {
        attr_.Reset();
        MFShutdown();
    }

    IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        UINT32 pid = 0;
        attr_->GetUINT32(kFrameServerClientPid, &pid);
        LOG("ActivateObject %s, client pid %u %s", guid_str(riid).c_str(), pid, pid ? process_name(pid).c_str() : "");
        if (!ppv) return E_POINTER;
        if (!source_) {
            auto s = Make<VCamSource>();
            if (!s) return E_OUTOFMEMORY;
            RETURN_IF_FAILED_LOG(s->Initialize(attr_.Get()));
            source_ = s;
        }
        const HRESULT hr = source_.CopyTo(riid, ppv);
        if (FAILED(hr)) LOG("ActivateObject: QueryInterface failed 0x%08lx", hr);
        return hr;
    }
    IFACEMETHODIMP ShutdownObject() override {
        if (source_) source_->Shutdown();
        source_.Reset();
        return S_OK;
    }
    IFACEMETHODIMP DetachObject() override {
        source_.Reset();
        return S_OK;
    }

    // IMFAttributes forwarding
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attr_->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attr_->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attr_->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attr_->Compare(a, m, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attr_->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attr_->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attr_->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attr_->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attr_->GetStringLength(k, n); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 cch, UINT32* n) override { return attr_->GetString(k, s, cch, n); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override { return attr_->GetAllocatedString(k, s, n); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attr_->GetBlobSize(k, n); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 cb, UINT32* n) override { return attr_->GetBlob(k, b, cb, n); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attr_->GetAllocatedBlob(k, b, n); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return attr_->GetUnknown(k, r, p); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attr_->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return attr_->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return attr_->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attr_->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attr_->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attr_->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attr_->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attr_->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attr_->SetBlob(k, b, n); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attr_->SetUnknown(k, u); }
    IFACEMETHODIMP LockStore() override { return attr_->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return attr_->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* n) override { return attr_->GetCount(n); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attr_->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* d) override { return attr_->CopyAllItems(d); }

private:
    ComPtr<IMFAttributes> attr_;
    ComPtr<VCamSource> source_;
};

class ClassFactory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto a = Make<VCamActivate>();
        if (!a) return E_OUTOFMEMORY;
        return a.CopyTo(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

HMODULE g_module = nullptr;

}  // namespace

// ---------------------------------------------------------------------------------------------
// DLL exports

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != dc::vcam::kMfClsid) return CLASS_E_CLASSNOTAVAILABLE;
    LOG("DllGetClassObject");
    auto f = Make<ClassFactory>();
    if (!f) return E_OUTOFMEMORY;
    return f.CopyTo(riid, ppv);
}

// The Frame Server keeps the DLL for the life of the camera anyway; never unload.
STDAPI DllCanUnloadNow() { return S_FALSE; }

// Registers under HKLM so the Frame Server (LOCAL SERVICE) can see the class. Needs elevation.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + DC_VCAM_MF_CLSID_WSTR;
    HKEY k = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (key + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE,
                                 nullptr, &k, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path), DWORD((wcslen(path) + 1) * 2));
    const wchar_t model[] = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), sizeof(model));
    RegCloseKey(k);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        const wchar_t name[] = DC_VCAM_MF_NAME;
        RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(name), sizeof(name));
        RegCloseKey(k);
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + DC_VCAM_MF_CLSID_WSTR;
    const LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
