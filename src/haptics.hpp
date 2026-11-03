#pragma once

#include "mods/api.h"

namespace ds {

// native_dir: where a shipped SDL3 library lives (Windows/macOS).
ModResult haptics_init(const char* native_dir, ModError* error);
// Main thread, every frame: (re)opens the controller's audio device, applies rumble mode changes.
void haptics_update();
void haptics_shutdown();
// Game rumble state (motor command on port 0) and whether the haptic audio device is open.
bool haptics_motor_on();
bool haptics_stream_open();

}  // namespace ds
