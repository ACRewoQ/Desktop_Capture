#pragma once

#include "dc/lanmedia/video.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dc::lan {
class VideoInput;
}

namespace dc::ui {

class Connection;

// The remote's preview (protocol.md 10): while a viewer is on screen, subscribes to capd's
// video/preview stream and hands each decodable access unit to the viewers. Nobody watching = no
// subscription, so capd does not encode. Decoding and drawing are PreviewVideo's (Windows only).
//
// GUI thread, except the sinks, which run on the receive threads (one call at a time).
class PreviewStream : public QObject {
    Q_OBJECT
    // Why there is no picture ("" while frames arrive or nobody watches), in Japanese.
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    // "960×540・30 fps・受信 29.9 fps" while watching.
    Q_PROPERTY(QString infoText READ infoText NOTIFY changed)
    Q_PROPERTY(bool receiving READ receiving NOTIFY changed)
    Q_PROPERTY(bool subscribed READ subscribed NOTIFY changed)
    // Capture (mix) -> received here, median of the last frames, ms; < 0 while the clock offset to the
    // engine PC is not known yet.
    Q_PROPERTY(double networkMs READ networkMs NOTIFY changed)

public:
    using Frame = lan::VideoReceiver::Frame;
    using Sink = std::function<void(const Frame&)>;

    // stream: "video/preview" (the mix as it goes out) or "video/next" (its next scene alone, for the
    // scene page); knockPort: the UDP port capd sends that stream from.
    explicit PreviewStream(Connection& connection, QObject* parent = nullptr);
    PreviewStream(Connection& connection, std::string stream, std::uint16_t knockPort, QObject* parent = nullptr);
    ~PreviewStream() override;

    // A viewer came on screen: subscribes while at least one is attached. The first frame a new viewer
    // gets is a keyframe only if it attached before the subscription (the receiver starts at one);
    // later viewers wait for the next keyframe.
    int attach(Sink sink);
    // After this returns the sink is not running and is not called again.
    void detach(int id);

    QString statusText() const;
    QString infoText() const;
    bool receiving() const { return receiving_; }
    bool subscribed() const { return subscription_ > 0; }
    int subscription() const { return subscription_; }
    std::uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
    std::uint16_t localPort() const;
    lan::VideoReceiver::Stats stats() const;
    double networkMs() const { return networkMs_; }

    // The engine PC's clock (frames' captureTime) on this device's clock (lan::nowSeconds), from
    // /session/clock: NaN until the first answer. Any thread.
    double toLocalTime(double engineTime) const;
    bool clockKnown() const { return clockKnown_.load(std::memory_order_acquire); }

signals:
    void changed();

private:
    void follow();  // subscribe / unsubscribe to match the viewers and the connection
    void subscribe();
    void closeInput();
    void tick();
    void syncClock();

    Connection& connection_;
    std::string stream_;
    std::uint16_t knockPort_;
    std::unique_ptr<lan::VideoInput> input_;
    int subscription_ = 0;
    bool requesting_ = false;
    std::uint64_t generation_ = 0;  // bumped when the input closes: late results are for an old one
    QString error_;
    QTimer retry_;
    QTimer tick_;
    bool receiving_ = false;
    double fps_ = 0;
    std::uint64_t lastFrames_ = 0;
    qint64 lastTick_ = 0;
    qint64 lastFrameTick_ = 0;  // when frames_ last moved

    mutable std::mutex sinkMutex_;
    std::map<int, Sink> sinks_;
    int nextSink_ = 1;
    std::atomic<std::uint64_t> frames_{0};

    // clock offset (engine - local, seconds) from the /session/clock answer with the shortest round trip
    struct ClockSample {
        double rtt;
        double offset;
    };
    std::vector<ClockSample> clockSamples_;
    qint64 lastClockCall_ = 0;
    bool clockPending_ = false;
    bool clockUnsupported_ = false;
    std::atomic<double> clockOffset_{0};
    std::atomic<bool> clockKnown_{false};
    // capture -> arrival, filled on the receive thread
    std::mutex networkMutex_;
    std::vector<double> networkSamples_;
    double networkMs_ = -1;
};

} // namespace dc::ui
