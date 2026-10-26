#include "preview_video.hpp"

#include "preview_stream.hpp"

#include "dc/lanmedia/udp_transport.hpp"

#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include "preview_decoder.hpp"

#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <rhi/qrhi.h>

#include <algorithm>
#include <iostream>
#endif

namespace dc::ui {

#ifdef _WIN32
namespace {

// Owns the objects on Qt's device; created and deleted on the render thread by the scene graph. The
// displayed texture belongs to Qt (a QRhiTexture inside the QSGTexture); the newest decoded frame is
// copied into its native texture. Member order: the ring (and with it the decode device that created
// the shared textures) is released after the reader's opened copies of them (P1/V6: the other way
// round crashed in the D3D11 runtime), and Qt's device after everything else.
class PreviewNode : public QSGSimpleTextureNode {
public:
    PreviewNode(std::shared_ptr<v6::SharedFrameRing> ring, ID3D11Device* dev, QRhi* rhi, QQuickWindow* w)
        : device_(dev), ring_(std::move(ring)), reader_(*ring_, dev) {
        std::unique_ptr<QRhiTexture> t(rhi->newTexture(QRhiTexture::BGRA8, QSize(ring_->width(), ring_->height()), 1,
                                                       QRhiTexture::UsedAsTransferSource));
        if (!t->create()) throw std::runtime_error("QRhiTexture::create failed");
        local_ = reinterpret_cast<ID3D11Texture2D*>(t->nativeTexture().object);
        setTexture(w->createTextureFromRhiTexture(t.release()));  // takes the QRhiTexture
        setOwnsTexture(false);
        setFiltering(QSGTexture::Linear);
    }
    ~PreviewNode() override { delete texture(); }

    const v6::SharedFrameRing* ring() const { return ring_.get(); }
    // The id of the frame copied now, 0 if there was no new one.
    std::uint64_t fetch(ID3D11DeviceContext* ctx) {
        std::uint64_t id = 0;
        if (!reader_.fetch(ctx, local_, &id)) return 0;
        haveFrame_ = true;
        return id;
    }
    bool haveFrame() const { return haveFrame_; }

private:
    v6::ComPtr<ID3D11Device> device_;           // destroyed last
    std::shared_ptr<v6::SharedFrameRing> ring_;  // after the reader
    v6::SharedFrameReader reader_;
    ID3D11Texture2D* local_ = nullptr;  // owned by the QSGTexture's QRhiTexture
    bool haveFrame_ = false;
};

} // namespace
#endif

PreviewVideo::PreviewVideo(QQuickItem* parent) : QQuickItem(parent) {
#ifdef _WIN32
    setFlag(ItemHasContents, true);
    poll_ = new QTimer(this);
    poll_->setInterval(250);
    connect(poll_, &QTimer::timeout, this, &PreviewVideo::poll);
#endif
}

PreviewVideo::~PreviewVideo() {
    if (sink_ && stream_) stream_->detach(sink_);
#ifdef _WIN32
    std::lock_guard lock(decoderMutex_);
    decoder_.reset();  // joins the decode thread; the node keeps the ring it shows
#endif
}

void PreviewVideo::setStream(PreviewStream* s) {
    if (s == stream_) return;
    stop();
    stream_ = s;
    emit streamChanged();
    follow();
}

void PreviewVideo::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        QMetaObject::invokeMethod(this, &PreviewVideo::follow, Qt::QueuedConnection);
}

void PreviewVideo::setError(const QString& e) {
    if (e == error_) return;
    error_ = e;
    emit stateChanged();
}

void PreviewVideo::follow() {
    const bool want = stream_ && window() && isVisible();
    if (!want) {
        stop();
        return;
    }
    if (sink_) return;
#ifdef _WIN32
    {
        std::lock_guard lock(adapterMutex_);
        if (notD3D11_) {
            setError(QStringLiteral("この描画方式では映像を表示できません（--backend d3d11 で起動してください）"));
            return;
        }
        if (!haveAdapter_) {
            update();  // updatePaintNode learns the adapter and calls follow() again
            return;
        }
    }
    start();
#else
    setError(QStringLiteral("この環境では映像を表示できません（Windows 版の dc-ui で表示します）"));
#endif
}

void PreviewVideo::start() {
#ifdef _WIN32
    LUID adapter;
    {
        std::lock_guard lock(adapterMutex_);
        adapter = adapter_;
    }
    {
        std::lock_guard lock(decoderMutex_);
        decoder_ = std::make_shared<PreviewDecoder>(
            adapter, [this] { QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection); });
    }
    PreviewStream* stream = stream_;
    sink_ = stream_->attach([this, stream](const PreviewStream::Frame& f) {
        const double captured = stream->toLocalTime(f.captureTime);
        std::lock_guard lock(decoderMutex_);
        if (decoder_) decoder_->push(f.data, f.keyframe, f.number, captured);
    });
    setError({});
    poll_->start();
#endif
}

void PreviewVideo::stop() {
    if (sink_ && stream_) stream_->detach(sink_);
    sink_ = 0;
#ifdef _WIN32
    poll_->stop();
    std::shared_ptr<PreviewDecoder> old;
    {
        std::lock_guard lock(decoderMutex_);
        old = std::move(decoder_);
    }
    old.reset();  // joins the decode thread
    update();     // drops the node
#endif
    {
        std::lock_guard lock(latencyMutex_);
        latencyWindow_.clear();
    }
    if (latencyMs_ >= 0) {
        latencyMs_ = latencyP95Ms_ = -1;
        emit latencyChanged();
    }
    const bool changed = showing_ || !frameSize_.isEmpty() || !decoderName_.isEmpty();
    showing_ = false;
    frameSize_ = {};
    decoderName_.clear();
    if (changed) emit stateChanged();
}

void PreviewVideo::poll() {
#ifdef _WIN32
    std::shared_ptr<PreviewDecoder> dec;
    {
        std::lock_guard lock(decoderMutex_);
        dec = decoder_;
    }
    if (!dec) return;
    const auto ring = dec->ring();
    const bool showing = ring && dec->decodedFrames() > 0;
    const QSize size = ring ? QSize(ring->width(), ring->height()) : QSize();
    const QString name = QString::fromStdString(dec->decoderName());
    const std::string err = dec->error();
    if (!err.empty()) setError(QStringLiteral("映像を表示できません（%1）").arg(QString::fromLocal8Bit(err.c_str())));
    if (showing != showing_ || size != frameSize_ || name != decoderName_) {
        showing_ = showing;
        frameSize_ = size;
        decoderName_ = name;
        emit stateChanged();
    }
    std::vector<double> w;
    {
        std::lock_guard lock(latencyMutex_);
        w = latencyWindow_;
    }
    double med = -1, p95 = -1;
    if (!w.empty()) {
        std::sort(w.begin(), w.end());
        med = w[w.size() / 2];
        p95 = w[std::min(w.size() - 1, w.size() * 95 / 100)];
    }
    if (med != latencyMs_ || p95 != latencyP95Ms_) {
        latencyMs_ = med;
        latencyP95Ms_ = p95;
        emit latencyChanged();
    }
#endif
}

std::vector<double> PreviewVideo::takeLatencySamples() {
    std::lock_guard lock(latencyMutex_);
    std::vector<double> all;
    all.swap(latencyAll_);
    return all;
}

QSGNode* PreviewVideo::updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) {
#ifdef _WIN32
    // Render thread, GUI thread blocked. Qt does not drop a paint node that is not returned again: it
    // stays in the tree (and kept drawing the old picture after a size change), so delete it here.
    Q_UNUSED(data);
    auto* node = static_cast<PreviewNode*>(old);
    const auto none = [&]() -> QSGNode* {
        delete node;
        return nullptr;
    };
    QQuickWindow* w = window();
    QSGRendererInterface* ri = w ? w->rendererInterface() : nullptr;
    if (!ri || ri->graphicsApi() != QSGRendererInterface::Direct3D11) {
        {
            std::lock_guard lock(adapterMutex_);
            notD3D11_ = true;
        }
        QMetaObject::invokeMethod(this, &PreviewVideo::follow, Qt::QueuedConnection);
        return none();
    }
    auto* dev = static_cast<ID3D11Device*>(ri->getResource(w, QSGRendererInterface::DeviceResource));
    auto* ctx = static_cast<ID3D11DeviceContext*>(ri->getResource(w, QSGRendererInterface::DeviceContextResource));
    auto* rhi = static_cast<QRhi*>(ri->getResource(w, QSGRendererInterface::RhiResource));
    if (!dev || !ctx || !rhi) return none();
    {
        std::lock_guard lock(adapterMutex_);
        if (!haveAdapter_) {
            adapter_ = v6::adapterLuidOf(dev);
            haveAdapter_ = true;
            QMetaObject::invokeMethod(this, &PreviewVideo::follow, Qt::QueuedConnection);
        }
    }

    std::shared_ptr<v6::SharedFrameRing> ring;
    {
        std::lock_guard lock(decoderMutex_);
        if (decoder_) ring = decoder_->ring();
    }
    if (!ring) return none();

    if (!node || node->ring() != ring.get()) {
        delete node;  // a new size: a new ring
        node = nullptr;
        try {
            node = new PreviewNode(ring, dev, rhi, w);
        } catch (const std::exception& e) {
            std::cerr << "preview: display side setup failed: " << e.what() << std::endl;
            return nullptr;
        }
    }
    if (const std::uint64_t id = node->fetch(ctx)) {
        double captured = std::nan("");
        {
            std::lock_guard lock(decoderMutex_);
            if (decoder_) captured = decoder_->captureTimeOf(id);
        }
        if (std::isfinite(captured)) {
            const double ms = (lan::nowSeconds() - captured) * 1000.0;
            std::lock_guard lock(latencyMutex_);
            if (latencyWindow_.size() >= 90) latencyWindow_.erase(latencyWindow_.begin());
            latencyWindow_.push_back(ms);
            if (latencyAll_.size() < 100000) latencyAll_.push_back(ms);
        }
    }

    // Letterbox into the item.
    const qreal sw = ring->width(), sh = ring->height();
    const qreal scale = std::min(width() / sw, height() / sh);
    const QSizeF size(sw * scale, sh * scale);
    node->setRect(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2),
                         node->haveFrame() ? size : QSizeF(0, 0)));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
#else
    return QQuickItem::updatePaintNode(old, data);
#endif
}

} // namespace dc::ui
