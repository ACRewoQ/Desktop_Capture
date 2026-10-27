#pragma once

#include <QString>

#include <functional>

class QQuickWindow;

namespace dc::ui {

// Synthetic touch on the mixer page (dc-ui --selftest): two faders dragged at once, fine mode after
// holding still, pinch to change the strip width, and a drag through an open sheet (touch and mouse)
// that must not reach the fader under it. Needs at least two tracks. Returns the failures.
int runTouchCheck(QQuickWindow* window, const std::function<void(const QString&)>& log);

} // namespace dc::ui
