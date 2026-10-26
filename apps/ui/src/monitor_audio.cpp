#include "monitor_audio.hpp"

#include "audio_out.hpp"
#include "connection.hpp"

#include "dc/lanmedia/audio_link.hpp"
#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

namespace dc::ui {

using namespace dc::proto;

namespace {

constexpr int kRetryMs = 3000;
constexpr int kTickMs = 100;

const char* codecName(lan::AudioCodec c) {
    switch (c) {
    case lan::AudioCodec::Pcm16: return "PCM 16 ビット";
    case lan::AudioCodec::Pcm24: return "PCM 24 ビット";
    case lan::AudioCodec::Opus: return "Opus";
    }
    return "";
}

} // namespace

MonitorAudio::MonitorAudio(Connection& connection, Options options, QObject* parent)
    : QObject(parent), connection_(connection), options_(options) {
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &MonitorAudio::follow);
    connect(&tick_, &QTimer::timeout, this, &MonitorAudio::tick);
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
        if (stream_ <= 0) return;
        for (const auto& c : d.changes)
            if (c.path.rfind("/lan/stream/", 0) == 0) {
                emit changed();
                return;
            }
    });
}

MonitorAudio::~MonitorAudio() { close(); }

void MonitorAudio::setStream(int n) {
    n = std::clamp(n, 0, static_cast<int>(proto::kMaxLanStreams));
    if (n == stream_) return;
    stream_ = n;
    follow();
}

void MonitorAudio::setVolume(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (v == volume_.load()) return;
    volume_.store(v);
    emit changed();
}

std::uint16_t MonitorAudio::localPort() const { return input_ ? input_->localPort() : 0; }

lan::AudioReceiver::Stats MonitorAudio::stats() const { return input_ ? input_->stats() : lan::AudioReceiver::Stats{}; }

void MonitorAudio::follow() {
    // A different stream than the one subscribed: start over (the receiver takes one stream).
    if (input_ && subscribedStream_ != stream_) {
        if (subscription_ > 0 && connection_.ready())
            connection_.call("/lan/unsubscribe", {{"id", std::int64_t{subscription_}}});
        subscription_ = 0;
        close();
    }
    if (stream_ <= 0 || !connection_.ready()) {
        if (stream_ <= 0) close();
        emit changed();
        return;
    }
    if (!input_) open();
    if (input_ && subscription_ == 0 && !requesting_ && !retry_.isActive()) subscribe();
    emit changed();
}

void MonitorAudio::open() {
    try {
        lan::AudioInput::Options o;
        o.receiver.channels = 2;
        input_ = std::make_unique<lan::AudioInput>(o);
    } catch (const std::exception& e) {
        error_ = QStringLiteral("受信用のポートを開けません（%1）").arg(QString::fromLocal8Bit(e.what()));
        retry_.start(kRetryMs);
        return;
    }
    // mixd sends from UDP 47810 on the engine's host: knock there for a reply-only firewall.
    try {
        input_->setExpectedSource(lan::UdpTransport::resolve(connection_.host().toStdString(), lan::kDefaultMediaPort));
    } catch (const std::exception&) {
    }
    subscribedStream_ = stream_;
    error_.clear();
    output_ = AudioOut::create([this](float* out, std::size_t frames, double now) { render(out, frames, now); },
                               options_.nullOutput);
    lastFrames_ = 0;
    tick_.start(kTickMs);
}

void MonitorAudio::close() {
    ++generation_;
    requesting_ = false;
    retry_.stop();
    tick_.stop();
    output_.reset();  // joins the output thread: nothing pulls the input any more
    input_.reset();
    subscribedStream_ = 0;
    playing_ = false;
    sounding_ = false;
    peak_ = 0;
    if (levelDb_ != -120) {
        levelDb_ = -120;
        emit levelChanged();
    }
}

void MonitorAudio::subscribe() {
    requesting_ = true;
    const std::uint64_t gen = generation_;
    const std::string stream = "audio/" + std::to_string(stream_);
    connection_.call("/lan/subscribe", {{"stream", stream}, {"port", std::int64_t{input_->localPort()}}},
                     [this, gen](const Result& r) {
                         const auto* id = std::get_if<std::int64_t>(&r.value);
                         if (gen != generation_) {
                             if (r.code == ErrorCode::Ok && id && connection_.ready())
                                 connection_.call("/lan/unsubscribe", {{"id", *id}});
                             return;
                         }
                         requesting_ = false;
                         if (r.code == ErrorCode::Ok && id) {
                             subscription_ = static_cast<int>(*id);
                             error_.clear();
                         } else {
                             error_ = QStringLiteral("モニター音声を申し込めません：%1").arg(Connection::describe(r));
                             retry_.start(kRetryMs);
                         }
                         follow();
                     });
}

void MonitorAudio::render(float* out, std::size_t frames, double now) {
    input_->pull(out, frames, now);
    const auto gain = static_cast<float>(volume_.load(std::memory_order_relaxed));
    float peak = 0;
    for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] *= gain;
        peak = std::max(peak, std::abs(out[i]));
    }
    // The tick reads and resets it: the peak since the last tick.
    float old = peak_.load(std::memory_order_relaxed);
    while (peak > old && !peak_.compare_exchange_weak(old, peak, std::memory_order_relaxed)) {
    }
}

void MonitorAudio::tick() {
    const float peak = peak_.exchange(0.0f, std::memory_order_relaxed);
    const double db = peak > 1e-6f ? std::max(-120.0, 20.0 * std::log10(static_cast<double>(peak))) : -120.0;
    if (db != levelDb_) {
        levelDb_ = db;
        emit levelChanged();
    }
    const auto s = stats();
    const bool playing = s.state == lan::AudioReceiver::State::Running && s.received != lastFrames_;
    lastFrames_ = s.received;
    if (playing != playing_ || playing) {
        playing_ = playing;
        emit changed();
    }
}

QString MonitorAudio::statusText() const {
    if (stream_ <= 0) return {};
    if (!connection_.ready()) return QStringLiteral("エンジンにつながっていません");
    if (!error_.isEmpty()) return error_;
    if (output_ && !output_->error().isEmpty()) return QStringLiteral("音を出せません：%1").arg(output_->error());
    if (subscription_ == 0) return QStringLiteral("申し込んでいます…");
    if (playing_) return {};
    const std::string base = "/lan/stream/" + std::to_string(stream_);
    if (!connection_.tree().getAs<bool>(base + "/active").value_or(false))
        return QStringLiteral("LAN 送信 %1 に出している音声がありません（ミキサーでマスターなどの出力先に足してください）").arg(stream_);
    return QStringLiteral("音声を待っています");
}

QString MonitorAudio::infoText() const {
    if (!input_) return {};
    const auto s = stats();
    QStringList parts;
    if (s.state == lan::AudioReceiver::State::Running) {
        parts << QString::fromUtf8(codecName(s.codec));
        parts << QStringLiteral("バッファ %1 ms").arg(std::lround(s.fillMs));
        if (s.received > 0 && s.lost + s.late > 0)
            parts << QStringLiteral("欠落 %1%").arg(100.0 * double(s.lost + s.late) / double(s.received + s.lost), 0, 'f', 1);
    }
    if (output_) parts << output_->deviceName();
    parts.removeAll(QString());
    return parts.join(QStringLiteral("・"));
}

} // namespace dc::ui
