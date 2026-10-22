#include "mf_codec.hpp"

#include <codecapi.h>
#include <d3d11.h>
#include <mferror.h>
#include <strmif.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace v6 {

namespace {

constexpr std::int64_t kHns = 10'000'000;  // 100 ns units per second

struct ActivateList {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~ActivateList() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};

std::string friendlyName(IMFActivate* act) {
    wchar_t* name = nullptr;
    UINT32 len = 0;
    std::string out = "(unnamed MFT)";
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
        out = toUtf8(name);
        CoTaskMemFree(name);
    }
    return out;
}

void setCodecValue(ICodecAPI* api, const GUID& key, UINT32 value, const char* what) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    const HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) std::cerr << "  note: " << what << " not accepted (" << hrText(hr) << ")\n";
}

void setCodecBool(ICodecAPI* api, const GUID& key, bool value, const char* what) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) std::cerr << "  note: " << what << " not accepted (" << hrText(hr) << ")\n";
}

ComPtr<IMFSample> makeSample(const std::uint8_t* data, size_t size, std::int64_t time, std::int64_t duration) {
    ComPtr<IMFMediaBuffer> buf;
    V6_CHECK(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf));
    BYTE* p = nullptr;
    V6_CHECK(buf->Lock(&p, nullptr, nullptr));
    std::memcpy(p, data, size);
    buf->Unlock();
    V6_CHECK(buf->SetCurrentLength(static_cast<DWORD>(size)));
    ComPtr<IMFSample> s;
    V6_CHECK(MFCreateSample(&s));
    V6_CHECK(s->AddBuffer(buf.Get()));
    V6_CHECK(s->SetSampleTime(time));
    V6_CHECK(s->SetSampleDuration(duration));
    return s;
}

ComPtr<IMFSample> makeEmptySample(DWORD size) {
    ComPtr<IMFMediaBuffer> buf;
    V6_CHECK(MFCreateMemoryBuffer(size, &buf));
    ComPtr<IMFSample> s;
    V6_CHECK(MFCreateSample(&s));
    V6_CHECK(s->AddBuffer(buf.Get()));
    return s;
}

// True if the Annex B stream contains an SPS NAL unit (type 7).
bool hasSps(const std::vector<std::uint8_t>& d) {
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == 7) return true;
    }
    return false;
}

std::uint64_t idFromTime(std::int64_t time, std::int64_t duration) {
    return static_cast<std::uint64_t>((time + duration / 2) / duration);
}

}  // namespace

MfRuntime::MfRuntime() {
    V6_CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    V6_CHECK(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
}

MfRuntime::~MfRuntime() {
    MFShutdown();
    CoUninitialize();
}

const char* toString(CodecKind k) {
    switch (k) {
    case CodecKind::Auto: return "auto";
    case CodecKind::Hardware: return "hw";
    case CodecKind::Software: return "sw";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------------------
// Encoder

H264Encoder::H264Encoder(int width, int height, int fps, int kbps, CodecKind kind, OutputFn onOutput)
    : w_(width), h_(height), fps_(fps), kbps_(kbps), onOutput_(std::move(onOutput)), frameDuration_(kHns / fps) {
    bool ok = false;
    if (kind != CodecKind::Software) ok = tryCreate(true);
    if (!ok && kind == CodecKind::Hardware) throw std::runtime_error("no usable hardware H.264 encoder MFT");
    if (!ok) ok = tryCreate(false);
    if (!ok) throw std::runtime_error("no usable H.264 encoder MFT");
    if (async_) thread_ = std::thread([this] { asyncLoop(); });
}

H264Encoder::~H264Encoder() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
    }
}

bool H264Encoder::tryCreate(bool hardwareOnly) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
    const UINT32 flags = hardwareOnly ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
                                      : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER);
    ActivateList list;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &list.items, &list.count))) return false;
    for (UINT32 i = 0; i < list.count; ++i) {
        const std::string name = friendlyName(list.items[i]);
        try {
            mft_.Reset();
            events_.Reset();
            V6_CHECK(list.items[i]->ActivateObject(IID_PPV_ARGS(&mft_)));
            name_ = name;
            hardware_ = hardwareOnly;
            configure();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "  encoder '" << name << "' skipped: " << e.what() << "\n";
            if (mft_) {
                ComPtr<IMFShutdown> sd;
                if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
            }
            list.items[i]->ShutdownObject();
            mft_.Reset();
        }
    }
    return false;
}

void H264Encoder::configure() {
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
        async_ = MFGetAttributeUINT32(attrs.Get(), MF_TRANSFORM_ASYNC, FALSE) != FALSE;
        if (async_) {
            V6_CHECK(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
            V6_CHECK(mft_.As(&events_));
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(mft_.As(&api))) {
        setCodecBool(api.Get(), CODECAPI_AVLowLatencyMode, true, "AVLowLatencyMode");
        setCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "CBR");
        setCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(kbps_) * 1000, "bitrate");
        setCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(fps_), "GOP size");
        setCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "B-frame count");
    }

    ComPtr<IMFMediaType> outType;
    V6_CHECK(MFCreateMediaType(&outType));
    V6_CHECK(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    V6_CHECK(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    V6_CHECK(outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(kbps_) * 1000));
    V6_CHECK(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w_, h_));
    V6_CHECK(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps_, 1));
    V6_CHECK(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    V6_CHECK(outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    V6_CHECK(outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
    V6_CHECK(mft_->SetOutputType(0, outType.Get(), 0));

    ComPtr<IMFMediaType> inType;
    V6_CHECK(MFCreateMediaType(&inType));
    V6_CHECK(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    V6_CHECK(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    V6_CHECK(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, w_, h_));
    V6_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps_, 1));
    V6_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    V6_CHECK(inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    V6_CHECK(mft_->SetInputType(0, inType.Get(), 0));

    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, &cur))) {
        UINT32 size = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            seqHeader_.resize(size);
            cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader_.data(), size, nullptr);
        }
    }

    V6_CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    V6_CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

ComPtr<IMFSample> H264Encoder::makeInput(const std::uint8_t* nv12, std::uint64_t id) {
    const size_t size = static_cast<size_t>(w_) * h_ * 3 / 2;
    return makeSample(nv12, size, static_cast<std::int64_t>(id) * frameDuration_, frameDuration_);
}

void H264Encoder::encode(const std::uint8_t* nv12, std::uint64_t id) {
    auto sample = makeInput(nv12, id);
    if (async_) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_.push_back(std::move(sample));
            while (pending_.size() > 4) pending_.pop_front();  // encoder fell behind; drop the oldest
        }
        cv_.notify_one();
        return;
    }
    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        drainOutput();
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    V6_CHECK(hr);
    drainOutput();
}

// Pulls every available output (sync MFT) or exactly one (async MFT, called per METransformHaveOutput).
void H264Encoder::drainOutput() {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO si{};
        V6_CHECK(mft_->GetOutputStreamInfo(0, &si));
        ComPtr<IMFSample> own;
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            own = makeEmptySample(std::max<DWORD>(si.cbSize, static_cast<DWORD>(w_ * h_)));
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        ComPtr<IMFSample> out = own;
        if (!own && ob.pSample) out.Attach(ob.pSample);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            V6_CHECK(mft_->GetOutputAvailableType(0, 0, &t));
            V6_CHECK(mft_->SetOutputType(0, t.Get(), 0));
            continue;
        }
        V6_CHECK(hr);
        if (!out) return;

        EncodedUnit unit;
        LONGLONG time = 0;
        out->GetSampleTime(&time);
        unit.id = idFromTime(time, frameDuration_);
        unit.keyframe = MFGetAttributeUINT32(out.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
        ComPtr<IMFMediaBuffer> buf;
        V6_CHECK(out->ConvertToContiguousBuffer(&buf));
        BYTE* p = nullptr;
        DWORD len = 0;
        V6_CHECK(buf->Lock(&p, nullptr, &len));
        unit.data.assign(p, p + len);
        buf->Unlock();

        if (unit.keyframe && !hasSps(unit.data)) {
            if (seqHeader_.empty()) {
                ComPtr<IMFMediaType> cur;
                UINT32 size = 0;
                if (SUCCEEDED(mft_->GetOutputCurrentType(0, &cur)) &&
                    SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
                    seqHeader_.resize(size);
                    cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader_.data(), size, nullptr);
                }
            }
            unit.data.insert(unit.data.begin(), seqHeader_.begin(), seqHeader_.end());
        }
        onOutput_(std::move(unit));
        if (async_) return;
    }
}

void H264Encoder::asyncLoop() {
    int needInput = 0;
    try {
        while (!stop_) {
            ComPtr<IMFMediaEvent> ev;
            const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                std::unique_lock<std::mutex> lock(mu_);
                if (needInput > 0 && !pending_.empty()) {
                    auto s = std::move(pending_.front());
                    pending_.pop_front();
                    lock.unlock();
                    V6_CHECK(mft_->ProcessInput(0, s.Get(), 0));
                    --needInput;
                    continue;
                }
                // Events do not signal the condition variable, so wake at least every millisecond.
                cv_.wait_for(lock, std::chrono::milliseconds(1));
                continue;
            }
            V6_CHECK(hr);
            MediaEventType type = MEUnknown;
            V6_CHECK(ev->GetType(&type));
            if (type == METransformNeedInput) {
                ++needInput;
            } else if (type == METransformHaveOutput) {
                drainOutput();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "encoder thread stopped: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Decoder

H264Decoder::H264Decoder(ID3D11Device* device, int width, int height, int fps) : frameDuration_(kHns / fps) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    ActivateList list;
    V6_CHECK(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                       &list.items, &list.count));
    if (list.count == 0) throw std::runtime_error("no H.264 decoder MFT found");
    V6_CHECK(list.items[0]->ActivateObject(IID_PPV_ARGS(&mft_)));
    name_ = friendlyName(list.items[0]);

    ComPtr<IMFAttributes> attrs;
    V6_CHECK(mft_->GetAttributes(&attrs));
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(mft_.As(&api))) setCodecBool(api.Get(), CODECAPI_AVLowLatencyMode, true, "AVLowLatencyMode");

    if (device) {
        if (!MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE))
            throw std::runtime_error(name_ + " is not D3D11 aware");
        V6_CHECK(MFCreateDXGIDeviceManager(&resetToken_, &manager_));
        V6_CHECK(manager_->ResetDevice(device, resetToken_));
        V6_CHECK(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get())));
        hardware_ = true;
    }

    ComPtr<IMFMediaType> inType;
    V6_CHECK(MFCreateMediaType(&inType));
    V6_CHECK(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    V6_CHECK(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    V6_CHECK(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height));
    V6_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1));
    V6_CHECK(inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    V6_CHECK(mft_->SetInputType(0, inType.Get(), 0));
    setOutputType();

    V6_CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    V6_CHECK(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

H264Decoder::~H264Decoder() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (manager_) mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
}

void H264Decoder::setOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = mft_->GetOutputAvailableType(0, i, &t);
        if (hr == MF_E_NO_MORE_TYPES) throw std::runtime_error("decoder offers no NV12 output");
        V6_CHECK(hr);
        GUID sub{};
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
            V6_CHECK(mft_->SetOutputType(0, t.Get(), 0));
            break;
        }
    }
    ComPtr<IMFMediaType> cur;
    V6_CHECK(mft_->GetOutputCurrentType(0, &cur));
    V6_CHECK(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &info_.codedWidth, &info_.codedHeight));
    MFVideoArea area{};
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area,
                               nullptr))) {
        info_.visible.left = area.OffsetX.value;
        info_.visible.top = area.OffsetY.value;
        info_.visible.right = info_.visible.left + area.Area.cx;
        info_.visible.bottom = info_.visible.top + area.Area.cy;
    } else {
        info_.visible = RECT{0, 0, static_cast<LONG>(info_.codedWidth), static_cast<LONG>(info_.codedHeight)};
    }
    UINT32 stride = 0;
    info_.stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride)
                                                                            : static_cast<LONG>(info_.codedWidth);

    MFT_OUTPUT_STREAM_INFO si{};
    V6_CHECK(mft_->GetOutputStreamInfo(0, &si));
    providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferSize_ = si.cbSize;
}

void H264Decoder::decode(const EncodedUnit& unit, const FrameFn& onFrame) {
    auto sample = makeSample(unit.data.data(), unit.data.size(), static_cast<std::int64_t>(unit.id) * frameDuration_,
                             frameDuration_);
    if (unit.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    auto drain = [&] {
        for (;;) {
            ComPtr<IMFSample> own;
            MFT_OUTPUT_DATA_BUFFER ob{};
            if (!providesSamples_) {
                own = makeEmptySample(outputBufferSize_ ? outputBufferSize_
                                                        : info_.codedWidth * info_.codedHeight * 3 / 2);
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            ComPtr<IMFSample> out = own;
            if (!own && ob.pSample) out.Attach(ob.pSample);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                setOutputType();
                continue;
            }
            V6_CHECK(hr);
            if (!out) return;
            LONGLONG time = 0;
            out->GetSampleTime(&time);
            onFrame(out.Get(), idFromTime(time, frameDuration_), info_);
        }
    };

    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        drain();
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    V6_CHECK(hr);
    drain();
}

}  // namespace v6
