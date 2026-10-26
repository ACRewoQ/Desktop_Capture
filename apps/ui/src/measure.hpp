#pragma once

#include <QString>

#include <functional>

class QQuickWindow;

namespace dc::ui {

class MonitorAudio;
class PreviewStream;

// dc-ui --measure SECONDS (G4 on the remote): shows the preview tab, waits for the picture, then for
// SECONDS samples the preview delay (composed by capd -> handed to the screen), the received frame rate,
// this process's memory and CPU, prints one line a second and a summary; then leaves the preview and
// measures the memory once more. Returns the number of failed G4 checks (delay p95 <= 200 ms, picture
// shown; memory is reported, the target is judged by the caller).
// With a monitor stream chosen, its state goes into each line and it must be playing at the end.
int runMeasure(QQuickWindow* window, PreviewStream& stream, MonitorAudio* monitor, double seconds,
               const std::function<void(const QString&)>& log);

} // namespace dc::ui
