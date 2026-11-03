// SDL access. The game links SDL3 statically:
// - Linux exports its SDL symbols, so we use the game's own instance (and its gamepad handle).
// - Windows/macOS do not, so we load our own SDL3 library (shipped beside the mod) and run a
//   private instance that opens only the DualSense through HIDAPI. The game's own writes to the
//   controller (LED, rumble) are then blocked by hooks so the two instances never fight.
#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

namespace ds {

struct SdlApi {
    decltype(&SDL_SendGamepadEffect) SendGamepadEffect = nullptr;

    // Audio
    decltype(&SDL_InitSubSystem) InitSubSystem = nullptr;
    decltype(&SDL_QuitSubSystem) QuitSubSystem = nullptr;
    decltype(&SDL_GetAudioPlaybackDevices) GetAudioPlaybackDevices = nullptr;
    decltype(&SDL_GetAudioDeviceName) GetAudioDeviceName = nullptr;
    decltype(&SDL_GetAudioDeviceFormat) GetAudioDeviceFormat = nullptr;
    decltype(&SDL_OpenAudioDeviceStream) OpenAudioDeviceStream = nullptr;
    decltype(&SDL_ResumeAudioStreamDevice) ResumeAudioStreamDevice = nullptr;
    decltype(&SDL_PutAudioStreamData) PutAudioStreamData = nullptr;
    decltype(&SDL_GetAudioStreamQueued) GetAudioStreamQueued = nullptr;
    decltype(&SDL_ClearAudioStream) ClearAudioStream = nullptr;
    decltype(&SDL_DestroyAudioStream) DestroyAudioStream = nullptr;
    decltype(&SDL_free) Free = nullptr;

    // Private gamepad instance (own_gamepad only)
    decltype(&SDL_Quit) Quit = nullptr;
    decltype(&SDL_SetHint) SetHint = nullptr;
    decltype(&SDL_GetGamepads) GetGamepads = nullptr;
    decltype(&SDL_GetGamepadVendorForID) GetGamepadVendorForID = nullptr;
    decltype(&SDL_GetGamepadProductForID) GetGamepadProductForID = nullptr;
    decltype(&SDL_OpenGamepad) OpenGamepad = nullptr;
    decltype(&SDL_CloseGamepad) CloseGamepad = nullptr;
    decltype(&SDL_GamepadConnected) GamepadConnected = nullptr;
    decltype(&SDL_UpdateGamepads) UpdateGamepads = nullptr;
    decltype(&SDL_RumbleGamepad) RumbleGamepad = nullptr;

    bool own_gamepad = false;  // gamepad effects go through our private instance
    bool has_audio() const { return OpenAudioDeviceStream != nullptr; }
};

extern SdlApi sdl;

// native_dir: the mod's extracted lib directory (where a shipped SDL3 library lives).
// Returns a short description of what was loaded.
const char* sdl_load(const char* native_dir);
void sdl_unload();

}  // namespace ds
