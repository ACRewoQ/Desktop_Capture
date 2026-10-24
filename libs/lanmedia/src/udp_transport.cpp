#include "dc/lanmedia/udp_transport.hpp"

#include "dc/shmaudio/clock.hpp"

namespace dc::lan {

double nowSeconds() { return double(shm::nowQpc()) / double(shm::qpcFrequency()); }

UdpTransport::UdpTransport(Options options) : opts_(std::move(options)), socket_(io_) {
    const Endpoint local(asio::ip::make_address(opts_.bindAddress), opts_.port);
    socket_.open(local.protocol());
    socket_.set_option(asio::socket_base::reuse_address(false));
    asio::error_code ec;
    socket_.set_option(asio::socket_base::receive_buffer_size(opts_.receiveBufferBytes), ec);
    socket_.set_option(asio::socket_base::send_buffer_size(opts_.sendBufferBytes), ec);
    socket_.bind(local);
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" (receiver not running yet) makes the next receive on
    // this socket fail with WSAECONNRESET.
    {
        BOOL off = FALSE;
        DWORD bytes = 0;
        constexpr DWORD kSioUdpConnreset = 0x9800000C; // SIO_UDP_CONNRESET
        WSAIoctl(socket_.native_handle(), kSioUdpConnreset, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    }
#endif
}

UdpTransport::~UdpTransport() { stop(); }

void UdpTransport::start(Handler handler) {
    if (running_.exchange(true)) return;
    handler_ = std::move(handler);
    receiveNext();
    thread_ = std::thread([this] { io_.run(); });
}

void UdpTransport::stop() {
    if (!running_.exchange(false)) return;
    asio::post(io_, [this] {
        asio::error_code ec;
        socket_.close(ec);
    });
    if (thread_.joinable()) thread_.join();
}

void UdpTransport::receiveNext() {
    socket_.async_receive_from(asio::buffer(buf_), from_, [this](const asio::error_code& ec, std::size_t n) {
        const double t = nowSeconds();
        if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor || !socket_.is_open()) return;
        // other errors (a truncated datagram, a reset reported by the OS): keep listening
        if (!ec && n <= kMaxDatagramBytes) {
            received_.fetch_add(1, std::memory_order_relaxed);
            if (handler_) handler_(std::span<const std::uint8_t>(buf_.data(), n), from_, t);
        }
        receiveNext();
    });
}

bool UdpTransport::send(std::span<const std::uint8_t> datagram, const Endpoint& to) {
    asio::error_code ec;
    socket_.send_to(asio::buffer(datagram.data(), datagram.size()), to, 0, ec);
    if (ec) {
        sendErrors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

std::uint16_t UdpTransport::localPort() const {
    asio::error_code ec;
    return socket_.local_endpoint(ec).port();
}

UdpTransport::Endpoint UdpTransport::resolve(const std::string& host, std::uint16_t port) {
    asio::io_context io;
    asio::ip::udp::resolver r(io);
    const auto results = r.resolve(asio::ip::udp::v4(), host, std::to_string(port));
    return results.begin()->endpoint();
}

} // namespace dc::lan
