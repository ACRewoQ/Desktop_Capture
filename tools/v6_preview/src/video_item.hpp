// QQuickItem that shows the newest decoded frame from a SharedFrameRing.
//
// Render thread, per frame: SharedFrameReader copies the newest BGRA frame into a texture owned by Qt's
// D3D11 device, which a QSGSimpleTextureNode draws. frameSwapped then stamps Presented for that frame.
#pragma once

#include "d3d_video.hpp"
#include "pipeline.hpp"
#include "stats.hpp"

#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <string>
#include <memory>
#include <mutex>
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
    void detach();  // must run before the ring is destroyed

    // Render-path counters for diagnosing a preview that shows nothing.
    std::string counters() const;

signals:
    void statsChanged();
    void infoChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    void onFrameSwapped();  // render thread
    void connectWindow(QQuickWindow* w);

    QString stats_, info_;

    std::mutex mu_;  // guards ring_, stats_ptr_, ack_ against detach() from the GUI thread
    v6::SharedFrameRing* ring_ = nullptr;
    v6::Stats* statsPtr_ = nullptr;
    AckFn ack_;

    // Render thread only.
    v6::SharedFrameRing* readerRing_ = nullptr;
    std::unique_ptr<v6::SharedFrameReader> reader_;
    v6::ComPtr<ID3D11Texture2D> local_;
    std::vector<std::uint64_t> pendingPresent_;
    bool haveFrame_ = false;

    std::atomic<std::uint64_t> nPaint_{0}, nFetchTry_{0}, nFetchOk_{0}, nSwap_{0};
};
