#include "preview_stream.hpp"

#include "connection.hpp"

#include "dc/lanmedia/video_link.hpp"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace dc::ui {

using namespace dc::proto;

namespace {

constexpr int kRetryMs = 3000;
constexpr int kTickMs = 500;
constexpr qint64 kStalledMs = 1500;  // no frame for this long = not receiving
constexpr qint64 kClockFirstMs = 300;   // clock samples while the offset is young
constexpr qint64 kClockEveryMs = 5000;  // then now and then (the two clocks drift a little)
constexpr std::size_t kClockSamples = 8;
constexpr std::size_t kNetworkSamples = 64;

double median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

qint64 num(const Connection& c, const char* path) {
    return static_cast<qint64>(c.tree().getAs<std::int64_t>(path).value_or(0));
}

} // namespace

PreviewStream::PreviewStream(Connection& connection, QObject* parent) : QObject(parent), connection_(connection) {
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &PreviewStream::follow);
    connect(&tick_, &QTimer::timeout, this, &PreviewStream::tick);
    // A new connection has none of the old subscriptions (they end with the connection).
    connect(&connection_, &Connection::reset, this, [this] {
        subscription_ = 0;
        follow();
    });
    connect(&connection_, &Connection::stateChanged, this, [this] {
        if (!connection_.ready()) {
            subscription_ = 0;
            ++generation_;
            requesting_ = false;
        }
        follow();
    });
    connect(&connection_, &Connection::deltaApplied, this, [this](const Delta& d) {
        for (const auto& c : d.changes)
            if (c.path.rfind("/video/preview/", 0) == 0) {
                emit changed();
                return;
            }
    });
}

PreviewStream::~PreviewStream() {
    {
        std::lock_guard lock(sinkMutex_);
        sinks_.clear();
    }
    input_.reset();
}

int PreviewStream::attach(Sink sink) {
    int id = 0;
    {
        std::lock_guard lock(sinkMutex_);
        id = nextSink_++;
        sinks_[id] = std::move(sink);
    }
    follow();
    return id;
}

void PreviewStream::detach(int id) {
    {
        std::lock_guard lock(sinkMutex_);
        sinks_.erase(id);
    }
    follow();
}

std::uint16_t PreviewStream::localPort() const { return input_ ? input_->localPort() : 0; }

lan::VideoReceiver::Stats PreviewStream::stats() const { return input_ ? input_->stats() : lan::VideoReceiver::Stats{}; }

void PreviewStream::follow() {
    bool watched = false;
    {
        std::lock_guard lock(sinkMutex_);
        watched = !sinks_.empty();
    }
    if (!watched || !connection_.ready()) {
        if (subscription_ > 0 && connection_.ready())
            connection_.call("/lan/unsubscribe", {{"id", std::int64_t{subscription_}}});
        subscription_ = 0;
        if (!watched) closeInput();
        emit changed();
        return;
    }
    if (!input_) {
        try {
            input_ = std::make_unique<lan::VideoInput>(lan::VideoInput::Options{}, [this](const Frame& f) {
                frames_.fetch_add(1, std::memory_order_relaxed);
                if (clockKnown()) {
                    std::lock_guard lock(networkMutex_);
                    if (networkSamples_.size() >= kNetworkSamples) networkSamples_.erase(networkSamples_.begin());
                    networkSamples_.push_back((f.arrival - toLocalTime(f.captureTime)) * 1000.0);
                }
                std::lock_guard lock(sinkMutex_);
                for (auto& [id, sink] : sinks_) sink(f);
            });
        } catch (const std::exception& e) {
            error_ = QStringLiteral("受信用のポートを開けません（%1）").arg(QString::fromLocal8Bit(e.what()));
            retry_.start(kRetryMs);
            emit changed();
            return;
        }
        // capd sends from the engine's host, normally from UDP 47811: knock there so that a firewall
        // that only lets replies in (a remote on a "public" network) lets the stream in.
        try {
            input_->setExpectedSource(lan::UdpTransport::resolve(connection_.host().toStdString(), lan::kDefaultVideoPort));
        } catch (const std::exception&) {
        }
        lastTick_ = QDateTime::currentMSecsSinceEpoch();
        lastFrameTick_ = lastTick_;
        lastFrames_ = frames_.load();
        tick_.start(kTickMs);
    }
    if (subscription_ == 0 && !requesting_ && !retry_.isActive()) subscribe();
}

void PreviewStream::subscribe() {
    requesting_ = true;
    const std::uint64_t gen = generation_;
    connection_.call("/lan/subscribe", {{"stream", std::string("video/preview")}, {"port", std::int64_t{input_->localPort()}}},
                     [this, gen](const Result& r) {
                         const auto* id = std::get_if<std::int64_t>(&r.value);
                         if (gen != generation_) {  // closed meanwhile
                             if (r.code == ErrorCode::Ok && id && connection_.ready())
                                 connection_.call("/lan/unsubscribe", {{"id", *id}});
                             return;
                         }
                         requesting_ = false;
                         if (r.code == ErrorCode::Ok && id) {
                             subscription_ = static_cast<int>(*id);
                             error_.clear();
                         } else {
                             error_ = QStringLiteral("プレビューを申し込めません：%1").arg(Connection::describe(r));
                             retry_.start(kRetryMs);
                         }
                         emit changed();
                         follow();  // the viewers may have gone while the request was out
                     });
    emit changed();
}

void PreviewStream::closeInput() {
    ++generation_;
    requesting_ = false;
    retry_.stop();
    tick_.stop();
    error_.clear();
    receiving_ = false;
    fps_ = 0;
    input_.reset();  // joins the receive threads; the sinks are gone or about to be
    std::lock_guard lock(networkMutex_);
    networkSamples_.clear();
    networkMs_ = -1;
}

double PreviewStream::toLocalTime(double engineTime) const {
    if (!clockKnown()) return std::numeric_limits<double>::quiet_NaN();
    return engineTime - clockOffset_.load(std::memory_order_relaxed);
}

void PreviewStream::syncClock() {
    if (clockPending_ || clockUnsupported_ || !connection_.ready()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastClockCall_ < (clockSamples_.size() < kClockSamples ? kClockFirstMs : kClockEveryMs)) return;
    lastClockCall_ = now;
    clockPending_ = true;
    const double t0 = lan::nowSeconds();
    connection_.call("/session/clock", {}, [this, t0](const Result& r) {
        const double t1 = lan::nowSeconds();
        clockPending_ = false;
        const auto* us = std::get_if<std::int64_t>(&r.value);
        if (r.code == ErrorCode::UnknownMethod) clockUnsupported_ = true;  // an older engine
        if (r.code != ErrorCode::Ok || !us) return;
        // NTP style: the engine read its clock halfway through the round trip, give or take rtt / 2.
        if (clockSamples_.size() >= kClockSamples) clockSamples_.erase(clockSamples_.begin());
        clockSamples_.push_back({t1 - t0, double(*us) / 1e6 - (t0 + t1) / 2});
        const auto best = std::min_element(clockSamples_.begin(), clockSamples_.end(),
                                           [](const ClockSample& a, const ClockSample& b) { return a.rtt < b.rtt; });
        clockOffset_.store(best->offset, std::memory_order_relaxed);
        clockKnown_.store(true, std::memory_order_release);
    });
}

void PreviewStream::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const std::uint64_t n = frames_.load();
    if (n != lastFrames_) lastFrameTick_ = now;
    if (now > lastTick_) fps_ = static_cast<double>(n - lastFrames_) * 1000.0 / static_cast<double>(now - lastTick_);
    lastFrames_ = n;
    lastTick_ = now;
    const bool receiving = now - lastFrameTick_ < kStalledMs && n > 0;
    receiving_ = receiving;
    syncClock();
    {
        std::lock_guard lock(networkMutex_);
        networkMs_ = median(networkSamples_);
    }
    emit changed();
}

QString PreviewStream::statusText() const {
    if (!input_ && error_.isEmpty()) return {};
    if (!connection_.ready()) return QStringLiteral("エンジンにつながっていません");
    if (!error_.isEmpty()) return error_;
    if (num(connection_, "/video/preview/mix") <= 0)
        return QStringLiteral("プレビューに出すミックスがありません（出力先の「リモコンのプレビュー」で選べます）");
    if (subscription_ == 0) return QStringLiteral("プレビューを申し込んでいます…");
    if (frames_.load() == 0) return QStringLiteral("映像を待っています（キャプチャーが動いていないかもしれません）");
    if (!receiving_) return QStringLiteral("映像が届いていません");
    return {};
}

QString PreviewStream::infoText() const {
    QString text = QStringLiteral("%1×%2・%3 fps")
                       .arg(num(connection_, "/video/preview/width"))
                       .arg(num(connection_, "/video/preview/height"))
                       .arg(num(connection_, "/video/preview/fps"));
    if (input_ && receiving_) text += QStringLiteral("・受信 %1 fps").arg(fps_, 0, 'f', 1);
    if (input_ && receiving_ && networkMs_ >= 0) text += QStringLiteral("・届くまで %1 ms").arg(std::lround(networkMs_));
    return text;
}

} // namespace dc::ui
