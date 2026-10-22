#pragma once

#include "d3d.hpp"

namespace dc::capbench {

// Shows the moving test pattern on `monitor` (centred half-size window, or the whole monitor with full) until
// `seconds` passed (0 = until Esc / killed).
int runPattern(const MonitorInfo& monitor, bool full, double seconds);

}  // namespace dc::capbench
