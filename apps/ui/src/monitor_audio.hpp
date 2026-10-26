#pragma once

#include "dc/lanmedia/audio_receiver.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>

namespace dc::lan {
class AudioInput;
}

namespace dc::ui {

class AudioOut;
class Connection;

// The remote's monitor audio: listens to one of the engine's LAN send streams (audio/<n>, whatever
// the mixer routes to "LAN 送信 n", usually the master) and plays it on this device's default output.
// Subscribed only while a stream is chosen and the engine is connected (protocol.md 10); stays on while
// the pages change, so the remote can listen while it mixes. Not saved: a restart starts silent.
//
// GUI thread; the receiver is pulled on the output's thread.
class MonitorAudio : public QObject {
    Q_OBJECT
    // 0 = off, 1..8 = audio/<n>.
    Q_PROPERTY(int stream READ stream WRITE setStream NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)        // sound arrives
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed) // why there is no sound, in Japanese
    Q_PROPERTY(QString infoText READ infoText NOTIFY changed)     // "Opus・バッファ 42 ms・既定の出力"
    Q_PROPERTY(double level READ level NOTIFY levelChanged)       // peak of the last ~100 ms, dBFS
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed) // 0..1, applied here

public:
    struct Options {
        bool nullOutput = false;  // render into nothing (tests)
    };

    explicit MonitorAudio(Connection& connection, Options options, QObject* parent = nullptr);
    explicit MonitorAudio(Connection& connection, QObject* parent = nullptr) : MonitorAudio(connection, Options{}, parent) {}
    ~MonitorAudio() override;

    int stream() const { return stream_; }
    void setStream(int n);
    double volume() const { return volume_.load(); }
    void setVolume(double v);

    bool playing() const { return playing_; }
    QString statusText() const;
    QString infoText() const;
    double level() const { return levelDb_; }

    bool subscribed() const { return subscription_ > 0; }
    int subscription() const { return subscription_; }
    std::uint16_t localPort() const;
    lan::AudioReceiver::Stats stats() const;

signals:
    void changed();
    void levelChanged();

private:
    void follow();
    void subscribe();
    void open();
    void close();
    void tick();
    void render(float* out, std::size_t frames, double now);

    Connection& connection_;
    Options options_;
    int stream_ = 0;
    int subscribedStream_ = 0;
    std::unique_ptr<lan::AudioInput> input_;
    std::unique_ptr<AudioOut> output_;
    int subscription_ = 0;
    bool requesting_ = false;
    std::uint64_t generation_ = 0;
    QString error_;
    QTimer retry_;
    QTimer tick_;
    bool playing_ = false;
    std::uint64_t lastFrames_ = 0;
    double levelDb_ = -120;
    std::atomic<double> volume_{1.0};
    std::atomic<float> peak_{0.0f};
    std::atomic<bool> sounding_{false};  // the receiver is running (not waiting for packets)
};

} // namespace dc::ui
