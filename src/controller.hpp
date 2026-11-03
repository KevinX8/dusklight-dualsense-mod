#pragma once

#include "mods/api.h"

namespace ds {

// Lightbar, player LEDs, mic LED and adaptive triggers. Main thread.
ModResult controller_init(ModError* error);
void controller_update();
void controller_shutdown();
// True when our private SDL instance drives the DualSense (Windows/macOS).
bool controller_owns_pad();

}  // namespace ds
