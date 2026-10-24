#include "connection.hpp"

#include "dc/protocol/constants.hpp"

#include <QAbstractSocket>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>

#include <algorithm>
#include <chrono>

namespace dc::ui {

using namespace dc::proto;

namespace {

Result localError(std::uint64_t id, const QString& message) {
    Result r;
    r.id = id;
    r.code = ErrorCode::Internal;
    r.message = message.toStdString();
    return r;
}

const char* errorText(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "成功";
    case ErrorCode::BadRequest: return "要求の形式が正しくありません";
    case ErrorCode::NotFound: return "対象が見つかりません";
    case ErrorCode::TypeMismatch: return "値の型が違います";
    case ErrorCode::OutOfRange: return "範囲外の値です";
    case ErrorCode::ReadOnly: return "変更できない項目です";
    case ErrorCode::UnsupportedVersion: return "プロトコルのバージョンが違います";
    case ErrorCode::Unauthorized: return "接続が許可されていません";
    case ErrorCode::Conflict: return "今の状態ではできません";
    case ErrorCode::Internal: return "内部エラー";
    case ErrorCode::UnknownMethod: return "未対応の操作です";
    case ErrorCode::LimitReached: return "上限に達しています";
    }
    return "エラー";
}

} // namespace

Connection::Connection(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)), clientName_(options_.clientName.toStdString()),
      decoder_(kMaxFrameBytes) {
    pingTimer_.setInterval(options_.pingIntervalMs);
    connect(&pingTimer_, &QTimer::timeout, this, [this] {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        send(Ping{static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count())});
    });
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &Connection::openSocket);
    handshakeTimer_.setSingleShot(true);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] {
        lastError_ = QStringLiteral("応答がありません");
        if (socket_) socket_->abort();
    });

    if (options_.meters) {
        meterSocket_ = new QUdpSocket(this);
        // Remote controllers receive meters over the LAN, the local UI over loopback.
        const QHostAddress bindTo =
            options_.role == ClientRole::Remote ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(QHostAddress::LocalHost);
        if (meterSocket_->bind(bindTo, 0)) {
            connect(meterSocket_, &QUdpSocket::readyRead, this, &Connection::readMeters);
        } else {
            delete meterSocket_;
            meterSocket_ = nullptr;
        }
    }
}

Connection::~Connection() {
    running_ = false;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
    // Handlers may touch objects that are being destroyed too; drop them silently.
    pending_.clear();
}

quint16 Connection::meterPort() const { return meterSocket_ ? meterSocket_->localPort() : 0; }

QString Connection::address() const { return QStringLiteral("%1:%2").arg(options_.host).arg(options_.port); }

QString Connection::statusText() const {
    switch (state_) {
    case State::Idle: return QStringLiteral("未接続");
    case State::Connecting:
    case State::Handshaking: return QStringLiteral("接続中…");
    case State::Ready: return QStringLiteral("接続済み");
    case State::Waiting:
        if (needsPairing_) return pairingMessage_;
        return lastError_.isEmpty() ? QStringLiteral("切断されました。再接続します")
                                    : QStringLiteral("切断されました（%1）。再接続します").arg(lastError_);
    }
    return {};
}

void Connection::setState(State s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged();
}

void Connection::start() {
    if (running_) return;
    running_ = true;
    reconnectDelayMs_ = options_.reconnectMinMs;
    openSocket();
}

void Connection::stop() {
    running_ = false;
    reconnectTimer_.stop();
    if (socket_) {
        send(Bye{"client exit"});
        socket_->flush();
        socket_->abort();
    }
    setState(State::Idle);
}

void Connection::openSocket() {
    if (!running_) return;
    if (socket_) {
        socket_->disconnect(this);
        socket_->deleteLater();
    }
    socket_ = new QTcpSocket(this);
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(socket_, &QTcpSocket::connected, this, &Connection::onConnected);
    connect(socket_, &QTcpSocket::readyRead, this, &Connection::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &Connection::onSocketClosed);
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err != QAbstractSocket::RemoteHostClosedError && lastError_.isEmpty()) lastError_ = socket_->errorString();
        // A failed connect never emits disconnected().
        if (socket_->state() == QAbstractSocket::UnconnectedState) onSocketClosed();
    });
    decoder_ = FrameDecoder(kMaxFrameBytes);
    gotWelcome_ = false;
    resyncPending_ = false;
    setState(State::Connecting);
    socket_->connectToHost(options_.host, options_.port);
    handshakeTimer_.start(options_.handshakeTimeoutMs);
}

void Connection::onConnected() {
    lastError_.clear();
    setState(State::Handshaking);
    Hello hello;
    hello.proto = kProtocolVersion;
    hello.clientName = clientName_;
    hello.role = options_.role;
    hello.meterPort = meterPort();
    hello.clientId = options_.clientId;
    hello.token = options_.token;
    if (options_.token.empty()) hello.pairCode = pairCode_;
    send(hello);
}

void Connection::onSocketClosed() {
    if (!socket_) return;
    // errorOccurred and disconnected can both report the same close.
    if (state_ == State::Waiting || state_ == State::Idle) return;
    handshakeTimer_.stop();
    pingTimer_.stop();
    failPending(QStringLiteral("接続が切れました"));
    if (!running_) {
        setState(State::Idle);
        return;
    }
    setState(State::Waiting);
    // Without a code, connecting again would only be refused again.
    if (!needsPairing_) scheduleReconnect();
}

void Connection::pair(const QString& code) {
    pairCode_ = code.trimmed().toStdString();
    needsPairing_ = false;
    pairingMessage_.clear();
    reconnectTimer_.stop();
    reconnectDelayMs_ = options_.reconnectMinMs;
    if (running_ && state_ == State::Waiting) openSocket();
    else emit stateChanged();
}

void Connection::scheduleReconnect() {
    reconnectTimer_.start(reconnectDelayMs_);
    reconnectDelayMs_ = std::min(reconnectDelayMs_ * 2, options_.reconnectMaxMs);
}

void Connection::onReadyRead() {
    const QByteArray bytes = socket_->readAll();
    decoder_.feed({reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())});
    while (auto payload = decoder_.next()) {
        auto decoded = decodePayload(*payload);
        if (!decoded.message) {
            lastError_ = QStringLiteral("不正なメッセージ: %1").arg(QString::fromStdString(decoded.error));
            socket_->abort();
            return;
        }
        handle(std::move(*decoded.message));
        if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    }
    if (decoder_.failed()) {
        lastError_ = QStringLiteral("メッセージが大きすぎます");
        socket_->abort();
    }
}

void Connection::handle(Message msg) {
    if (auto* w = std::get_if<Welcome>(&msg)) {
        welcome_ = *w;
        gotWelcome_ = true;
        pairCode_.clear();
        if (!w->token.empty() && w->token != options_.token) {
            options_.token = w->token;
            emit tokenChanged(QString::fromStdString(w->token));
        }
        return;
    }
    if (auto* s = std::get_if<Snapshot>(&msg)) {
        tree_.load(*s);
        resyncPending_ = false;
        if (state_ != State::Ready && gotWelcome_) {
            handshakeTimer_.stop();
            pingTimer_.start();
            reconnectDelayMs_ = options_.reconnectMinMs;
            setState(State::Ready);
        }
        emit reset();
        return;
    }
    if (auto* d = std::get_if<Delta>(&msg)) {
        switch (tree_.apply(*d)) {
        case ParamTree::ApplyResult::Applied: emit deltaApplied(*d); break;
        case ParamTree::ApplyResult::Stale: break;
        case ParamTree::ApplyResult::Gap:
            if (!resyncPending_) {
                resyncPending_ = true;
                call("/session/resync");
            }
            break;
        }
        return;
    }
    if (auto* r = std::get_if<Result>(&msg)) {
        auto it = pending_.find(r->id);
        if (it == pending_.end()) return;
        ResultHandler h = std::move(it->second);
        pending_.erase(it);
        if (h) h(*r);
        return;
    }
    if (auto* e = std::get_if<Event>(&msg)) {
        emit eventReceived(*e);
        return;
    }
    if (auto* p = std::get_if<Ping>(&msg)) {
        send(Pong{p->t});
        return;
    }
    if (auto* e = std::get_if<Error>(&msg)) {
        lastError_ = QString::fromStdString(e->message.empty() ? errorName(e->code) : e->message);
        if (e->code == ErrorCode::Unauthorized && options_.role != ClientRole::Local) {
            // The engine's reasons (mixd lan_service): not paired, wrong code, pairing not open,
            // unknown token. A refused token is forgotten so that the next try uses a code.
            const std::string& m = e->message;
            if (m.find("token") != std::string::npos && !options_.token.empty()) {
                options_.token.clear();
                emit tokenChanged(QString());
            }
            pairingMessage_ = m.find("wrong") != std::string::npos ? QStringLiteral("数字が違います。もう一度入れてください")
                            : m.find("not open") != std::string::npos
                                ? QStringLiteral("エンジンの PC で受付が始まっていません（LAN ページの「受付を始める」）")
                            : m.find("token") != std::string::npos
                                ? QStringLiteral("ペアリングが取り消されました。もう一度ペアリングしてください")
                                : QStringLiteral("初めてつなぐ PC です。ペアリングしてください");
            needsPairing_ = true;
            pairCode_.clear();
        }
        return; // the server closes the connection
    }
    if (std::holds_alternative<Bye>(msg)) {
        lastError_ = QStringLiteral("サーバーが終了しました");
        socket_->disconnectFromHost();
    }
}

void Connection::send(const Message& msg) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) return;
    const Bytes frame = encodeFrame(msg);
    socket_->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size()));
}

std::uint64_t Connection::request(Message msg, std::uint64_t id, ResultHandler handler) {
    if (state_ != State::Ready) {
        if (handler)
            QMetaObject::invokeMethod(
                this, [h = std::move(handler), id] { h(localError(id, QStringLiteral("エンジンに接続していません"))); },
                Qt::QueuedConnection);
        return id;
    }
    pending_[id] = std::move(handler);
    send(msg);
    return id;
}

std::uint64_t Connection::set(std::vector<Change> changes, ResultHandler handler) {
    const std::uint64_t id = nextId_++;
    return request(Set{id, std::move(changes)}, id, std::move(handler));
}

std::uint64_t Connection::call(std::string method, Args args, ResultHandler handler) {
    const std::uint64_t id = nextId_++;
    return request(Call{id, std::move(method), std::move(args)}, id, std::move(handler));
}

void Connection::failPending(const QString& reason) {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [id, h] : pending)
        if (h) h(localError(id, reason));
}

void Connection::readMeters() {
    while (meterSocket_->hasPendingDatagrams()) {
        const QNetworkDatagram dg = meterSocket_->receiveDatagram();
        if (state_ != State::Ready) continue;
        const QByteArray bytes = dg.data();
        if (auto p = decodeMeterPacket(
                {reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())}))
            emit metersReceived(*p);
    }
}

QString Connection::describe(const Result& r) {
    QString text = QString::fromUtf8(errorText(r.code));
    if (!r.message.empty()) text += QStringLiteral("（%1）").arg(QString::fromStdString(r.message));
    return text;
}

} // namespace dc::ui
