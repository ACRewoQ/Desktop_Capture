// QQuickItem that shows the newest decoded frame from a SharedFrameRing.
//
// Render thread, per frame: SharedFrameReader copies the newest BGRA frame into a texture owned by Qt's
// D3D11 device, which a QSGSimpleTextureNode draws. frameSwapped then stamps Presented for that frame.
//
// Lifetimes: every D3D object of Qt's device lives in the scene graph node (VideoNode), which Qt deletes on
// the render thread. State touched from frameSwapped lives in a shared PresentState, so neither path depends
// on the item still existing while the window tears down on two threads.
#pragma once

#include "d3d_video.hpp"
#include "pipeline.hpp"
#include "stats.hpp"

#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class VideoItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString stats READ stats NOTIFY statsChanged)
    Q_PROPERTY(QString info READ info NOTIFY infoChanged)

public:
    explicit VideoItem(QQuickItem* parent = nullptr);
    ~VideoItem() override;

    QString stats() const { return stats_; }
    QString info() const { return info_; }
    void setStatsText(const QString& s);
    void setInfoText(const QString& s);

    // Current QPC time, milliseconds modulo 1 000 000, as the test pattern burns it into each frame.
    Q_INVOKABLE QString clockText() const;

    using AckFn = std::function<void(const v6::AckTimes&)>;
    void attach(v6::SharedFrameRing* ring, v6::Stats* stats, AckFn ack);
    void detach();  // must run before the ring or the stats are destroyed

    // Render-path counters and the display-side pixel check, for the report.
    std::string counters() const;
    std::string probeText() const;

    struct PresentState {
        mutable std::mutex mu;
        v6::Stats* stats = nullptr;
        AckFn ack;
        std::vector<std::uint64_t> pending;  // copied this frame, waiting for frameSwapped
        std::atomic<std::uint64_t> nPaint{0}, nFetchTry{0}, nFetchOk{0}, nSwap{0};
        std::string probeText;
    };

signals:
    void statsChanged();
    void infoChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    void connectWindow(QQuickWindow* w);

    QString stats_, info_;
    std::mutex ringMu_;
    v6::SharedFrameRing* ring_ = nullptr;
    std::shared_ptr<PresentState> state_;
};
