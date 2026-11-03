#include "sdl_dyn.hpp"

#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ds {

SdlApi sdl;

namespace {

void* g_lib = nullptr;

void* host_sym(const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name));
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

void* lib_sym(const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(g_lib), name));
#else
    return dlsym(g_lib, name);
#endif
}

void* open_one(const std::string& path) {
#ifdef _WIN32
    return LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

// Prefer the copy shipped beside the mod, then a system one.
void* open_lib(const char* native_dir) {
#if defined(_WIN32)
    const char* names[] = {"SDL3.dll"};
    const char sep = '\\';
#elif defined(__APPLE__)
    const char* names[] = {"libSDL3.0.dylib", "libSDL3.dylib"};
    const char sep = '/';
#else
    const char* names[] = {"libSDL3.so.0", "libSDL3.so"};
    const char sep = '/';
#endif
    for (const char* name : names) {
        if (native_dir != nullptr && *native_dir != '\0') {
            if (void* lib = open_one(std::string(native_dir) + sep + name)) {
                return lib;
            }
        }
    }
    for (const char* name : names) {
        if (void* lib = open_one(name)) {
            return lib;
        }
    }
    return nullptr;
}

#define DS_LOAD(field, name) sdl.field = reinterpret_cast<decltype(sdl.field)>(sym(name))

template <class Fn>
bool load_audio(Fn&& sym) {
    DS_LOAD(InitSubSystem, "SDL_InitSubSystem");
    DS_LOAD(QuitSubSystem, "SDL_QuitSubSystem");
    DS_LOAD(GetAudioPlaybackDevices, "SDL_GetAudioPlaybackDevices");
    DS_LOAD(GetAudioDeviceName, "SDL_GetAudioDeviceName");
    DS_LOAD(GetAudioDeviceFormat, "SDL_GetAudioDeviceFormat");
    DS_LOAD(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream");
    DS_LOAD(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice");
    DS_LOAD(PutAudioStreamData, "SDL_PutAudioStreamData");
    DS_LOAD(GetAudioStreamQueued, "SDL_GetAudioStreamQueued");
    DS_LOAD(ClearAudioStream, "SDL_ClearAudioStream");
    DS_LOAD(DestroyAudioStream, "SDL_DestroyAudioStream");
    DS_LOAD(Free, "SDL_free");
    const bool ok = sdl.InitSubSystem && sdl.QuitSubSystem && sdl.GetAudioPlaybackDevices &&
                    sdl.GetAudioDeviceName && sdl.GetAudioDeviceFormat &&
                    sdl.OpenAudioDeviceStream && sdl.ResumeAudioStreamDevice &&
                    sdl.PutAudioStreamData && sdl.GetAudioStreamQueued && sdl.ClearAudioStream &&
                    sdl.DestroyAudioStream && sdl.Free;
    if (!ok) {
        sdl.OpenAudioDeviceStream = nullptr;
    }
    return ok;
}

template <class Fn>
bool load_gamepad(Fn&& sym) {
    DS_LOAD(SendGamepadEffect, "SDL_SendGamepadEffect");
    DS_LOAD(Quit, "SDL_Quit");
    DS_LOAD(SetHint, "SDL_SetHint");
    DS_LOAD(GetGamepads, "SDL_GetGamepads");
    DS_LOAD(GetGamepadVendorForID, "SDL_GetGamepadVendorForID");
    DS_LOAD(GetGamepadProductForID, "SDL_GetGamepadProductForID");
    DS_LOAD(OpenGamepad, "SDL_OpenGamepad");
    DS_LOAD(CloseGamepad, "SDL_CloseGamepad");
    DS_LOAD(GamepadConnected, "SDL_GamepadConnected");
    DS_LOAD(UpdateGamepads, "SDL_UpdateGamepads");
    DS_LOAD(RumbleGamepad, "SDL_RumbleGamepad");
    const bool ok = sdl.SendGamepadEffect && sdl.Quit && sdl.SetHint && sdl.GetGamepads &&
                    sdl.GetGamepadVendorForID && sdl.GetGamepadProductForID && sdl.OpenGamepad &&
                    sdl.CloseGamepad && sdl.GamepadConnected && sdl.UpdateGamepads &&
                    sdl.RumbleGamepad && sdl.InitSubSystem && sdl.Free;
    if (!ok) {
        sdl.SendGamepadEffect = nullptr;
    }
    return ok;
}

#undef DS_LOAD

// Our instance must only touch the DualSense: HIDAPI PS5 driver on, every other backend and
// HIDAPI driver off (they would open, and sometimes re-initialize, the game's other controllers).
void configure_private_instance() {
    const char* off[] = {
        SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_WGI, SDL_HINT_XINPUT_ENABLED,
        SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_MFI,
        SDL_HINT_JOYSTICK_IOKIT, SDL_HINT_JOYSTICK_HIDAPI_PS3, SDL_HINT_JOYSTICK_HIDAPI_PS4,
        SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
        SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE,
        SDL_HINT_JOYSTICK_HIDAPI_WII, SDL_HINT_JOYSTICK_HIDAPI_STEAM,
        SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, SDL_HINT_JOYSTICK_HIDAPI_LUNA,
        SDL_HINT_JOYSTICK_HIDAPI_SHIELD, SDL_HINT_JOYSTICK_HIDAPI_STADIA,
        SDL_HINT_JOYSTICK_HIDAPI_8BITDO, SDL_HINT_JOYSTICK_HIDAPI_FLYDIGI,
        SDL_HINT_JOYSTICK_HIDAPI_SINPUT, SDL_HINT_JOYSTICK_HIDAPI_ZUIKI,
        SDL_HINT_JOYSTICK_HIDAPI_STEAM_HORI, SDL_HINT_JOYSTICK_HIDAPI_GIP,
        SDL_HINT_JOYSTICK_HIDAPI_SWITCH2, SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC,
        SDL_HINT_JOYSTICK_HIDAPI_LG4FF,
        // We set the player LEDs ourselves.
        SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED,
    };
    for (const char* hint : off) {
        sdl.SetHint(hint, "0");
    }
    sdl.SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    sdl.SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    // We have no window; keep the device alive while the game window is unfocused.
    sdl.SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

}  // namespace

const char* sdl_load(const char* native_dir) {
    sdl = {};
    sdl.SendGamepadEffect =
        reinterpret_cast<decltype(sdl.SendGamepadEffect)>(host_sym("SDL_SendGamepadEffect"));
    const bool hostGamepad = sdl.SendGamepadEffect != nullptr;
    if (hostGamepad && load_audio(host_sym)) {
        return "game SDL";
    }

    g_lib = open_lib(native_dir);
    if (g_lib == nullptr) {
        return hostGamepad ? "game SDL (no audio)" : "unavailable (no SDL3 library found)";
    }
    if (!load_audio(lib_sym) || !sdl.InitSubSystem(SDL_INIT_AUDIO)) {
        sdl.OpenAudioDeviceStream = nullptr;
    }
    if (hostGamepad) {
        return "game SDL gamepad, private SDL audio";
    }
    if (load_gamepad(lib_sym)) {
        configure_private_instance();
        if (sdl.InitSubSystem(SDL_INIT_GAMEPAD)) {
            sdl.own_gamepad = true;
            return "private SDL instance (audio + DualSense)";
        }
        sdl.SendGamepadEffect = nullptr;
    }
    return "private SDL audio only";
}

void sdl_unload() {
    if (g_lib != nullptr) {
        // SDL_Quit stops the private instance's threads (HIDAPI, audio) before we unload it.
        if (sdl.Quit != nullptr) {
            sdl.Quit();
        } else if (sdl.QuitSubSystem != nullptr) {
            sdl.QuitSubSystem(SDL_INIT_AUDIO);
        }
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(g_lib));
#else
        dlclose(g_lib);
#endif
        g_lib = nullptr;
    }
    sdl = {};
}

}  // namespace ds
