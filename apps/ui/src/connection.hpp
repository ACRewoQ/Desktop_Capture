#pragma once

#include "dc/protocol/codec.hpp"
#include "dc/protocol/messages.hpp"
#include "dc/protocol/meter.hpp"
#include "dc/protocol/param_tree.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QTcpSocket;
class QUdpSocket;

namespace dc::ui {

// Control protocol client on Qt's own sockets (docs/spec/protocol.md). Everything runs on the GUI
// thread: no locking, and the parameter mirror can be read directly by the models. Reconnects by
// itself and rebuilds the mirror from the snapshot every time it does.
class Connection : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString address READ address CONSTANT)
    Q_PROPERTY(QString serverName READ serverName NOTIFY stateChanged)

public:
    enum class State { Idle, Connecting, Handshaking, Ready, Waiting };

    struct Options {
        QString host = QStringLiteral("127.0.0.1");
        quint16 port = 47800;
        QString clientName = QStringLiteral("dc-ui");
        proto::ClientRole role = proto::ClientRole::Local;
        bool meters = true;
        int pingIntervalMs = 5000;
        int reconnectMinMs = 500;
        int reconnectMaxMs = 5000;
        int handshakeTimeoutMs = 5000;
    };

    using ResultHandler = std::function<void(const proto::Result&)>;

    explicit Connection(Options options, QObject* parent = nullptr);
    ~Connection() override;

    // Starts connecting and keeps reconnecting until stop().
    void start();
    void stop();

    State state() const { return state_; }
    bool ready() const { return state_ == State::Ready; }
    QString statusText() const;
    QString address() const;
    QString serverName() const { return QString::fromStdString(welcome_.serverName); }
    const std::string& clientName() const { return clientName_; }
    quint16 meterPort() const;

    const proto::ParamTree& tree() const { return tree_; }
    const proto::Welcome& welcome() const { return welcome_; }

    // Sends a request. The handler always runs exactly once, later on the GUI thread: with the
    // server's Result, or with an Internal error when not connected or the connection drops.
    // Deltas caused by the request have been applied to tree() when it runs.
    std::uint64_t set(std::vector<proto::Change> changes, ResultHandler handler = {});
    std::uint64_t call(std::string method, proto::Args args = {}, ResultHandler handler = {});

    // Japanese text for an error Result, for the toast.
    static QString describe(const proto::Result& r);

signals:
    void stateChanged();
    // The mirror was replaced by a snapshot (first connection, reconnection, resync).
    void reset();
    // A delta was applied to the mirror.
    void deltaApplied(const dc::proto::Delta& delta);
    void eventReceived(const dc::proto::Event& event);
    void metersReceived(const dc::proto::MeterPacket& packet);

private:
    void setState(State s);
    void openSocket();
    void scheduleReconnect();
    void onConnected();
    void onReadyRead();
    void onSocketClosed();
    void handle(proto::Message msg);
    void send(const proto::Message& msg);
    void failPending(const QString& reason);
    void readMeters();
    std::uint64_t request(proto::Message msg, std::uint64_t id, ResultHandler handler);

    Options options_;
    std::string clientName_;
    QTcpSocket* socket_ = nullptr;
    QUdpSocket* meterSocket_ = nullptr;
    QTimer pingTimer_;
    QTimer reconnectTimer_;
    QTimer handshakeTimer_;
    int reconnectDelayMs_ = 0;
    bool running_ = false;
    State state_ = State::Idle;
    QString lastError_;

    proto::FrameDecoder decoder_;
    proto::ParamTree tree_;
    proto::Welcome welcome_;
    bool gotWelcome_ = false;
    bool resyncPending_ = false;
    std::uint64_t nextId_ = 1;
    std::map<std::uint64_t, ResultHandler> pending_;
};

} // namespace dc::ui
