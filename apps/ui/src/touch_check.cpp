#include "touch_check.hpp"

#include "selftest.hpp"

#include <QCoreApplication>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <vector>

namespace dc::ui {

namespace {

// Synthetic fingers on the window: touch events as a touch screen sends them.
class Fingers {
public:
    explicit Fingers(QQuickWindow* window) : window_(window) {
        device_ = new QPointingDevice(QStringLiteral("dc-ui touch check"), 0x7d5c, QInputDevice::DeviceType::TouchScreen,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Area, 10, 0,
                                      QString(), QPointingDeviceUniqueId(), window);
        QWindowSystemInterface::registerInputDevice(device_);
    }
    // points: one position per finger (ids 0..n-1); all fingers go down, move or up together.
    void press(const std::vector<QPointF>& at) { send(QEventPoint::State::Pressed, at); }
    void move(const std::vector<QPointF>& at) { send(QEventPoint::State::Updated, at); }
    void release(const std::vector<QPointF>& at) { send(QEventPoint::State::Released, at); }
    // From a to b in steps, about 60 Hz.
    void glide(const std::vector<QPointF>& a, const std::vector<QPointF>& b, int steps = 20) {
        for (int s = 1; s <= steps; ++s) {
            std::vector<QPointF> p;
            for (std::size_t i = 0; i < a.size(); ++i) p.push_back(a[i] + (b[i] - a[i]) * (double(s) / steps));
            move(p);
            waitUntil([] { return false; }, 16);
        }
    }

private:
    // Through the window system interface, as the platform plugin delivers real touches.
    void send(QEventPoint::State state, const std::vector<QPointF>& at) {
        QList<QWindowSystemInterface::TouchPoint> points;
        for (std::size_t i = 0; i < at.size(); ++i) {
            QWindowSystemInterface::TouchPoint p;
            p.id = int(i);
            p.state = state;
            p.pressure = state == QEventPoint::State::Released ? 0 : 1;
            p.area = QRectF(0, 0, 4, 4);
            p.area.moveCenter(window_->mapToGlobal(at[i]));
            points << p;
        }
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(window_, device_, points);
        QCoreApplication::processEvents();
    }

    QQuickWindow* window_;
    QPointingDevice* device_ = nullptr;
};

// Delegates and loaded pages are visual children only, so search the item tree.
QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) return root;
    for (QQuickItem* c : root->childItems())
        if (QQuickItem* f = findItem(c, name)) return f;
    return nullptr;
}

QPointF centreOf(QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

} // namespace

int runTouchCheck(QQuickWindow* window, const std::function<void(const QString&)>& log) {
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("  OK  ") : QStringLiteral("  FAIL"), what));
        if (!ok) ++failures;
    };
    auto find = [&](const char* name) { return findItem(window->contentItem(), QString::fromLatin1(name)); };

    window->setProperty("currentPage", QStringLiteral("mixer"));
    waitUntil([] { return false; }, 300);
    QQuickItem* page = find("mixerPage");
    QQuickItem* f0 = find("fader_0");
    QQuickItem* f1 = find("fader_1");
    QQuickItem* strip0 = find("strip_0");
    if (!page || !f0 || !f1 || !strip0) {
        check(false, QStringLiteral("touch check: mixer with two tracks on screen"));
        return failures;
    }
    Fingers fingers(window);
    auto pos = [](QQuickItem* f) { return f->property("position").toDouble(); };
    auto settle = [] { waitUntil([] { return false; }, 150); };

    // Double tap: 0 dB (also a known start for the next checks).
    for (QQuickItem* f : {f0, f1}) {
        const QPointF p = centreOf(f);
        for (int t = 0; t < 2; ++t) {
            fingers.press({p});
            fingers.release({p});
            waitUntil([] { return false; }, 60);
        }
    }
    settle();
    const double zeroDb = f0->property("zeroDbPosition").toDouble();
    check(std::abs(pos(f0) - zeroDb) < 0.01 && std::abs(pos(f1) - zeroDb) < 0.01,
          QStringLiteral("double tap sets 0 dB (%1, %2)").arg(pos(f0), 0, 'f', 2).arg(pos(f1), 0, 'f', 2));

    // Two faders at once, in opposite directions: both move, the strips keep their width.
    const double width0 = strip0->width();
    const double a0 = pos(f0), b0 = pos(f1);
    const QPointF pa = centreOf(f0), pb = centreOf(f1);
    fingers.press({pa, pb});
    fingers.glide({pa, pb}, {pa + QPointF(0, 60), pb - QPointF(0, 30)});
    fingers.release({pa + QPointF(0, 60), pb - QPointF(0, 30)});
    settle();
    check(pos(f0) < a0 - 0.05 && pos(f1) > b0 + 0.03 && strip0->width() == width0,
          QStringLiteral("two faders at once (%1 -> %2, %3 -> %4, strip %5 px)")
              .arg(a0, 0, 'f', 2).arg(pos(f0), 0, 'f', 2).arg(b0, 0, 'f', 2).arg(pos(f1), 0, 'f', 2).arg(strip0->width()));

    // Fine mode: hold still, then the same move goes a fifth as far.
    const double c0 = pos(f0);
    const QPointF pc = centreOf(f0);
    fingers.press({pc});
    waitUntil([] { return false; }, 800);
    const bool armed = f0->property("fine").toBool();
    fingers.glide({pc}, {pc - QPointF(0, 100)});
    fingers.release({pc - QPointF(0, 100)});
    settle();
    const double fineMove = pos(f0) - c0;
    const double c1 = pos(f0);
    fingers.press({pc});
    fingers.glide({pc}, {pc + QPointF(0, 100)});
    fingers.release({pc + QPointF(0, 100)});
    settle();
    const double normalMove = c1 - pos(f0);
    check(armed && fineMove > 0 && normalMove > 0 && std::abs(fineMove / normalMove - 0.2) < 0.06,
          QStringLiteral("fine mode after holding still (moved %1 vs %2)").arg(fineMove, 0, 'f', 3).arg(normalMove, 0, 'f', 3));

    // Pinch sideways on the strips' names: wider, then narrower than at first.
    QQuickItem* list = find("trackList");
    const QPointF mid = list->mapToScene(QPointF(list->width() / 2, 40));
    fingers.press({mid - QPointF(40, 0), mid + QPointF(40, 0)});
    fingers.glide({mid - QPointF(40, 0), mid + QPointF(40, 0)}, {mid - QPointF(120, 0), mid + QPointF(120, 0)});
    fingers.release({mid - QPointF(120, 0), mid + QPointF(120, 0)});
    settle();
    const double wide = strip0->width();
    // and over the faders: sideways is a pinch there too, not a fader move
    const QPointF low = list->mapToScene(QPointF(list->width() / 2, list->height() * 0.6));
    fingers.press({low - QPointF(150, 0), low + QPointF(150, 0)});
    fingers.glide({low - QPointF(150, 0), low + QPointF(150, 0)}, {low - QPointF(30, 0), low + QPointF(30, 0)});
    fingers.release({low - QPointF(30, 0), low + QPointF(30, 0)});
    settle();
    const double narrow = strip0->width();
    check(wide > width0 * 1.3 && narrow < width0 && narrow >= 72,
          QStringLiteral("pinch changes the strip width (%1 -> %2 -> %3 px)").arg(width0).arg(wide).arg(narrow));
    page->setProperty("stripWidth", 108.0);
    return failures;
}

} // namespace dc::ui
