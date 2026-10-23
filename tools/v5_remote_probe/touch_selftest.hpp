#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QTimer>

#include <functional>
#include <vector>

class QQuickItem;
class QQuickWindow;
class Probe;
class StripModel;

// Automated multi-touch check with synthetic touch input (Windows InjectTouchInput), so the
// simultaneous fader behaviour can be verified on a PC without a touch screen:
//   1. up to 10 fingers drag 10 faders by different amounts at the same time
//   2. double tap on a fader resets it to 0 dB
//   3. a horizontal swipe that starts on a fader scrolls the strip list and moves no fader
// Off Windows QTest touch events stand in, to check the QML logic. A real touch screen check on the target device is still done by hand (docs/verification/v5-remote-lite.md).
class TouchSelfTest : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxContacts = 10;

    // Must run before QGuiApplication exists, so that Qt sees the injected touch device at start-up.
    static bool prepare();

    TouchSelfTest(QQuickWindow* window, StripModel* model, Probe* probe, QObject* parent = nullptr);
    void start();

signals:
    void finished(const QJsonObject& result);

private:
    struct Contact {
        int id = 0;
        QPointF scenePos;
        enum Phase { Down, Move, Up } phase = Move;
    };
    using Frame = std::vector<Contact>;
    struct Step {
        std::vector<Frame> frames;
        int settleMs = 300;
        std::function<void()> before;
        std::function<void()> check;
    };

    void buildSteps();
    void runStep();
    void nextFrame();
    bool inject(const Frame& frame);
    bool isForeground() const;
    void addResult(const QString& name, bool pass, QJsonObject details);
    QQuickItem* findItem(const QString& objectName) const;
    QPointF centerOf(QQuickItem* item) const;

    QQuickWindow* window_;
    StripModel* model_;
    Probe* probe_;
    QTimer frameTimer_;
    std::vector<Step> steps_;
    std::size_t step_ = 0;
    std::size_t frame_ = 0;
    QJsonArray results_;
    int injectErrors_ = 0;
    bool allPass_ = true;
};
