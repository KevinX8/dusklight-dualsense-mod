// Audio haptics: the game's own sound effects, re-rendered into the DualSense actuators.
//
// DspRender is replaced. Before the normal mix, a shadow pass renders only the voices that belong
// to sound effects with a haptic gain: their DSP state is snapshotted, every other voice is
// muted, the original renderer runs into a scratch buffer, and the state is restored so the
// normal mix renders exactly what it would have. The scratch mix is low-passed, mixed with the
// optional rumble buzz and sent to the controller's audio device on channels 3/4 (the actuators).
#include "haptics.hpp"

#include "sdl_dyn.hpp"
#include "settings.hpp"

#include "mods/service.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/hook.h"
extern const HookService* svc_hook;
#include "mods/svc/hook.hpp"

#include "JSystem/JAudio2/JAISe.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASTrack.h"
#include "dusk/audio/DuskDsp.hpp"
#include <dolphin/pad.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string>
#include <unordered_map>

DEFINE_HOOK(dusk::audio::DspRender, DspRenderHook);
// Every SE passes through here when started (startTrack_ itself is partly inlined).
DEFINE_HOOK(&JAISe::JAISeMgr_startID_, SeStartId);
DEFINE_HOOK(&JASTrack::channelStart, TrackChannelStart);
DEFINE_HOOK(PADControlMotor, PadControlMotor);

namespace ds {
namespace {

using dusk::audio::OutputSubframe;
constexpr float kRate = static_cast<float>(dusk::audio::SampleRate);
constexpr int kN = DSP_SUBFRAME_SIZE;

// --- Voice -> sound effect attribution -------------------------------------------------------

struct ChanInfo {
    u32 soundId;
    const void* mgr;  // JASChannel::mCallbackData at start; detects reuse of the channel object
};

std::mutex g_mapMutex;
std::unordered_map<const JASTrack*, u32> g_seTracks;
std::unordered_map<const JASChannel*, ChanInfo> g_chanInfo;

void on_se_start_id(ModContext*, void* args, void*, void*) {
    JAISe* se = mods::arg<JAISe*>(args, 0);
    std::lock_guard lock(g_mapMutex);
    g_seTracks[&se->inner_.track] = static_cast<u32>(se->getID());
}

void on_channel_start(ModContext*, void* args, void* retval, void*) {
    JASChannel* channel = retval ? *static_cast<JASChannel**>(retval) : nullptr;
    if (channel == nullptr) {
        return;
    }
    const JASTrack* root = mods::arg<JASTrack*>(args, 0);
    while (root->mParent != nullptr) {
        root = root->mParent;
    }
    std::lock_guard lock(g_mapMutex);
    const auto it = g_seTracks.find(root);
    if (it != g_seTracks.end()) {
        g_chanInfo[channel] = {it->second, channel->mCallbackData};
    } else {
        g_chanInfo.erase(channel);
    }
}

float category_gain(u32 soundId) {
    // Z2 sound IDs carry their SE category in bits 16..23 (see SE_CATEGORY_* in audio_res.h).
    const u32 cat = (soundId >> 16) & 0xFF;
    const Opt o = cat <= 9 ? static_cast<Opt>(CatSystem + cat) : CatOther;
    return static_cast<float>(opt(o)) / 100.0f;
}

// --- Shadow render ---------------------------------------------------------------------------

struct VoiceSave {
    int index;
    float gain;
    JASDsp::TChannel channel;
    dusk::audio::ChannelAuxData aux;
};
VoiceSave g_saves[DSP_CHANNELS];
u16 g_wasActive[DSP_CHANNELS];

// Renders haptic voices into l/r (mono-ish stereo downmix). Returns false if none were active.
bool shadow_render(float* l, float* r) {
#ifdef _WIN32
    // shortcut: DuskDsp.hpp globals lack DUSK_GAME_DATA, so Windows can't import them; sound-driven
    // haptics stay off there (buzz rumble still works) until the engine annotates them.
    (void)l;
    (void)r;
    return false;
#else
    JASDSPChannel* dsp = JASDSPChannel::sDspChannels;
    if (dsp == nullptr) {
        return false;
    }
    const float master = static_cast<float>(opt(HapticGain)) / 100.0f;
    int count = 0;
    {
        std::lock_guard lock(g_mapMutex);
        if (g_chanInfo.empty()) {
            return false;
        }
        for (int i = 0; i < DSP_CHANNELS; ++i) {
            const JASDsp::TChannel& voice = JASDsp::CH_BUF[i];
            if (!voice.mIsActive || voice.mPauseFlag || voice.mForcedStop ||
                dsp[i].mChannel != &JASDsp::CH_BUF[i]) {
                continue;
            }
            const auto* channel = static_cast<const JASChannel*>(dsp[i].mCallbackData);
            const auto it = g_chanInfo.find(channel);
            if (it == g_chanInfo.end() || channel->mCallbackData != it->second.mgr) {
                continue;
            }
            const float gain = category_gain(it->second.soundId) * master;
            if (gain > 0.0f) {
                g_saves[count].index = i;
                g_saves[count].gain = gain;
                ++count;
            }
        }
    }
    if (count == 0) {
        return false;
    }

    using namespace dusk::audio;
    for (int k = 0; k < count; ++k) {
        const int i = g_saves[k].index;
        std::memcpy(&g_saves[k].channel, &JASDsp::CH_BUF[i], sizeof(JASDsp::TChannel));
        std::memcpy(&g_saves[k].aux, &ChannelAux[i], sizeof(ChannelAuxData));
    }
    for (int i = 0; i < DSP_CHANNELS; ++i) {
        g_wasActive[i] = JASDsp::CH_BUF[i].mIsActive;
    }
    const bool reverb = EnableReverb;
    const bool hrtf = EnableHrtf;
    const f32 volume = MasterVolume;
    const f32 prevVolume = PrevMasterVolume;
    EnableReverb = false;
    EnableHrtf = false;

    // One pass per distinct gain (usually one or two categories are sounding at once).
    bool done[DSP_CHANNELS] = {};
    for (int k = 0; k < count; ++k) {
        if (done[k]) {
            continue;
        }
        const float gain = g_saves[k].gain;
        for (int i = 0; i < DSP_CHANNELS; ++i) {
            JASDsp::CH_BUF[i].mIsActive = 0;
        }
        for (int m = k; m < count; ++m) {
            if (!done[m] && g_saves[m].gain == gain) {
                JASDsp::CH_BUF[g_saves[m].index].mIsActive = g_wasActive[g_saves[m].index];
                done[m] = true;
            }
        }
        MasterVolume = PrevMasterVolume = 1.0f;
        OutputSubframe scratch = {};
        DspRenderHook::g_orig(scratch);
        const auto& c = scratch.channels;
        const bool surround = OutChannelCount > 2;
        for (int j = 0; j < kN; ++j) {
            float sl = c[0][j];
            float sr = c[1][j];
            if (surround) {
                const float center = 0.7f * c[2][j];
                sl += center + c[4][j] + (OutChannelCount > 6 ? c[6][j] : 0.0f);
                sr += center + c[5][j] + (OutChannelCount > 6 ? c[7][j] : 0.0f);
            }
            l[j] += sl * gain;
            r[j] += sr * gain;
        }
    }

    for (int k = 0; k < count; ++k) {
        const int i = g_saves[k].index;
        std::memcpy(&JASDsp::CH_BUF[i], &g_saves[k].channel, sizeof(JASDsp::TChannel));
        std::memcpy(&ChannelAux[i], &g_saves[k].aux, sizeof(ChannelAuxData));
    }
    for (int i = 0; i < DSP_CHANNELS; ++i) {
        JASDsp::CH_BUF[i].mIsActive = g_wasActive[i];
    }
    EnableReverb = reverb;
    EnableHrtf = hrtf;
    MasterVolume = volume;
    PrevMasterVolume = prevVolume;
    return true;
#endif
}

// --- Output ----------------------------------------------------------------------------------

std::mutex g_streamMutex;
SDL_AudioStream* g_stream = nullptr;
SDL_AudioDeviceID g_device = 0;
std::atomic<bool> g_streamOpen = false;
std::atomic<bool> g_motorOn = false;

// Audio-thread DSP state.
float g_lp[2][2] = {};
float g_buzzPhase = 0.0f;
float g_buzzEnv = 0.0f;
float g_frames[kN * 4];

void emit(float* l, float* r) {
    const int64_t cutoff = opt(HapticLowpass);
    const float a = cutoff > 0 ? 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> *
                                                 static_cast<float>(cutoff) / kRate)
                               : 1.0f;
    const bool buzz = opt(RumbleMode) == RumbleBuzz && g_motorOn.load(std::memory_order_relaxed);
    const float buzzTarget = buzz ? static_cast<float>(opt(BuzzStrength)) / 100.0f : 0.0f;
    const float buzzStep =
        2.0f * std::numbers::pi_v<float> * static_cast<float>(opt(BuzzFreq)) / kRate;
    float* ch[2] = {l, r};
    for (int j = 0; j < kN; ++j) {
        // ~5 ms attack/release keeps the buzz from clicking as the game pulses the motor.
        g_buzzEnv += (buzzTarget - g_buzzEnv) * 0.004f;
        const float b = g_buzzEnv > 1e-4f ? std::sin(g_buzzPhase) * g_buzzEnv : 0.0f;
        g_buzzPhase = std::fmod(g_buzzPhase + buzzStep, 2.0f * std::numbers::pi_v<float>);
        for (int c = 0; c < 2; ++c) {
            // Two cascaded one-pole low-passes (12 dB/oct).
            g_lp[c][0] += a * (ch[c][j] - g_lp[c][0]);
            g_lp[c][1] += a * (g_lp[c][0] - g_lp[c][1]);
            g_frames[j * 4 + 2 + c] = std::clamp(g_lp[c][1] + b, -1.0f, 1.0f);
        }
        g_frames[j * 4 + 0] = 0.0f;
        g_frames[j * 4 + 1] = 0.0f;
    }
    std::lock_guard lock(g_streamMutex);
    if (g_stream == nullptr) {
        return;
    }
    sdl.PutAudioStreamData(g_stream, g_frames, sizeof(g_frames));
    // The controller's clock drifts against the game's output; drop the backlog past ~60 ms.
    constexpr int kMaxQueued = static_cast<int>(kRate * 0.06f) * 4 * sizeof(float);
    if (sdl.GetAudioStreamQueued(g_stream) > kMaxQueued) {
        sdl.ClearAudioStream(g_stream);
    }
}

void on_dsp_render(ModContext*, void* args, void*, void*) {
    OutputSubframe& out = mods::arg_ref<OutputSubframe&>(args, 0);
    if (!g_streamOpen.load(std::memory_order_relaxed)) {
        DspRenderHook::g_orig(out);
        return;
    }
    float l[kN] = {};
    float r[kN] = {};
#ifdef _WIN32
    if (opt_on(HapticsEnabled)) {
#else
    if (opt_on(HapticsEnabled) && !dusk::audio::DumpAudio) {
#endif
        shadow_render(l, r);
    }
    DspRenderHook::g_orig(out);
    emit(l, r);
}

// --- Rumble ----------------------------------------------------------------------------------

HookAction on_pad_motor(ModContext*, void* args, void*, void*) {
    const u32 chan = mods::arg<u32>(args, 0);
    const u32 cmd = mods::arg<u32>(args, 1);
    const int64_t mode = opt(RumbleMode);
    if (chan != 0 || mode == RumbleMotors) {
        return HOOK_CONTINUE;
    }
    if (mode == RumbleBuzz && !g_streamOpen.load()) {
        return HOOK_CONTINUE;  // no haptic device: fall back to the motors
    }
    g_motorOn = cmd == PAD_MOTOR_RUMBLE;
    return HOOK_SKIP_ORIGINAL;
}

// --- Device management (main thread) ---------------------------------------------------------

int g_pollFrames = 0;
int64_t g_lastRumbleMode = -1;
std::string g_lastStatus;

void set_status(const std::string& s) {
    if (s != g_lastStatus) {
        g_lastStatus = s;
        settings_set_status(s.c_str());
        mods::log::info("{}", s);
    }
}

void close_stream() {
    SDL_AudioStream* stream = nullptr;
    {
        std::lock_guard lock(g_streamMutex);
        stream = g_stream;
        g_stream = nullptr;
        g_device = 0;
        g_streamOpen = false;
    }
    if (stream != nullptr) {
        sdl.DestroyAudioStream(stream);
    }
    g_motorOn = false;
}

bool is_dualsense_name(const char* name) {
    return name != nullptr &&
           (std::strstr(name, "DualSense") != nullptr ||
               std::strstr(name, "Wireless Controller") != nullptr);
}

void poll_device() {
    int count = 0;
    SDL_AudioDeviceID* ids = sdl.GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    std::string name;
    bool stillPresent = false;
    for (int i = 0; ids != nullptr && i < count; ++i) {
        if (ids[i] == g_device) {
            stillPresent = true;
        }
        const char* n = sdl.GetAudioDeviceName(ids[i]);
        if (found == 0 && is_dualsense_name(n)) {
            found = ids[i];
            name = n;
        }
    }
    if (ids != nullptr) {
        sdl.Free(ids);
    }

    if (g_streamOpen) {
        if (!stillPresent) {
            close_stream();
            set_status("DualSense audio device disconnected");
        }
        return;
    }
    if (found == 0) {
        set_status("No DualSense audio device found. Haptics need a USB connection.");
        return;
    }
    SDL_AudioSpec spec{};
    int frames = 0;
    if (!sdl.GetAudioDeviceFormat(found, &spec, &frames) || spec.channels < 4) {
        set_status(fmt::format("'{}' exposes {} audio channels; haptics need 4. On Linux, set the "
                               "controller's audio profile to 'Analog Surround 4.0'.",
            name, spec.channels));
        return;
    }
    const SDL_AudioSpec want{SDL_AUDIO_F32, 4, dusk::audio::SampleRate};
    SDL_AudioStream* stream = sdl.OpenAudioDeviceStream(found, &want, nullptr, nullptr);
    if (stream == nullptr) {
        set_status(fmt::format("Failed to open '{}' for haptics", name));
        return;
    }
    sdl.ResumeAudioStreamDevice(stream);
    {
        std::lock_guard lock(g_streamMutex);
        g_stream = stream;
        g_device = found;
        g_streamOpen = true;
    }
    set_status(fmt::format("Haptics active on '{}'", name));
}

}  // namespace

ModResult haptics_init(ModError* error) {
    const char* source = sdl_load();
    mods::log::info("SDL audio: {}; gamepad effects: {}", source,
        sdl.SendGamepadEffect ? "available" : "unavailable");

    if (mods::hook::add_post<SeStartId>(on_se_start_id) != MOD_OK ||
        mods::hook::add_post<TrackChannelStart>(on_channel_start) != MOD_OK ||
        mods::hook::replace<DspRenderHook>(on_dsp_render) != MOD_OK) {
        mods::log::warn("audio hooks unavailable; sound-driven haptics disabled");
    }
    if (mods::hook::add_pre<PadControlMotor>(on_pad_motor) != MOD_OK) {
        mods::log::warn("PADControlMotor hook unavailable; rumble mode setting has no effect");
    }
    if (!sdl.has_audio()) {
        set_status("SDL audio unavailable; haptics disabled");
    }
    (void)error;
    return MOD_OK;
}

void haptics_update() {
    const int64_t mode = opt(RumbleMode);
    if (g_lastRumbleMode == RumbleMotors && mode != RumbleMotors && PadControlMotor::g_orig) {
        PadControlMotor::g_orig(0, PAD_MOTOR_STOP_HARD);
    }
    g_lastRumbleMode = mode;

    if (!sdl.has_audio()) {
        return;
    }
    if (g_pollFrames-- <= 0) {
        g_pollFrames = 120;
        poll_device();
    }
}

void haptics_shutdown() {
    {
        // The audio thread renders under this lock: once held, it is not inside our hook, and
        // after the uninstall it can't re-enter it before the library is unloaded.
        JASCriticalSection section;
        mods::hook::uninstall<DspRenderHook>();
        mods::hook::uninstall<TrackChannelStart>();
    }
    if (sdl.has_audio()) {
        close_stream();
    }
    {
        std::lock_guard lock(g_mapMutex);
        g_seTracks.clear();
        g_chanInfo.clear();
    }
    sdl_unload();
}

}  // namespace ds
