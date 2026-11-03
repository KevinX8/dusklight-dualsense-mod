// The game links SDL3 statically. On Linux its symbols are exported from the executable, so we
// resolve them from the host process at runtime. Windows/macOS do not export SDL, so gamepad
// effects are unavailable there; audio falls back to a separately loaded SDL3 library (its own
// instance, which is fine for opening the controller's audio device).
#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

namespace ds {

struct SdlApi {
    // Host instance only (the gamepad handle belongs to the game's SDL).
    decltype(&SDL_SendGamepadEffect) SendGamepadEffect = nullptr;

    // Audio: host instance, or a standalone SDL3 library when the host does not export it.
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

    bool audio_is_host = false;
    bool has_audio() const { return OpenAudioDeviceStream != nullptr; }
};

extern SdlApi sdl;

// Resolves everything it can. Returns a short description of where audio came from.
const char* sdl_load();
void sdl_unload();

}  // namespace ds
