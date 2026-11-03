// Exercises the private-instance path (the Windows/macOS route) on any OS: this executable does not
// export SDL, so sdl_load must fall back to a standalone SDL3 library and set up a gamepad instance.
#undef NDEBUG
#include "sdl_dyn.hpp"

#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    const char* source = ds::sdl_load(argc > 1 ? argv[1] : nullptr);
    std::printf("sdl_load: %s\n", source);
    assert(ds::sdl.own_gamepad);
    assert(ds::sdl.has_audio());

    int count = 0;
    SDL_JoystickID* pads = ds::sdl.GetGamepads(&count);
    std::printf("gamepads visible to the private instance: %d\n", count);
    for (int i = 0; i < count; ++i) {
        std::printf("  %04x:%04x\n", ds::sdl.GetGamepadVendorForID(pads[i]),
            ds::sdl.GetGamepadProductForID(pads[i]));
    }
    ds::sdl.Free(pads);

    SDL_AudioDeviceID* devs = ds::sdl.GetAudioPlaybackDevices(&count);
    std::printf("audio playback devices: %d\n", count);
    ds::sdl.Free(devs);

    ds::sdl_unload();
    assert(!ds::sdl.own_gamepad);
    std::puts("sdl_dyn_test: all checks passed");
    return 0;
}
