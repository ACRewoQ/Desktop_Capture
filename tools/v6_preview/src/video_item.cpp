#include "video_item.hpp"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <algorithm>
#include <iostream>

VideoItem::VideoItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, &VideoItem::connectWindow);
}

VideoItem::~VideoItem() { detach(); }

void VideoItem::connectWindow(QQuickWindow* w) {
    if (!w) return;
    connect(w, &QQuickWindow::frameSwapped, this, &VideoItem::onFrameSwapped, Qt::DirectConnection);
    // The render thread may destroy the scene graph (e.g. device loss); drop D3D objects tied to it.
    connect(
        w, &QQuickWindow::sceneGraphInvalidated, this,
        [this] {
            reader_.reset();
            probe_.reset();
            local_.Reset();
            readerRing_ = nullptr;
        },
        Qt::DirectConnection);
}

void VideoItem::setStatsText(const QString& s) {
    if (s == stats_) return;
    stats_ = s;
    emit statsChanged();
}

void VideoItem::setInfoText(const QString& s) {
    if (s == info_) return;
    info_ = s;
    emit infoChanged();
}

QString VideoItem::clockText() const {
    const long long ms = (v6::nowUs() / 1000) % 1000000;
    return QStringLiteral("%1").arg(ms, 6, 10, QLatin1Char('0'));
}

void VideoItem::attach(v6::SharedFrameRing* ring, v6::Stats* stats, AckFn ack) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ring_ = ring;
        statsPtr_ = stats;
        ack_ = std::move(ack);
    }
    update();
}

void VideoItem::detach() {
    std::lock_guard<std::mutex> lock(mu_);
    ring_ = nullptr;
    statsPtr_ = nullptr;
    ack_ = nullptr;
}

QSGNode* VideoItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGSimpleTextureNode*>(old);
    nPaint_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    if (!ring_) return node;

    QQuickWindow* w = window();
    QSGRendererInterface* ri = w->rendererInterface();
    if (ri->graphicsApi() != QSGRendererInterface::Direct3D11) {
        std::cerr << "scene graph is not using Direct3D 11\n";
        return node;
    }
    auto* dev = static_cast<ID3D11Device*>(ri->getResource(w, QSGRendererInterface::DeviceResource));
    auto* ctx = static_cast<ID3D11DeviceContext*>(ri->getResource(w, QSGRendererInterface::DeviceContextResource));

    if (!reader_ || readerRing_ != ring_) {
        try {
            reader_ = std::make_unique<v6::SharedFrameReader>(*ring_, dev);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(ring_->width());
            d.Height = static_cast<UINT>(ring_->height());
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            local_.Reset();
            V6_CHECK(dev->CreateTexture2D(&d, nullptr, &local_));
            probe_ = std::make_unique<v6::FrameProbe>(dev, ring_->width(), ring_->height());
        } catch (const std::exception& e) {
            std::cerr << "display side setup failed: " << e.what() << "\n";
            reader_.reset();
            return node;
        }
        readerRing_ = ring_;
        haveFrame_ = false;
        delete node;
        node = nullptr;
    }

    std::uint64_t id = 0;
    nFetchTry_.fetch_add(1, std::memory_order_relaxed);
    if (reader_->fetch(ctx, local_.Get(), &id)) {
        nFetchOk_.fetch_add(1, std::memory_order_relaxed);
        haveFrame_ = true;
        if (statsPtr_) statsPtr_->mark(id, v6::Copied);
        pendingPresent_.push_back(id);
        if (probe_ && nFetchOk_.load(std::memory_order_relaxed) % 60 == 30) {
            probe_->check(ctx, local_.Get(), id, nullptr);
            std::lock_guard<std::mutex> plock(probeMu_);
            probeText_ = "display-side pixel check: " + std::to_string(probe_->ok()) + " ok, " +
                         std::to_string(probe_->bad()) + " bad; last: " + probe_->lastDetail();
        }
    }
    if (!haveFrame_) return node;

    if (!node) {
        node = new QSGSimpleTextureNode;
        QSGTexture* tex = QNativeInterface::QSGD3D11Texture::fromNative(
            local_.Get(), w, QSize(ring_->width(), ring_->height()));
        node->setTexture(tex);
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }
    // Letterbox into the item.
    const qreal sw = ring_->width(), sh = ring_->height();
    const qreal scale = std::min(width() / sw, height() / sh);
    const QSizeF size(sw * scale, sh * scale);
    node->setRect(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

std::string VideoItem::probeText() const {
    std::lock_guard<std::mutex> lock(probeMu_);
    return probeText_.empty() ? "display-side pixel check: none yet" : probeText_;
}

std::string VideoItem::counters() const {
    return "render path: updatePaintNode " + std::to_string(nPaint_.load()) + ", fetch " +
           std::to_string(nFetchOk_.load()) + "/" + std::to_string(nFetchTry_.load()) + ", frameSwapped " +
           std::to_string(nSwap_.load());
}

void VideoItem::onFrameSwapped() {
    nSwap_.fetch_add(1, std::memory_order_relaxed);
    if (pendingPresent_.empty()) return;
    const std::int64_t now = v6::nowUs();
    std::lock_guard<std::mutex> lock(mu_);
    for (std::uint64_t id : pendingPresent_) {
        if (!statsPtr_) break;
        statsPtr_->mark(id, v6::Presented, now);
        if (ack_) {
            auto d = [&](v6::Stage a, v6::Stage b) {
                const std::int64_t ta = statsPtr_->stamp(id, a), tb = statsPtr_->stamp(id, b);
                return (ta >= 0 && tb >= 0) ? tb - ta : -1;
            };
            v6::AckTimes t;
            t.id = id;
            t.decode = d(v6::Received, v6::Decoded);
            t.convert = d(v6::Decoded, v6::Converted);
            t.copy = d(v6::Converted, v6::Copied);
            t.present = d(v6::Copied, v6::Presented);
            ack_(t);
        }
    }
    pendingPresent_.clear();
}
