#pragma once

#include "dc/lanmedia/packet.hpp"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>

namespace dc::lan {

// Seconds on the clock the transport stamps arrivals with (QPC on Windows), the same clock the audio
// engine uses for its device timestamps.
double nowSeconds();

// One UDP socket for all media streams of a process: receives on its own thread and hands each
// datagram, with its arrival time, to the handler (which dispatches by stream id); sends from any
// thread. IPv4.
class UdpTransport {
public:
    struct Options {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = kDefaultMediaPort; // 0 = pick a free port (tests)
        int receiveBufferBytes = 1 << 20;
        int sendBufferBytes = 1 << 20;
    };

    using Endpoint = asio::ip::udp::endpoint;
    // Called on the receive thread.
    using Handler = std::function<void(std::span<const std::uint8_t> datagram, const Endpoint& from, double arrival)>;

    explicit UdpTransport(Options options);
    ~UdpTransport();
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void start(Handler handler);
    void stop();

    // Any thread. Returns false if the datagram could not be handed to the OS.
    bool send(std::span<const std::uint8_t> datagram, const Endpoint& to);

    std::uint16_t localPort() const;
    static Endpoint resolve(const std::string& host, std::uint16_t port);

    std::uint64_t received() const { return received_.load(std::memory_order_relaxed); }
    std::uint64_t sendErrors() const { return sendErrors_.load(std::memory_order_relaxed); }

private:
    void receiveNext();

    Options opts_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    Handler handler_;
    std::array<std::uint8_t, 2048> buf_{};
    Endpoint from_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> received_{0}, sendErrors_{0};
};

} // namespace dc::lan
