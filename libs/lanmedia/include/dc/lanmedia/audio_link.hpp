#pragma once

// An audio stream on its own socket, for a listener that is not an engine (the remote's monitor audio,
// docs/spec/lan-media.md 3): the socket, the AudioReceiver, the receiver reports back to the sender,
// and the knock that opens a reply-only firewall. The engine itself receives LAN audio through its
// shared media socket (mixd's LanService) instead.

#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/udp_transport.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace dc::lan {

class AudioInput {
public:
    struct Options {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = 0; // 0 = any free port: tell the sender with /lan/subscribe
        AudioReceiver::Config receiver{};
    };

    explicit AudioInput(Options options);
    ~AudioInput();
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    // Audio thread: `frames` output frames, now = nowSeconds() at this block (AudioReceiver::pull).
    void pull(float* out, std::size_t frames, double now) { receiver_.pull(out, frames, now); }

    std::uint16_t localPort() const { return transport_.localPort(); }
    AudioReceiver::Stats stats() const { return receiver_.stats(); }
    const AudioReceiver::Config& config() const { return receiver_.config(); }
    std::uint64_t datagrams() const { return transport_.received(); }

    // Where the stream is expected to come from (the engine's UDP 47810). Until the first packet
    // arrives an empty report goes there twice a second, as VideoInput::setExpectedSource does.
    void setExpectedSource(const UdpTransport::Endpoint& sender);

private:
    void reportLoop();

    UdpTransport transport_;
    AudioReceiver receiver_;
    std::mutex mutex_;
    bool haveSource_ = false;
    UdpTransport::Endpoint source_;
    bool haveExpected_ = false;
    UdpTransport::Endpoint expected_;
    std::atomic<bool> running_{true};
    std::thread reports_;
};

} // namespace dc::lan
