#pragma once

#include "mods/api.h"

namespace ds {

// Lightbar, player LEDs, mic LED and adaptive triggers. Main thread.
ModResult controller_init(ModError* error);
void controller_update();
void controller_shutdown();

}  // namespace ds
