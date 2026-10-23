// WASAPI loopback capture of one process (tree) or of the default render device.
//
// Process loopback uses ActivateAudioInterfaceAsync on VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK with
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK (Windows 10 build 20348 or later according to the SDK docs).
// Device loopback is the classic AUDCLNT_STREAMFLAGS_LOOPBACK on the render endpoint and is the Windows 10
// fallback in the plan. Both deliver 48 kHz float stereo through AUTOCONVERTPCM.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct IAudioClient;
struct IAudioCaptureClient;

namespace v1 {

enum class LoopbackMode { IncludeProcessTree, ExcludeProcessTree, DefaultDevice };

const char* modeName(LoopbackMode m);

struct CaptureStats {
    std::uint64_t frames = 0;         // frames delivered to the sink
    std::uint64_t packets = 0;        // GetBuffer calls that returned data
    std::uint64_t silentPackets = 0;  // packets flagged AUDCLNT_BUFFERFLAGS_SILENT
    std::uint64_t discontinuities = 0;
    double maxGapMs = 0;              // largest hole between consecutive packets (QPC vs. frames)
    double maxPacketMs = 0;           // largest single packet
    HRESULT lastError = S_OK;         // first failure inside the capture thread
};

class LoopbackCapture {
public:
    static constexpr std::uint32_t kRate = 48000;
    static constexpr std::uint32_t kChannels = 2;

    // Interleaved float stereo, `frames` frames, qpcHns = QPC (100 ns units) of the first frame.
    using Sink = std::function<void(const float* data, std::uint32_t frames, std::uint64_t qpcHns)>;

    LoopbackCapture() = default;
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    ~LoopbackCapture();

    // Activates and initializes the client. pid is ignored for DefaultDevice.
    // Returns S_OK or the failing HRESULT; `where` names the failing step.
    HRESULT open(LoopbackMode mode, DWORD pid, std::string* where);
    HRESULT start(Sink sink);
    void stop();

    double activationMs() const { return activationMs_; }
    bool usedPcm16() const { return pcm16_; }
    CaptureStats stats() const;

private:
    void run();

    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE event_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    Sink sink_;
    bool pcm16_ = false;
    double activationMs_ = 0;
    mutable std::mutex statsMutex_;
    CaptureStats stats_;
};

}  // namespace v1
