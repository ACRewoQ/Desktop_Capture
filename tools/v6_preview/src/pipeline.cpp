#include "pipeline.hpp"

#include <asio.hpp>
#include <timeapi.h>

#include <chrono>
#include <cstring>
#include <future>
#include <iostream>

#include "test_pattern.hpp"

namespace v6 {

namespace {

// Each worker thread joins the multithreaded apartment; Media Foundation objects are created on it.
struct ComThread {
    bool ok;
    ComThread() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComThread() {
        if (ok) CoUninitialize();
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------

SenderPipeline::SenderPipeline(const PipelineOptions& o, Stats& stats, Sink sink)
    : o_(o), stats_(stats), sink_(std::move(sink)) {}

SenderPipeline::~SenderPipeline() { stop(); }

void SenderPipeline::start() {
    std::promise<void> ready;
    auto fut = ready.get_future();
    thread_ = std::thread([this, &ready] {
        ComThread com;
        try {
            encoder_ = std::make_unique<H264Encoder>(o_.width, o_.height, o_.fps, o_.kbps, o_.encoder,
                                                     [this](EncodedUnit&& u) {
                                                         stats_.mark(u.id, Encoded);
                                                         sink_(std::move(u));
                                                     });
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        ready.set_value();
        run();
        encoder_.reset();
    });
    try {
        fut.get();
    } catch (...) {
        thread_.join();
        throw;
    }
}

void SenderPipeline::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void SenderPipeline::run() {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    TestPattern pattern(o_.width, o_.height);
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / o_.fps);
    auto next = std::chrono::steady_clock::now();
    try {
        for (std::uint64_t id = 0; !stop_; ++id) {
            const std::int64_t t0 = nowUs();
            pattern.render(id, t0);
            stats_.mark(id, Gen, t0);
            stats_.countGenerated();
            encoder_->encode(pattern.nv12().data(), id);
            next += period;
            const auto now = std::chrono::steady_clock::now();
            if (next < now - period * 4) next = now;  // fell far behind: don't burst
            std::this_thread::sleep_until(next);
        }
    } catch (const std::exception& e) {
        std::cerr << "sender stopped: " << e.what() << "\n";
    }
    timeEndPeriod(1);
}

// ---------------------------------------------------------------------------------------------------------------

ReceiverPipeline::ReceiverPipeline(const PipelineOptions& o, Stats& stats, const LUID* adapter,
                                   std::function<void(std::uint64_t)> onPublished)
    : o_(o), stats_(stats), onPublished_(std::move(onPublished)) {
    std::promise<void> ready;
    auto fut = ready.get_future();
    LUID luid{};
    const bool haveLuid = adapter != nullptr;
    if (haveLuid) luid = *adapter;
    thread_ = std::thread([this, &ready, luid, haveLuid] {
        ComThread com;
        try {
            device_ = createVideoDevice(haveLuid ? &luid : nullptr, &adapterName_);
            decoder_ = std::make_unique<H264Decoder>(o_.hwDecode ? device_.Get() : nullptr, o_.width, o_.height,
                                                     o_.fps);
            decoderName_ = decoder_->name() + (decoder_->hardware() ? " [D3D11/DXVA]" : " [software]");
            converter_ = std::make_unique<FrameConverter>(device_.Get(), o_.width, o_.height);
            ring_ = std::make_unique<SharedFrameRing>(device_.Get(), o_.width, o_.height);
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        ready.set_value();
        run();
        decoder_.reset();
    });
    try {
        fut.get();
    } catch (...) {
        thread_.join();
        throw;
    }
}

ReceiverPipeline::~ReceiverPipeline() { stop(); }

void ReceiverPipeline::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string ReceiverPipeline::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void ReceiverPipeline::push(EncodedUnit&& unit) {
    stats_.mark(unit.id, Received);
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(unit));
    }
    cv_.notify_one();
}

void ReceiverPipeline::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    try {
        for (;;) {
            EncodedUnit unit;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                unit = std::move(queue_.front());
                queue_.pop_front();
            }
            decoder_->decode(unit, [&](IMFSample* sample, std::uint64_t id, const VideoInfo& info) {
                stats_.mark(id, Decoded);
                decoded_.fetch_add(1, std::memory_order_relaxed);
                ID3D11Texture2D* target = ring_->beginWrite();
                converter_->convert(sample, info, target);
                ring_->endWrite(id);
                stats_.mark(id, Converted);
                if (onPublished_) onPublished_(id);
            });
        }
    } catch (const std::exception& e) {
        std::cerr << "receiver stopped: " << e.what() << "\n";
        std::lock_guard<std::mutex> lock(mu_);
        error_ = e.what();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// TCP link. Messages: u32 length (LE, excluding itself), u8 type, payload.
//   type 1 access unit: u64 id, u8 keyframe, bytes
//   type 2 ack:         u64 id, i64 decode, i64 convert, i64 copy, i64 present (microseconds)

namespace {

constexpr std::uint8_t kMsgUnit = 1, kMsgAck = 2;

template <class T>
void put(std::vector<std::uint8_t>& b, T v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}

template <class T>
T get(const std::uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    p += sizeof v;
    return v;
}

void writeMessage(asio::ip::tcp::socket& s, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> head;
    put<std::uint32_t>(head, static_cast<std::uint32_t>(payload.size() + 1));
    head.push_back(type);
    std::array<asio::const_buffer, 2> bufs{asio::buffer(head), asio::buffer(payload)};
    asio::write(s, bufs);
}

bool readMessage(asio::ip::tcp::socket& s, std::uint8_t& type, std::vector<std::uint8_t>& payload) {
    std::uint32_t len = 0;
    asio::error_code ec;
    asio::read(s, asio::buffer(&len, 4), ec);
    if (ec || len == 0 || len > (64u << 20)) return false;
    payload.resize(len);
    asio::read(s, asio::buffer(payload), ec);
    if (ec) return false;
    type = payload[0];
    payload.erase(payload.begin());
    return true;
}

}  // namespace

struct TcpSender::Impl {
    asio::io_context io;
    asio::ip::tcp::socket sock{io};
    std::mutex writeMu;
    std::thread reader;
};

TcpSender::TcpSender(const std::string& host, int port, std::function<void(const AckTimes&)> onAck)
    : impl_(std::make_unique<Impl>()) {
    asio::ip::tcp::resolver res(impl_->io);
    asio::connect(impl_->sock, res.resolve(host, std::to_string(port)));
    impl_->sock.set_option(asio::ip::tcp::no_delay(true));
    impl_->reader = std::thread([this, onAck = std::move(onAck)] {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> msg;
        while (readMessage(impl_->sock, type, msg)) {
            if (type != kMsgAck || msg.size() < 40) continue;
            const std::uint8_t* p = msg.data();
            AckTimes a;
            a.id = get<std::uint64_t>(p);
            a.decode = get<std::int64_t>(p);
            a.convert = get<std::int64_t>(p);
            a.copy = get<std::int64_t>(p);
            a.present = get<std::int64_t>(p);
            onAck(a);
        }
    });
}

TcpSender::~TcpSender() {
    asio::error_code ec;
    impl_->sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    impl_->sock.close(ec);
    if (impl_->reader.joinable()) impl_->reader.join();
}

void TcpSender::send(const EncodedUnit& unit) {
    std::vector<std::uint8_t> b;
    b.reserve(unit.data.size() + 9);
    put<std::uint64_t>(b, unit.id);
    b.push_back(unit.keyframe ? 1 : 0);
    b.insert(b.end(), unit.data.begin(), unit.data.end());
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    writeMessage(impl_->sock, kMsgUnit, b);
}

struct TcpReceiver::Impl {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io};
    asio::ip::tcp::socket sock{io};
    std::mutex writeMu;
    std::atomic<bool> connected{false};
    std::thread thread;
};

TcpReceiver::TcpReceiver(int port, std::function<void(EncodedUnit&&)> onUnit) : impl_(std::make_unique<Impl>()) {
    asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), static_cast<unsigned short>(port));
    impl_->acceptor.open(ep.protocol());
    impl_->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(ep);
    impl_->acceptor.listen();
    impl_->thread = std::thread([this, onUnit = std::move(onUnit)] {
        asio::error_code ec;
        impl_->acceptor.accept(impl_->sock, ec);
        if (ec) return;
        impl_->sock.set_option(asio::ip::tcp::no_delay(true), ec);
        impl_->connected = true;
        std::cout << "sender connected from " << impl_->sock.remote_endpoint(ec).address().to_string() << "\n";
        std::uint8_t type = 0;
        std::vector<std::uint8_t> msg;
        while (readMessage(impl_->sock, type, msg)) {
            if (type != kMsgUnit || msg.size() < 9) continue;
            const std::uint8_t* p = msg.data();
            EncodedUnit u;
            u.id = get<std::uint64_t>(p);
            u.keyframe = get<std::uint8_t>(p) != 0;
            u.data.assign(p, static_cast<const std::uint8_t*>(msg.data() + msg.size()));
            onUnit(std::move(u));
        }
        impl_->connected = false;
        std::cout << "sender disconnected\n";
    });
}

TcpReceiver::~TcpReceiver() {
    asio::error_code ec;
    impl_->acceptor.close(ec);
    impl_->sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    impl_->sock.close(ec);
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool TcpReceiver::connected() const { return impl_->connected; }

void TcpReceiver::sendAck(const AckTimes& a) {
    if (!impl_->connected) return;
    std::vector<std::uint8_t> b;
    put<std::uint64_t>(b, a.id);
    put<std::int64_t>(b, a.decode);
    put<std::int64_t>(b, a.convert);
    put<std::int64_t>(b, a.copy);
    put<std::int64_t>(b, a.present);
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    try {
        writeMessage(impl_->sock, kMsgAck, b);
    } catch (const std::exception&) {
    }
}

}  // namespace v6
