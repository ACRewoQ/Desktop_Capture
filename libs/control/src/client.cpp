#include "dc/control/client.hpp"

namespace dc::control {

using asio::ip::tcp;
using namespace dc::proto;

ControlClient::ControlClient(Options options)
    : options_(std::move(options)), socket_(io_), pingTimer_(io_) {}

ControlClient::~ControlClient() { disconnect(); }

bool ControlClient::connect(std::chrono::milliseconds timeout, std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        disconnect();
        return false;
    };

    try {
        tcp::resolver resolver(io_);
        auto endpoints = resolver.resolve(options_.host, std::to_string(options_.port));
        asio::connect(socket_, endpoints);
        socket_.set_option(tcp::no_delay(true));
    } catch (const std::exception& e) {
        return fail(std::string("connect failed: ") + e.what());
    }

    Hello hello;
    hello.proto = kProtocolVersion;
    hello.clientName = options_.clientName;
    hello.role = options_.role;
    hello.meterPort = options_.meterPort;
    hello.token = options_.token;
    hello.pairCode = options_.pairCode;
    send(hello);
    read();
    schedulePing();
    thread_ = std::thread([this] { io_.run(); });

    std::unique_lock lock(mutex_);
    const bool ok = cv_.wait_for(lock, timeout, [&] { return (gotWelcome_ && gotSnapshot_) || !lastError_.empty(); });
    if (!ok) {
        lock.unlock();
        return fail("timed out waiting for Welcome/Snapshot");
    }
    if (!lastError_.empty()) {
        auto msg = lastError_;
        lock.unlock();
        return fail(msg);
    }
    connected_ = true;
    return true;
}

void ControlClient::disconnect() {
    if (thread_.joinable()) {
        asio::post(io_, [this] {
            asio::error_code ec;
            pingTimer_.cancel();
            socket_.shutdown(tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        });
        thread_.join();
    } else {
        asio::error_code ec;
        socket_.close(ec);
    }
    connected_ = false;
    failAll("disconnected");
}

void ControlClient::schedulePing() {
    pingTimer_.expires_after(options_.pingInterval);
    pingTimer_.async_wait([this](asio::error_code ec) {
        if (ec) return;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        send(Ping{static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count())});
        schedulePing();
    });
}

namespace {
std::future<Result> notConnected() {
    std::promise<Result> p;
    Result r;
    r.code = ErrorCode::Internal;
    r.message = "not connected";
    p.set_value(r);
    return p.get_future();
}
} // namespace

std::future<Result> ControlClient::set(std::vector<Change> changes) {
    if (!connected_) return notConnected();
    std::promise<Result> p;
    auto f = p.get_future();
    Set msg;
    {
        std::lock_guard lock(mutex_);
        msg.id = nextId_++;
        pending_.emplace(msg.id, std::move(p));
    }
    msg.changes = std::move(changes);
    asio::post(io_, [this, m = std::move(msg)]() mutable { send(std::move(m)); });
    return f;
}

std::future<Result> ControlClient::call(std::string method, Args args) {
    if (!connected_) return notConnected();
    std::promise<Result> p;
    auto f = p.get_future();
    Call msg;
    {
        std::lock_guard lock(mutex_);
        msg.id = nextId_++;
        pending_.emplace(msg.id, std::move(p));
    }
    msg.method = std::move(method);
    msg.args = std::move(args);
    asio::post(io_, [this, m = std::move(msg)]() mutable { send(std::move(m)); });
    return f;
}

ParamTree ControlClient::tree() const {
    std::lock_guard lock(mutex_);
    return tree_;
}

Welcome ControlClient::welcome() const {
    std::lock_guard lock(mutex_);
    return welcome_;
}

bool ControlClient::waitForRev(std::uint64_t rev, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return tree_.rev() >= rev; });
}

void ControlClient::send(Message msg) {
    const bool idle = queue_.empty();
    queue_.push_back(encodeFrame(msg));
    if (idle) writeNext();
}

void ControlClient::writeNext() {
    asio::async_write(socket_, asio::buffer(queue_.front()), [this](asio::error_code ec, std::size_t) {
        if (ec) return; // the read side reports the disconnect
        queue_.pop_front();
        if (!queue_.empty()) writeNext();
    });
}

void ControlClient::read() {
    socket_.async_read_some(asio::buffer(readBuf_), [this](asio::error_code ec, std::size_t n) {
        if (ec) {
            const bool was = connected_.exchange(false);
            pingTimer_.cancel();
            {
                std::lock_guard lock(mutex_);
                if (lastError_.empty()) lastError_ = "connection closed: " + ec.message();
            }
            cv_.notify_all();
            failAll("disconnected");
            if (was && onDisconnected) onDisconnected(ec.message());
            return;
        }
        decoder_.feed({readBuf_.data(), n});
        while (auto payload = decoder_.next()) {
            auto decoded = decodePayload(*payload);
            if (!decoded.message) {
                asio::error_code ignored;
                socket_.close(ignored);
                return;
            }
            handle(std::move(*decoded.message));
        }
        read();
    });
}

void ControlClient::handle(Message msg) {
    if (auto* w = std::get_if<Welcome>(&msg)) {
        {
            std::lock_guard lock(mutex_);
            welcome_ = *w;
            gotWelcome_ = true;
        }
        cv_.notify_all();
    } else if (auto* s = std::get_if<Snapshot>(&msg)) {
        {
            std::lock_guard lock(mutex_);
            tree_.load(*s);
            resyncRequested_ = false;
        }
        // Callbacks run before waiters wake up, so waitForRev() also means "callbacks done".
        if (onSnapshot) onSnapshot(*s);
        {
            std::lock_guard lock(mutex_);
            gotSnapshot_ = true;
        }
        cv_.notify_all();
    } else if (auto* d = std::get_if<Delta>(&msg)) {
        ParamTree::ApplyResult r;
        {
            std::lock_guard lock(mutex_);
            r = tree_.apply(*d);
        }
        if (r == ParamTree::ApplyResult::Applied) {
            if (onDelta) onDelta(*d);
            cv_.notify_all();
        } else if (r == ParamTree::ApplyResult::Gap) {
            bool request = false;
            {
                std::lock_guard lock(mutex_);
                request = !resyncRequested_;
                resyncRequested_ = true;
            }
            if (request) call("/session/resync");
        }
    } else if (auto* res = std::get_if<Result>(&msg)) {
        std::promise<Result> p;
        bool found = false;
        {
            std::lock_guard lock(mutex_);
            if (auto it = pending_.find(res->id); it != pending_.end()) {
                p = std::move(it->second);
                pending_.erase(it);
                found = true;
            }
        }
        if (found) p.set_value(*res);
    } else if (auto* ev = std::get_if<Event>(&msg)) {
        if (onEvent) onEvent(*ev);
    } else if (auto* err = std::get_if<Error>(&msg)) {
        {
            std::lock_guard lock(mutex_);
            lastError_ = std::string(errorName(err->code)) + ": " + err->message;
        }
        cv_.notify_all();
    } else if (auto* bye = std::get_if<Bye>(&msg)) {
        std::lock_guard lock(mutex_);
        lastError_ = "server said bye: " + bye->reason;
    }
}

void ControlClient::failAll(const std::string& reason) {
    std::map<std::uint64_t, std::promise<Result>> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, p] : pending) {
        Result r;
        r.id = id;
        r.code = ErrorCode::Internal;
        r.message = reason;
        p.set_value(r);
    }
}

} // namespace dc::control
