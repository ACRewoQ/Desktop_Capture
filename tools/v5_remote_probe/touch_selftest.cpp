#include "touch_selftest.hpp"

#include "probe.hpp"
#include "strip_model.hpp"

#include <QQuickItem>
#include <QQuickWindow>

#include <cmath>

#ifdef _WIN32
#include <windows.h>
#else
#include <QTest>
#endif

namespace {
constexpr int kFrameMs = 16;
}

bool TouchSelfTest::prepare() {
#ifdef _WIN32
    return InitializeTouchInjection(kMaxContacts, TOUCH_FEEDBACK_NONE) != FALSE;
#else
    return true; // QTest touch events (development check of the QML logic only)
#endif
}

TouchSelfTest::TouchSelfTest(QQuickWindow* window, StripModel* model, Probe* probe, QObject* parent)
    : QObject(parent), window_(window), model_(model), probe_(probe) {
    frameTimer_.setInterval(kFrameMs);
    connect(&frameTimer_, &QTimer::timeout, this, &TouchSelfTest::nextFrame);
}

QQuickItem* TouchSelfTest::findItem(const QString& objectName) const {
    std::vector<QQuickItem*> stack{window_->contentItem()};
    while (!stack.empty()) {
        QQuickItem* item = stack.back();
        stack.pop_back();
        if (item->objectName() == objectName) return item;
        for (QQuickItem* child : item->childItems()) stack.push_back(child);
    }
    return nullptr;
}

QPointF TouchSelfTest::centerOf(QQuickItem* item) const {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
}

void TouchSelfTest::start() {
    window_->raise();
    window_->requestActivate();
    buildSteps();
    step_ = 0;
    runStep();
}

void TouchSelfTest::buildSteps() {
    // 1. Simultaneous drags: finger i moves fader i down by (i + 1) / K of half the fader height.
    steps_.push_back({});
    steps_.back().before = [this] {
        Step& self = steps_[step_];
        std::vector<QPointF> starts;
        for (int row = 0; row < model_->count() && static_cast<int>(starts.size()) < kMaxContacts; ++row) {
            QQuickItem* t = findItem(QStringLiteral("faderThumb_%1").arg(row));
            if (!t || !t->isVisible()) break;
            const QPointF c = centerOf(t);
            if (c.x() < 0 || c.x() > window_->width()) break;
            starts.push_back(c);
        }
        QQuickItem* fader0 = findItem(QStringLiteral("fader_0"));
        const int k = static_cast<int>(starts.size());
        const double travel = fader0 ? fader0->height() * 0.5 : 150.0;
        constexpr int kMoves = 30;
        for (int f = 0; f <= kMoves + 1; ++f) {
            Frame frame;
            for (int i = 0; i < k; ++i) {
                const double dy = travel * (i + 1) / k * std::min(f, kMoves) / kMoves;
                const auto phase = f == 0 ? Contact::Down : (f == kMoves + 1 ? Contact::Up : Contact::Move);
                frame.push_back({i, starts[static_cast<std::size_t>(i)] + QPointF(0, dy), phase});
            }
            self.frames.push_back(frame);
        }
        probe_->resetMaxSimultaneous();
        self.check = [this, k] {
            QJsonArray after;
            bool moved = true, ordered = true;
            double prev = 1e9;
            for (int i = 0; i < k; ++i) {
                const double db = model_->volumeDb(i);
                after.append(db);
                moved = moved && db < -1.0;
                ordered = ordered && db < prev;
                prev = db;
            }
            const int maxSim = probe_->maxSimultaneousFaders();
            addResult(QStringLiteral("simultaneous_drag"), k >= 2 && moved && ordered && maxSim >= k,
                      {{"fingers", k}, {"max_simultaneous_faders", maxSim}, {"all_moved", moved},
                       {"ordered_by_distance", ordered}, {"volume_db_after", after}});
        };
    };

    // 2. Double tap on fader 0 resets it to 0 dB.
    steps_.push_back({});
    steps_.back().settleMs = 400;
    steps_.back().before = [this] {
        Step& self = steps_[step_];
        QQuickItem* t = findItem(QStringLiteral("faderThumb_0"));
        if (!t) return;
        const QPointF p = centerOf(t);
        Frame down{{0, p, Contact::Down}}, up{{0, p, Contact::Up}}, idle;
        self.frames = {down, up, idle, idle, idle, down, up};
        self.check = [this] {
            const double db = model_->volumeDb(0);
            addResult(QStringLiteral("double_tap_reset"), std::abs(db) < 0.01, {{"volume_db_after", db}});
        };
    };

    // 3. Horizontal swipe starting on a fader: the list scrolls, no fader value changes.
    steps_.push_back({});
    steps_.back().settleMs = 800;
    steps_.back().before = [this] {
        Step& self = steps_[step_];
        QQuickItem* list = findItem(QStringLiteral("stripList"));
        QQuickItem* fader = findItem(QStringLiteral("fader_2"));
        if (!list || !fader) return;
        auto volumes = std::make_shared<std::vector<double>>();
        for (int i = 0; i < model_->count(); ++i) volumes->push_back(model_->volumeDb(i));
        const double contentXBefore = list->property("contentX").toDouble();
        const QPointF start = centerOf(fader);
        const double distance = -window_->width() * 0.5;
        constexpr int kMoves = 20;
        for (int f = 0; f <= kMoves + 1; ++f) {
            const double dx = distance * std::min(f, kMoves) / kMoves;
            self.frames.push_back(
                {{0, start + QPointF(dx, 0), f == 0 ? Contact::Down : (f == kMoves + 1 ? Contact::Up : Contact::Move)}});
        }
        const bool scrollable = list->property("contentWidth").toDouble() > list->width() + 1;
        self.check = [this, list, volumes, contentXBefore, scrollable] {
            const double contentX = list->property("contentX").toDouble();
            bool unchanged = true;
            for (int i = 0; i < model_->count() && i < static_cast<int>(volumes->size()); ++i)
                unchanged = unchanged && model_->volumeDb(i) == (*volumes)[static_cast<std::size_t>(i)];
            const bool scrolled = contentX > contentXBefore + 10;
            addResult(QStringLiteral("swipe_scrolls_not_faders"), unchanged && (scrolled || !scrollable),
                      {{"scrollable", scrollable}, {"content_x_before", contentXBefore}, {"content_x_after", contentX},
                       {"faders_unchanged", unchanged}});
        };
    };
}

// Injected touch goes to whatever window is under the point, so never inject while another
// window is in front of ours.
bool TouchSelfTest::isForeground() const {
#ifdef _WIN32
    return GetForegroundWindow() == reinterpret_cast<HWND>(window_->winId());
#else
    return true;
#endif
}

void TouchSelfTest::runStep() {
    if (!isForeground()) {
        emit finished({{"supported", true}, {"pass", false}, {"reason", "probe window is not in the foreground"},
                       {"tests", results_}});
        return;
    }
    if (step_ >= steps_.size()) {
        emit finished({{"supported", true}, {"pass", allPass_ && injectErrors_ == 0}, {"inject_errors", injectErrors_},
                       {"tests", results_}});
        return;
    }
    Step& s = steps_[step_];
    if (s.before) s.before();
    frame_ = 0;
    frameTimer_.start();
}

void TouchSelfTest::nextFrame() {
    Step& s = steps_[step_];
    if (frame_ < s.frames.size()) {
        if (!isForeground()) {
            frameTimer_.stop();
            emit finished({{"supported", true}, {"pass", false}, {"reason", "probe window lost the foreground"},
                           {"tests", results_}});
            return;
        }
        if (!s.frames[frame_].empty() && !inject(s.frames[frame_])) ++injectErrors_;
        ++frame_;
        return;
    }
    frameTimer_.stop();
    QTimer::singleShot(s.settleMs, this, [this] {
        Step& done = steps_[step_];
        if (done.check) done.check();
        else addResult(QStringLiteral("step_%1").arg(step_ + 1), false, {{"reason", "items not found"}});
        ++step_;
        runStep();
    });
}

bool TouchSelfTest::inject(const Frame& frame) {
#ifdef _WIN32
    const HWND hwnd = reinterpret_cast<HWND>(window_->winId());
    const double dpr = window_->devicePixelRatio();
    std::vector<POINTER_TOUCH_INFO> contacts(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        POINT pt{static_cast<LONG>(std::lround(frame[i].scenePos.x() * dpr)),
                 static_cast<LONG>(std::lround(frame[i].scenePos.y() * dpr))};
        ClientToScreen(hwnd, &pt);
        POINTER_TOUCH_INFO& c = contacts[i];
        c = {};
        c.pointerInfo.pointerType = PT_TOUCH;
        c.pointerInfo.pointerId = static_cast<UINT32>(frame[i].id);
        c.pointerInfo.ptPixelLocation = pt;
        switch (frame[i].phase) {
        case Contact::Down: c.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT; break;
        case Contact::Move: c.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT; break;
        case Contact::Up: c.pointerInfo.pointerFlags = POINTER_FLAG_UP; break;
        }
        c.touchFlags = TOUCH_FLAG_NONE;
        c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        c.orientation = 90;
        c.pressure = 32000;
        c.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
    }
    return InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data()) != FALSE;
#else
    static QPointingDevice* device = QTest::createTouchDevice();
    auto seq = QTest::touchEvent(window_, device);
    for (const Contact& c : frame) {
        const QPoint p = c.scenePos.toPoint();
        switch (c.phase) {
        case Contact::Down: seq.press(c.id, p, window_); break;
        case Contact::Move: seq.move(c.id, p, window_); break;
        case Contact::Up: seq.release(c.id, p, window_); break;
        }
    }
    return seq.commit();
#endif
}

void TouchSelfTest::addResult(const QString& name, bool pass, QJsonObject details) {
    details.insert(QStringLiteral("name"), name);
    details.insert(QStringLiteral("pass"), pass);
    results_.append(details);
    allPass_ = allPass_ && pass;
}
