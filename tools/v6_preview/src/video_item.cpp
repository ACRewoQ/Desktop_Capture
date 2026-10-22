#include "video_item.hpp"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <algorithm>
#include <iostream>

namespace {

// Owns the display-device objects; created and deleted on the render thread by the scene graph.
class VideoNode : public QSGSimpleTextureNode {
public:
    VideoNode(v6::SharedFrameRing& ring, ID3D11Device* dev, QQuickWindow* w)
        : ring_(&ring), reader_(ring, dev), probe_(dev, ring.width(), ring.height()) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(ring.width());
        d.Height = static_cast<UINT>(ring.height());
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        V6_CHECK(dev->CreateTexture2D(&d, nullptr, &local_));
        setTexture(QNativeInterface::QSGD3D11Texture::fromNative(local_.Get(), w, QSize(ring.width(), ring.height())));
        setOwnsTexture(true);
        setFiltering(QSGTexture::Linear);
    }

    v6::SharedFrameRing* ring() const { return ring_; }

    // Copies a newer frame into the displayed texture, if there is one.
    bool fetch(ID3D11DeviceContext* ctx, std::uint64_t* id) {
        if (!reader_.fetch(ctx, local_.Get(), id)) return false;
        haveFrame_ = true;
        return true;
    }
    bool haveFrame() const { return haveFrame_; }
    v6::FrameProbe& probe() { return probe_; }
    ID3D11Texture2D* local() const { return local_.Get(); }

private:
    v6::SharedFrameRing* ring_;
    v6::SharedFrameReader reader_;
    v6::FrameProbe probe_;
    v6::ComPtr<ID3D11Texture2D> local_;
    bool haveFrame_ = false;
};

}  // namespace

VideoItem::VideoItem(QQuickItem* parent) : QQuickItem(parent), state_(std::make_shared<PresentState>()) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, &VideoItem::connectWindow);
}

VideoItem::~VideoItem() { detach(); }

void VideoItem::connectWindow(QQuickWindow* w) {
    if (!w) return;
    // Context object = the window, state captured by value: safe even while this item is being destroyed.
    connect(
        w, &QQuickWindow::frameSwapped, w,
        [st = state_] {
            st->nSwap.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(st->mu);
            if (st->pending.empty() || !st->stats) {
                st->pending.clear();
                return;
            }
            const std::int64_t now = v6::nowUs();
            for (std::uint64_t id : st->pending) {
                st->stats->mark(id, v6::Presented, now);
                if (!st->ack) continue;
                auto d = [&](v6::Stage a, v6::Stage b) {
                    const std::int64_t ta = st->stats->stamp(id, a), tb = st->stats->stamp(id, b);
                    return (ta >= 0 && tb >= 0) ? tb - ta : -1;
                };
                v6::AckTimes t;
                t.id = id;
                t.decode = d(v6::Received, v6::Decoded);
                t.convert = d(v6::Decoded, v6::Converted);
                t.copy = d(v6::Converted, v6::Copied);
                t.present = d(v6::Copied, v6::Presented);
                st->ack(t);
            }
            st->pending.clear();
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
        std::lock_guard<std::mutex> lock(ringMu_);
        ring_ = ring;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->stats = stats;
        state_->ack = std::move(ack);
    }
    update();
}

void VideoItem::detach() {
    {
        std::lock_guard<std::mutex> lock(ringMu_);
        ring_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stats = nullptr;
    state_->ack = nullptr;
    state_->pending.clear();
}

std::string VideoItem::counters() const {
    const auto& s = *state_;
    return "render path: updatePaintNode " + std::to_string(s.nPaint.load()) + ", fetch " +
           std::to_string(s.nFetchOk.load()) + "/" + std::to_string(s.nFetchTry.load()) + ", frameSwapped " +
           std::to_string(s.nSwap.load());
}

std::string VideoItem::probeText() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->probeText.empty() ? "display-side pixel check: none yet" : state_->probeText;
}

QSGNode* VideoItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto* node = static_cast<VideoNode*>(old);
    state_->nPaint.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(ringMu_);
    if (!ring_) return node;

    QQuickWindow* w = window();
    QSGRendererInterface* ri = w->rendererInterface();
    if (ri->graphicsApi() != QSGRendererInterface::Direct3D11) {
        std::cerr << "scene graph is not using Direct3D 11\n";
        return node;
    }
    auto* dev = static_cast<ID3D11Device*>(ri->getResource(w, QSGRendererInterface::DeviceResource));
    auto* ctx = static_cast<ID3D11DeviceContext*>(ri->getResource(w, QSGRendererInterface::DeviceContextResource));

    if (!node || node->ring() != ring_) {
        delete node;
        node = nullptr;
        try {
            node = new VideoNode(*ring_, dev, w);
        } catch (const std::exception& e) {
            std::cerr << "display side setup failed: " << e.what() << "\n";
            return nullptr;
        }
    }

    std::uint64_t id = 0;
    state_->nFetchTry.fetch_add(1, std::memory_order_relaxed);
    if (node->fetch(ctx, &id)) {
        const std::uint64_t n = state_->nFetchOk.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> slock(state_->mu);
        if (state_->stats) state_->stats->mark(id, v6::Copied);
        state_->pending.push_back(id);
        if (n % 60 == 30) {
            v6::FrameProbe& p = node->probe();
            p.check(ctx, node->local(), id, nullptr);
            state_->probeText = "display-side pixel check: " + std::to_string(p.ok()) + " ok, " +
                                std::to_string(p.bad()) + " bad; last: " + p.lastDetail();
        }
    }

    // Letterbox into the item.
    const qreal sw = ring_->width(), sh = ring_->height();
    const qreal scale = std::min(width() / sw, height() / sh);
    const QSizeF size(sw * scale, sh * scale);
    node->setRect(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2),
                         node->haveFrame() ? size : QSizeF(0, 0)));
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
