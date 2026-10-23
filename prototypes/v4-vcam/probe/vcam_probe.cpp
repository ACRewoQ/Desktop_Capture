// V4 prototype: camera-app stand-in that measures what a virtual camera delivers.
//
//   vcam-probe list
//   vcam-probe mf --name <substring> [--seconds 10] [--width 1920 --height 1080]
//   vcam-probe ds --name <substring> [--seconds 10]
//
// mf: opens the camera with Media Foundation (IMFSourceReader), as Windows Camera / Chrome do
// ds: opens it with DirectShow (capture graph + Sample Grabber), as Zoom / OBS / older apps do
// For every frame it decodes the stamp (common/pattern.hpp) and reports, on the last line:
//   RESULT <api> frames=N fps=F stamped=S unique=U dropped=D latency_ms avg=A p95=P max=M format=...
#include <windows.h>

#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../common/pattern.hpp"

using Microsoft::WRL::ComPtr;

namespace {

int64_t qpc_now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
double qpc_ms(int64_t d) {
    static const double f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return double(v.QuadPart);
    }();
    return double(d) * 1000.0 / f;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

// Device names are localized (Windows appends e.g. "(Windows 仮想カメラ)" to MF virtual cameras),
// so they are printed as UTF-8; printf("%ls") would stop at the first non-ASCII character.
std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Exact (case-insensitive) name match wins over a substring match.
template <class List>
auto find_device(const List& list, const std::wstring& want) -> decltype(&list.front()) {
    for (auto& d : list)
        if (lower(d.first) == lower(want)) return &d;
    for (auto& d : list)
        if (lower(d.first).find(lower(want)) != std::wstring::npos) return &d;
    return nullptr;
}

// --- statistics -------------------------------------------------------------------------------

struct Stats {
    std::mutex m;
    uint64_t frames = 0, stamped = 0, unique = 0, dropped = 0;
    std::optional<uint32_t> last_index;
    std::vector<double> latency;
    int64_t first = 0, last = 0;

    void add(const std::optional<dc::vcam::Stamp>& s, int64_t now) {
        std::lock_guard lock(m);
        if (!frames) first = now;
        last = now;
        ++frames;
        if (!s) return;
        ++stamped;
        if (last_index && s->frame_index == *last_index) return;  // repeated frame
        if (last_index && s->frame_index > *last_index + 1) dropped += s->frame_index - *last_index - 1;
        last_index = s->frame_index;
        ++unique;
        latency.push_back(qpc_ms(now - s->qpc));
    }

    void print(const char* api, const std::string& format) {
        std::lock_guard lock(m);
        const double secs = frames > 1 ? qpc_ms(last - first) / 1000.0 : 0;
        double avg = 0, p95 = 0, mx = 0;
        if (!latency.empty()) {
            std::vector<double> v = latency;
            std::sort(v.begin(), v.end());
            for (double x : v) avg += x;
            avg /= double(v.size());
            p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
            mx = v.back();
        }
        std::printf("RESULT %s frames=%llu fps=%.1f stamped=%llu unique=%llu dropped=%llu latency_ms avg=%.1f p95=%.1f "
                    "max=%.1f format=%s\n",
                    api, frames, secs > 0 ? double(frames - 1) / secs : 0.0, stamped, unique, dropped, avg, p95, mx,
                    format.c_str());
    }
};

// --- Media Foundation ---------------------------------------------------------------------------

std::vector<std::pair<std::wstring, ComPtr<IMFActivate>>> mf_devices() {
    std::vector<std::pair<std::wstring, ComPtr<IMFActivate>>> out;
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    if (FAILED(MFEnumDeviceSources(a.Get(), &devs, &n))) return out;
    for (UINT32 i = 0; i < n; ++i) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        out.emplace_back(name ? name : L"?", devs[i]);
        CoTaskMemFree(name);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return out;
}

int run_mf(const std::wstring& want, double seconds, UINT32 want_w, UINT32 want_h) {
    ComPtr<IMFActivate> dev;
    const auto devices = mf_devices();
    if (auto d = find_device(devices, want)) {
        dev = d->second;
        std::printf("mf: opening \"%s\"\n", utf8(d->first).c_str());
    }
    if (!dev) {
        std::printf("RESULT mf NOT_FOUND\n");
        return 1;
    }
    ComPtr<IMFMediaSource> src;
    HRESULT hr = dev->ActivateObject(IID_PPV_ARGS(&src));
    if (FAILED(hr)) {
        std::printf("RESULT mf ACTIVATE_FAILED 0x%08lx\n", hr);
        return 1;
    }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    if (FAILED(hr)) {
        std::printf("RESULT mf READER_FAILED 0x%08lx\n", hr);
        return 1;
    }
    // Pick the native NV12 (or RGB32) type of the wanted size, else the first one.
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(stream, i, &t))) break;
        GUID sub{};
        UINT32 w = 0, h = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        std::printf("mf: native type %lu: %s %ux%u\n", i,
                    sub == MFVideoFormat_NV12 ? "NV12" : sub == MFVideoFormat_RGB32 ? "RGB32" : sub == MFVideoFormat_YUY2 ? "YUY2" : "other",
                    w, h);
        if (!chosen && w == want_w && h == want_h && (sub == MFVideoFormat_NV12 || sub == MFVideoFormat_RGB32))
            chosen = t;
    }
    if (!chosen) reader->GetNativeMediaType(stream, 0, &chosen);
    hr = reader->SetCurrentMediaType(stream, nullptr, chosen.Get());
    if (FAILED(hr)) {
        std::printf("RESULT mf SET_TYPE_FAILED 0x%08lx\n", hr);
        return 1;
    }
    GUID sub{};
    UINT32 w = 0, h = 0;
    chosen->GetGUID(MF_MT_SUBTYPE, &sub);
    MFGetAttributeSize(chosen.Get(), MF_MT_FRAME_SIZE, &w, &h);
    const bool nv12 = sub == MFVideoFormat_NV12;
    char fmt[64];
    std::snprintf(fmt, sizeof(fmt), "%s_%ux%u", nv12 ? "NV12" : "RGB32", w, h);

    Stats st;
    const int64_t t0 = qpc_now();
    int errors = 0;
    while (qpc_ms(qpc_now() - t0) < seconds * 1000) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) {
            std::printf("mf: ReadSample 0x%08lx\n", hr);
            if (++errors > 5) break;
            continue;
        }
        if (!sample) continue;
        const int64_t now = qpc_now();
        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* p = nullptr;
        DWORD len = 0;
        std::optional<dc::vcam::Stamp> s;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> b2;
        BYTE* base = nullptr;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&base, &pitch))) {
            p = base;
        } else if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
            pitch = LONG(nv12 ? w : w * 4);
        }
        if (p) {
            if (nv12)
                s = dc::vcam::decode_stamp(int(w), int(h), [&](int x, int y) { return int(p[ptrdiff_t(y) * pitch + x]); });
            else
                s = dc::vcam::decode_stamp(int(w), int(h), [&](int x, int y) { return int(p[ptrdiff_t(y) * pitch + x * 4 + 1]); });
            if (b2) b2->Unlock2D();
            else buf->Unlock();
        }
        st.add(s, now);
    }
    st.print("mf", fmt);
    src->Shutdown();
    return 0;
}

// --- DirectShow ---------------------------------------------------------------------------------
// Sample Grabber and Null Renderer still ship with Windows (qedit.dll) but their header is gone
// from the SDK, so the few declarations needed are repeated here.

const CLSID CLSID_SampleGrabber_ = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const CLSID CLSID_NullRenderer_ = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB_ : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber_ : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB_*, long) = 0;
};

class GrabberCB : public ISampleGrabberCB_ {
public:
    Stats* stats = nullptr;
    int w = 0, h = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB_)) {
            *ppv = static_cast<ISampleGrabberCB_*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* p, long len) override {
        const int64_t now = qpc_now();
        const int stride = (w * 3 + 3) & ~3;
        std::optional<dc::vcam::Stamp> s;
        if (p && len >= stride * h)  // RGB24, bottom-up DIB
            s = dc::vcam::decode_stamp(w, h, [&](int x, int y) {
                const BYTE* px = p + ptrdiff_t(h - 1 - y) * stride + x * 3;
                return (px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8;
            });
        stats->add(s, now);
        return S_OK;
    }
};

std::vector<std::pair<std::wstring, ComPtr<IMoniker>>> ds_devices() {
    std::vector<std::pair<std::wstring, ComPtr<IMoniker>>> out;
    ComPtr<ICreateDevEnum> de;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&de)))) return out;
    ComPtr<IEnumMoniker> em;
    if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) != S_OK) return out;
    ComPtr<IMoniker> m;
    while (em->Next(1, &m, nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        std::wstring name = L"?";
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) name = v.bstrVal;
            VariantClear(&v);
        }
        out.emplace_back(name, m);
        m.Reset();
    }
    return out;
}

int run_ds(const std::wstring& want, double seconds) {
    ComPtr<IMoniker> mon;
    const auto devices = ds_devices();
    if (auto d = find_device(devices, want)) {
        mon = d->second;
        std::printf("ds: opening \"%s\"\n", utf8(d->first).c_str());
    }
    if (!mon) {
        std::printf("RESULT ds NOT_FOUND\n");
        return 1;
    }
    ComPtr<IBaseFilter> cam, grab, null;
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> cgb;
    HRESULT hr = mon->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&cam));
    if (FAILED(hr)) {
        std::printf("RESULT ds BIND_FAILED 0x%08lx\n", hr);
        return 1;
    }
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb));
    CoCreateInstance(CLSID_SampleGrabber_, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grab));
    CoCreateInstance(CLSID_NullRenderer_, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&null));
    if (!graph || !cgb || !grab || !null) {
        std::printf("RESULT ds GRAPH_FAILED (qedit.dll missing?)\n");
        return 1;
    }
    cgb->SetFiltergraph(graph.Get());
    graph->AddFilter(cam.Get(), L"camera");
    graph->AddFilter(grab.Get(), L"grabber");
    graph->AddFilter(null.Get(), L"null");
    ComPtr<ISampleGrabber_> sg;
    grab.As(&sg);
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_RGB24;
    mt.formattype = FORMAT_VideoInfo;
    sg->SetMediaType(&mt);
    hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, cam.Get(), grab.Get(), null.Get());
    if (FAILED(hr)) {
        std::printf("RESULT ds RENDER_FAILED 0x%08lx\n", hr);
        return 1;
    }
    AM_MEDIA_TYPE cmt{};
    sg->GetConnectedMediaType(&cmt);
    int w = 0, h = 0;
    if (cmt.formattype == FORMAT_VideoInfo && cmt.pbFormat) {
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(cmt.pbFormat);
        w = vih->bmiHeader.biWidth;
        h = std::abs(vih->bmiHeader.biHeight);
    }
    if (cmt.pbFormat) CoTaskMemFree(cmt.pbFormat);
    if (cmt.pUnk) cmt.pUnk->Release();
    char fmt[64];
    std::snprintf(fmt, sizeof(fmt), "RGB24_%dx%d", w, h);

    Stats st;
    GrabberCB cb;
    cb.stats = &st;
    cb.w = w;
    cb.h = h;
    sg->SetBufferSamples(FALSE);
    sg->SetCallback(&cb, 1);
    ComPtr<IMediaControl> mc;
    graph.As(&mc);
    hr = mc->Run();
    if (FAILED(hr)) {
        std::printf("RESULT ds RUN_FAILED 0x%08lx\n", hr);
        return 1;
    }
    Sleep(DWORD(seconds * 1000));
    mc->Stop();
    sg->SetCallback(nullptr, 1);
    st.print("ds", fmt);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring name = L"Desktop Capture Camera";
    double seconds = 10;
    UINT32 w = 1920, h = 1080;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--name") name = argv[i + 1];
        else if (k == L"--seconds") seconds = _wtof(argv[i + 1]);
        else if (k == L"--width") w = UINT32(_wtoi(argv[i + 1]));
        else if (k == L"--height") h = UINT32(_wtoi(argv[i + 1]));
    }
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    int rc = 2;
    if (cmd == L"list") {
        std::printf("Media Foundation video capture devices:\n");
        for (auto& d : mf_devices()) std::printf("  mf: %s\n", utf8(d.first).c_str());
        std::printf("DirectShow video input devices:\n");
        for (auto& d : ds_devices()) std::printf("  ds: %s\n", utf8(d.first).c_str());
        rc = 0;
    } else if (cmd == L"mf") {
        rc = run_mf(name, seconds, w, h);
    } else if (cmd == L"ds") {
        rc = run_ds(name, seconds);
    } else {
        std::printf("usage: vcam-probe list | mf [--name N] [--seconds S] [--width W --height H] | ds [--name N] [--seconds S]\n");
    }
    std::fflush(stdout);
    MFShutdown();
    CoUninitialize();
    return rc;
}
