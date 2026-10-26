#pragma once
// The tray icon, drawn at run time (no image files): three faders on a dark square, the knobs coloured by how the
// engine and the capture process are doing.

#include "tray_core.hpp"

#include <windows.h>

namespace dc::tray {

HICON makeTrayIcon(Health health, int size);

} // namespace dc::tray
