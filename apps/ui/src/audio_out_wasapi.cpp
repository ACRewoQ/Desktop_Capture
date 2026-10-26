// AudioOut on Windows: WASAPI shared mode, event driven, on the default output device.

#include "audio_out.hpp"

#include "dc/lanmedia/udp_transport.hpp"

#include <windows.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace dc::ui {

std::unique_ptr<AudioOut> makeNullAudioOut(AudioOut::Render render);

namespace {

constexpr REFERENCE_TIME kBufferHns = 400000;  // 40 ms: room for a late wakeup, filled only to what is due
constexpr DWORD kDefaultCheckMs = 2000;        // how often to look whether the default device changed
constexpr DWORD kRetryMs = 1000;               // after a failure (no device, device lost)

QString hr(const char* what, HRESULT h) {
    return QStringLiteral("%1 0x%2").arg(QString::fromLatin1(what)).arg(static_cast<quint32>(h), 8, 16, QLatin1Char('0'));
}

class WasapiOut final : public AudioOut {
public:
    explicit WasapiOut(Render render) : render_(std::move(render)) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
    }
    ~WasapiOut() override {
        SetEvent(stop_);
        thread_.join();
        CloseHandle(stop_);
    }
    QString deviceName() const override {
        std::lock_guard lock(mutex_);
        return name_;
    }
    QString error() const override {
        std::lock_guard lock(mutex_);
        return error_;
    }

private:
    void setState(const QString& name, const QString& error) {
        std::lock_guard lock(mutex_);
        name_ = name;
        error_ = error;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        lan::raiseThreadPriority();
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT h = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(h)) {
            setState({}, hr("MMDeviceEnumerator", h));
            CoUninitialize();
            return;
        }
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            const QString failure = playDefault(enumerator.Get(), ready);
            if (!failure.isEmpty()) setState({}, failure);
            WaitForSingleObject(stop_, kRetryMs);
        }
        CloseHandle(ready);
        enumerator.Reset();
        CoUninitialize();
    }

    // Plays on the current default device until it fails, the default changes or stop. Returns why it
    // failed ("" = stopped or the default changed).
    QString playDefault(IMMDeviceEnumerator* enumerator, HANDLE ready) {
        ComPtr<IMMDevice> device;
        HRESULT h = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(h)) return QStringLiteral("出力デバイスがありません");
        const std::wstring id = deviceId(device.Get());
        QString name = QStringLiteral("既定の出力");
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                name = QString::fromWCharArray(v.pwszVal);
            PropVariantClear(&v);
        }

        ComPtr<IAudioClient> client;
        h = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(h)) return hr("IAudioClient", h);
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = 2;
        fmt.Format.nSamplesPerSec = 48000;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = 8;
        fmt.Format.nAvgBytesPerSec = 48000 * 8;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        // The OS converts to the device's own format and rate (the remote's device is not ours to set).
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        h = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferHns, 0, &fmt.Format, nullptr);
        if (FAILED(h)) return hr("Initialize", h);
        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        client->SetEventHandle(ready);
        ComPtr<IAudioRenderClient> render;
        h = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(h)) return hr("IAudioRenderClient", h);
        // Ask for at most ~20 ms ahead: the receiver's buffer, not the device's, absorbs the network.
        const UINT32 fillTo = std::min<UINT32>(bufferFrames, 960);
        h = client->Start();
        if (FAILED(h)) return hr("Start", h);
        setState(name, {});

        HANDLE waits[2] = {stop_, ready};
        ULONGLONG lastCheck = GetTickCount64();
        QString failure;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (w == WAIT_OBJECT_0) break;
            UINT32 padding = 0;
            h = client->GetCurrentPadding(&padding);
            if (FAILED(h)) {
                failure = h == AUDCLNT_E_DEVICE_INVALIDATED ? QStringLiteral("出力デバイスが外れました") : hr("GetCurrentPadding", h);
                break;
            }
            if (padding < fillTo) {
                const UINT32 n = fillTo - padding;
                BYTE* data = nullptr;
                h = render->GetBuffer(n, &data);
                if (FAILED(h)) {
                    failure = hr("GetBuffer", h);
                    break;
                }
                render_(reinterpret_cast<float*>(data), n, lan::nowSeconds());
                render->ReleaseBuffer(n, 0);
            }
            if (GetTickCount64() - lastCheck >= kDefaultCheckMs) {
                lastCheck = GetTickCount64();
                ComPtr<IMMDevice> now;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &now)) && deviceId(now.Get()) != id)
                    break;  // the user picked another output: follow it
            }
        }
        client->Stop();
        return failure;
    }

    static std::wstring deviceId(IMMDevice* device) {
        LPWSTR id = nullptr;
        std::wstring s;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            s = id;
            CoTaskMemFree(id);
        }
        return s;
    }

    Render render_;
    HANDLE stop_ = nullptr;
    std::thread thread_;
    mutable std::mutex mutex_;
    QString name_;
    QString error_ = QStringLiteral("出力を開いています…");
};

} // namespace

std::unique_ptr<AudioOut> AudioOut::create(Render render, bool null) {
    if (null) return makeNullAudioOut(std::move(render));
    return std::make_unique<WasapiOut>(std::move(render));
}

} // namespace dc::ui
