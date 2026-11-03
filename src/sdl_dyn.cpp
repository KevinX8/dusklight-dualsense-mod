#include "sdl_dyn.hpp"

#ifdef _WIN32
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

void* open_lib() {
#if defined(_WIN32)
    return LoadLibraryW(L"SDL3.dll");
#elif defined(__APPLE__)
    void* lib = dlopen("libSDL3.0.dylib", RTLD_NOW | RTLD_LOCAL);
    return lib ? lib : dlopen("libSDL3.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    void* lib = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    return lib ? lib : dlopen("libSDL3.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

template <class Fn>
bool load_audio(Fn&& sym) {
#define DS_LOAD(field, name) sdl.field = reinterpret_cast<decltype(sdl.field)>(sym(name))
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
#undef DS_LOAD
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

}  // namespace

const char* sdl_load() {
    sdl = {};
    sdl.SendGamepadEffect =
        reinterpret_cast<decltype(sdl.SendGamepadEffect)>(host_sym("SDL_SendGamepadEffect"));
    if (load_audio(host_sym)) {
        sdl.audio_is_host = true;
        return "game SDL";
    }
    g_lib = open_lib();
    if (g_lib != nullptr && load_audio(lib_sym)) {
        // A separate SDL instance needs its own audio subsystem.
        if (sdl.InitSubSystem(SDL_INIT_AUDIO)) {
            return "standalone SDL3 library";
        }
        sdl.OpenAudioDeviceStream = nullptr;
    }
    return "unavailable";
}

void sdl_unload() {
    if (g_lib != nullptr) {
        if (sdl.QuitSubSystem != nullptr) {
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
