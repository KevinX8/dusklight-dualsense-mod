// Hardware check without the game: finds the DualSense audio device like the mod does and plays
// a 1 s 440 Hz tone on the speaker (ch 1/2), then a 1 s 120 Hz buzz on the actuators (ch 3/4).
#include "sdl_dyn.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

int main() {
    std::printf("sdl_load: %s\n", ds::sdl_load(nullptr));
    if (!ds::sdl.has_audio()) {
        return 1;
    }
    int count = 0;
    SDL_AudioDeviceID* ids = ds::sdl.GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; i < count; ++i) {
        const char* name = ds::sdl.GetAudioDeviceName(ids[i]);
        SDL_AudioSpec spec{};
        int frames = 0;
        ds::sdl.GetAudioDeviceFormat(ids[i], &spec, &frames);
        std::printf("device %u: '%s' %d ch %d Hz\n", ids[i], name, spec.channels, spec.freq);
        if (!found && name && (std::strstr(name, "DualSense") || std::strstr(name, "Wireless Controller"))) {
            found = ids[i];
        }
    }
    if (!found) {
        std::puts("no DualSense device");
        return 1;
    }
    const SDL_AudioSpec want{SDL_AUDIO_F32, 4, 48000};
    SDL_AudioStream* stream = ds::sdl.OpenAudioDeviceStream(found, &want, nullptr, nullptr);
    if (!stream) {
        std::puts("open failed");
        return 1;
    }
    ds::sdl.ResumeAudioStreamDevice(stream);
    for (int pass = 0; pass < 2; ++pass) {
        std::printf(pass == 0 ? "speaker tone (ch1/2)...\n" : "actuator buzz (ch3/4)...\n");
        std::vector<float> buf(48000 * 4, 0.0f);
        for (int j = 0; j < 48000; ++j) {
            const float v = 0.5f * std::sin(2.0f * 3.14159f * (pass == 0 ? 440.0f : 120.0f) * j / 48000.0f);
            buf[j * 4 + (pass == 0 ? 0 : 2)] = v;
            buf[j * 4 + (pass == 0 ? 1 : 3)] = v;
        }
        ds::sdl.PutAudioStreamData(stream, buf.data(), static_cast<int>(buf.size() * sizeof(float)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    }
    ds::sdl.DestroyAudioStream(stream);
    ds::sdl_unload();
    std::puts("done");
    return 0;
}
