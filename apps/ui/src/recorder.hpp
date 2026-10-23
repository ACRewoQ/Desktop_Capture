#pragma once

#include <QObject>
#include <QString>

namespace dc::ui {

class Connection;

// The "デスクトップキャプチャ" output: recording state from /capture/* and start / stop.
class Recorder : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY changed)
    Q_PROPERTY(QString file READ file NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)

public:
    explicit Recorder(Connection& connection, QObject* parent = nullptr);

    QString state() const;
    bool recording() const { return state() == QLatin1String("recording"); }
    // Starting / stopping, or a request is on its way.
    bool busy() const;
    QString elapsedText() const;
    QString file() const;
    QString error() const;
    // "MKV · H.264 1920×1080 60fps · 自動" from the recording settings.
    QString summary() const;

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

signals:
    void changed();
    void failed(const QString& message);

private:
    void request(const char* method);

    Connection& connection_;
    bool requesting_ = false;
};

} // namespace dc::ui
