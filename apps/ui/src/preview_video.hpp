#pragma once

#include "preview_stream.hpp"

#include <QQuickItem>
#include <QString>

#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

class QTimer;

namespace dc::ui {

class PreviewDecoder;

// The preview picture (video/preview from capd), letterboxed into the item. While the item is
// visible in a window it attaches to the PreviewStream, which subscribes; decoding and drawing as in
// P1/V6: decoder device -> shared BGRA textures -> copied into a texture of Qt's D3D11 device on the
// render thread. Needs the D3D11 scene graph (Windows); elsewhere it only says so in `error`.
class PreviewVideo : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(dc::ui::PreviewStream* stream READ stream WRITE setStream NOTIFY streamChanged)
    // A picture is on screen (hide the placeholder).
    Q_PROPERTY(bool showing READ showing NOTIFY stateChanged)
    // Why this screen cannot show the preview ("" = it can), in Japanese.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString decoderName READ decoderName NOTIFY stateChanged)
    // Size of the picture as decoded (0 until the first frame).
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY stateChanged)

public:
    explicit PreviewVideo(QQuickItem* parent = nullptr);
    ~PreviewVideo() override;

    PreviewStream* stream() const { return stream_; }
    void setStream(PreviewStream* s);
    bool showing() const { return showing_; }
    QString error() const { return error_; }
    QString decoderName() const { return decoderName_; }
    QSize frameSize() const { return frameSize_; }

signals:
    void streamChanged();
    void stateChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void follow();  // start / stop with visibility, window and stream
    void start();
    void stop();
    void poll();
    void setError(const QString& e);

    PreviewStream* stream_ = nullptr;
    int sink_ = 0;
    bool showing_ = false;
    QString error_, decoderName_;
    QSize frameSize_;

#ifdef _WIN32
    // Learnt on the render thread from Qt's device; the decoder runs on the same adapter.
    std::mutex adapterMutex_;
    bool haveAdapter_ = false;
    bool notD3D11_ = false;
    LUID adapter_{};

    std::mutex decoderMutex_;  // the sink (receive thread) against start/stop (GUI thread)
    std::shared_ptr<PreviewDecoder> decoder_;
    QTimer* poll_ = nullptr;
#endif
};

} // namespace dc::ui
