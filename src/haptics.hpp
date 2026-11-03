#pragma once

#include "mods/api.h"

namespace ds {

ModResult haptics_init(ModError* error);
// Main thread, every frame: (re)opens the controller's audio device, applies rumble mode changes.
void haptics_update();
void haptics_shutdown();

}  // namespace ds
