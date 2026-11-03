// Audio haptics: the game's own sound effects, re-rendered into the DualSense actuators.
//
// DspRender is replaced. Before the normal mix, a shadow pass renders only the voices that belong
// to sound effects with a haptic gain: their DSP state is snapshotted, every other voice is
// muted, the original renderer runs into a scratch buffer, and the state is restored so the
// normal mix renders exactly what it would have. The scratch mix is low-passed, mixed with the
// optional rumble buzz and sent to the controller's audio device on channels 3/4 (the actuators).
#include "haptics.hpp"

#include "controller.hpp"

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
#include "Z2AudioLib/Z2SeMgr.h"
#include "dsp_globals.hpp"
#include <dolphin/pad.h>

#include <algorithm>
#include <iterator>
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

// Sounds that can play on the controller speaker, keyed by sound ID -> the toggle enabling them.
const std::unordered_map<u32, Opt>& speaker_table() {
    static const std::unordered_map<u32, Opt> table = [] {
        std::unordered_map<u32, Opt> t;
        for (u32 id : {Z2SE_NAVI_CALLVOICE, Z2SE_NAVI_BLINK, Z2SE_NAVI_TALK_START,
                 Z2SE_NAVI_TALK_END}) {
            t[id] = SpkMidnaCall;
        }
        static const u32 midnaVoice[] = {
#include "midna_voice.inc"
        };
        for (u32 id : midnaVoice) {
            t[id] = SpkMidnaVoice;
        }
        // What the Wii version played on the Wii Remote speaker (Z2AudioCS call sites).
        for (u32 id : {Z2SE_READ_RIDDLE_A, Z2SE_READ_RIDDLE_B, Z2SE_WOLF_SENSE_OPEN_START,
                 Z2SE_WOLF_SENSE_CLOSE, Z2SE_AL_REEL_SLOW_LOOP, Z2SE_AL_REEL_FAST_LOOP,
                 Z2SE_LK_HS_WIND_UP, Z2SE_LK_HS_WIND_UP_FIN, Z2SE_SWORD_POWER_COME,
                 Z2SE_WOLF_POWER_COME, Z2SE_BOOM_POWER_RESUME, Z2SE_BOOM_TORNADO,
                 Z2SE_AL_SPINNER_RAIL, Z2SE_WOLFATTACK_WIND_RUSH}) {
            t[id] = SpkWiiRemote;
        }
        for (u32 id : {Z2SE_AL_DRAW_BOW, Z2SE_OBJ_ARROW_DRAW_NORMAL, Z2SE_OBJ_ARROW_SHOT_NORMAL,
                 Z2SE_OBJ_ARROW_SHOT_GAIN, Z2SE_OBJ_ARROW_SHOT_FIRE, Z2SE_OBJ_ARROWBOMB_SHOT,
                 Z2SE_AL_ARROW_EMPTY, Z2SE_AL_PACHINKO_DRAW, Z2SE_AL_PACHINKO_SHOT,
                 Z2SE_HIT_ARROW_STICK, Z2SE_HIT_ARROW_REBOUND}) {
            t[id] = SpkBow;
        }
        for (u32 id : {Z2SE_GREEN_LUPY_GET, Z2SE_BLUE_LUPY_GET, Z2SE_RED_LUPY_GET,
                 Z2SE_CONSUMP_ITEM_GET, Z2SE_HEART_PIECE_GET}) {
            t[id] = SpkPickups;
        }
        return t;
    }();
    return table;
}

float speaker_gain(u32 soundId) {
    if (!opt_on(SpeakerEnabled)) {
        return 0.0f;
    }
    const auto& table = speaker_table();
    const auto it = table.find(soundId);
    return it != table.end() && opt_on(it->second) ? static_cast<float>(opt(SpeakerVolume)) / 100.0f
                                                    : 0.0f;
}

// --- Shadow render ---------------------------------------------------------------------------

struct VoiceSave {
    int index;
    float gain[2];  // 0 = haptics, 1 = speaker
    JASDsp::TChannel channel;
    dusk::audio::ChannelAuxData aux;
};
VoiceSave g_saves[DSP_CHANNELS];
int g_saveCount = 0;
// Diagnostics shown in the mods panel: voices attributed this second and output peaks.
std::atomic<int> g_dbgRenders = 0;
std::atomic<int> g_dbgHaptic = 0, g_dbgSpeaker = 0, g_dbgTracked = 0;
std::atomic<float> g_dbgHapticPeak = 0.0f, g_dbgSpeakerPeak = 0.0f;
template <class T>
void atomic_max(std::atomic<T>& a, T v) {
    T cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
u16 g_wasActive[DSP_CHANNELS];
// Voices moved to the controller speaker sit out the normal mix (already advanced by our pass).
bool g_moved[DSP_CHANNELS];
bool g_anyMoved = false;

// Runs the original mixer for saved voices with a non-zero gain[out], one pass per distinct gain,
// adding a stereo downmix to l/r. Leaves those voices advanced; the caller restores them.
void render_pass(int out, float* l, float* r) {
    using namespace dusk::audio;
    bool done[DSP_CHANNELS] = {};
    for (int k = 0; k < g_saveCount; ++k) {
        const float gain = g_saves[k].gain[out];
        if (done[k] || gain <= 0.0f) {
            continue;
        }
        for (int i = 0; i < DSP_CHANNELS; ++i) {
            JASDsp::CH_BUF[i].mIsActive = 0;
        }
        for (int m = k; m < g_saveCount; ++m) {
            if (!done[m] && g_saves[m].gain[out] == gain) {
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
}

void restore_voices(bool keepMoved) {
    for (int k = 0; k < g_saveCount; ++k) {
        const int i = g_saves[k].index;
        if (keepMoved && g_moved[i]) {
            continue;
        }
        std::memcpy(&JASDsp::CH_BUF[i], &g_saves[k].channel, sizeof(JASDsp::TChannel));
        std::memcpy(&dusk::audio::ChannelAux[i], &g_saves[k].aux, sizeof(dusk::audio::ChannelAuxData));
    }
}

// Renders haptic voices into hl/hr and speaker voices into sl/sr.
void shadow_render(float* hl, float* hr, float* sl, float* sr) {
    JASDSPChannel* dsp = JASDSPChannel::sDspChannels;
    if (dsp == nullptr) {
        return;
    }
    const bool haptics = opt_on(HapticsEnabled);
    const float master = static_cast<float>(opt(HapticGain)) / 100.0f;
    g_saveCount = 0;
    bool anyHaptic = false, anySpeaker = false;
    {
        std::lock_guard lock(g_mapMutex);
        atomic_max(g_dbgTracked, static_cast<int>(g_chanInfo.size()));
        if (g_chanInfo.empty()) {
            return;
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
            const float hg = haptics ? category_gain(it->second.soundId) * master : 0.0f;
            const float sg = speaker_gain(it->second.soundId);
            if (hg > 0.0f || sg > 0.0f) {
                g_saves[g_saveCount].index = i;
                g_saves[g_saveCount].gain[0] = hg;
                g_saves[g_saveCount].gain[1] = sg;
                ++g_saveCount;
                anyHaptic |= hg > 0.0f;
                anySpeaker |= sg > 0.0f;
            }
        }
    }
    if (g_saveCount == 0) {
        return;
    }
    {
        int h = 0, sp = 0;
        for (int k = 0; k < g_saveCount; ++k) {
            h += g_saves[k].gain[0] > 0.0f;
            sp += g_saves[k].gain[1] > 0.0f;
        }
        atomic_max(g_dbgHaptic, h);
        atomic_max(g_dbgSpeaker, sp);
    }

    using namespace dusk::audio;
    for (int k = 0; k < g_saveCount; ++k) {
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

    if (anyHaptic) {
        render_pass(0, hl, hr);
        restore_voices(false);
    }
    if (anySpeaker) {
        render_pass(1, sl, sr);
        const bool move = opt(SpeakerMode) == SpeakerMove;
        for (int k = 0; k < g_saveCount; ++k) {
            const bool moved = move && g_saves[k].gain[1] > 0.0f;
            g_moved[g_saves[k].index] = moved;
            g_anyMoved |= moved;
        }
        restore_voices(true);
    }

    for (int i = 0; i < DSP_CHANNELS; ++i) {
        JASDsp::CH_BUF[i].mIsActive = g_moved[i] ? 0 : g_wasActive[i];
    }
    EnableReverb = reverb;
    EnableHrtf = hrtf;
    MasterVolume = volume;
    PrevMasterVolume = prevVolume;
}

// After the normal mix: moved voices rejoin (their state is already one subframe ahead).
void unmove_voices() {
    if (!g_anyMoved) {
        return;
    }
    for (int i = 0; i < DSP_CHANNELS; ++i) {
        if (g_moved[i]) {
            JASDsp::CH_BUF[i].mIsActive = g_wasActive[i];
            g_moved[i] = false;
        }
    }
    g_anyMoved = false;
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

void emit(const float* l, const float* r, const float* sl, const float* sr) {
    const int64_t cutoff = opt(HapticLowpass);
    const float a = cutoff > 0 ? 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> *
                                                 static_cast<float>(cutoff) / kRate)
                               : 1.0f;
    const bool buzz = opt(RumbleMode) == RumbleBuzz && g_motorOn.load(std::memory_order_relaxed);
    const float buzzTarget = buzz ? static_cast<float>(opt(BuzzStrength)) / 100.0f : 0.0f;
    const float buzzStep =
        2.0f * std::numbers::pi_v<float> * static_cast<float>(opt(BuzzFreq)) / kRate;
    const float* ch[2] = {l, r};
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
        // Channels 1/2 are the speaker (or the headphone jack). The speaker is mono.
        const float speaker = std::clamp((sl[j] + sr[j]) * 0.5f, -1.0f, 1.0f);
        g_frames[j * 4 + 0] = speaker;
        g_frames[j * 4 + 1] = speaker;
    }
    float hp = 0.0f, sp = 0.0f;
    for (int j = 0; j < kN; ++j) {
        hp = std::max({hp, std::fabs(g_frames[j * 4 + 2]), std::fabs(g_frames[j * 4 + 3])});
        sp = std::max(sp, std::fabs(g_frames[j * 4]));
    }
    atomic_max(g_dbgHapticPeak, hp);
    atomic_max(g_dbgSpeakerPeak, sp);
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

// Pre/post rather than replace, so other mods can still replace DspRender (e.g. Dawnlight).
float g_hl[kN], g_hr[kN], g_sl[kN], g_sr[kN];
bool g_shadowRan = false;

HookAction on_dsp_render_pre(ModContext*, void*, void*, void*) {
    g_shadowRan = g_streamOpen.load(std::memory_order_relaxed);
    g_dbgRenders.fetch_add(1, std::memory_order_relaxed);
    if (!g_shadowRan) {
        return HOOK_CONTINUE;
    }
    std::fill(std::begin(g_hl), std::end(g_hl), 0.0f);
    std::fill(std::begin(g_hr), std::end(g_hr), 0.0f);
    std::fill(std::begin(g_sl), std::end(g_sl), 0.0f);
    std::fill(std::begin(g_sr), std::end(g_sr), 0.0f);
    if ((opt_on(HapticsEnabled) || opt_on(SpeakerEnabled)) && !dusk::audio::DumpAudio) {
        shadow_render(g_hl, g_hr, g_sl, g_sr);
    }
    return HOOK_CONTINUE;
}

void on_dsp_render_post(ModContext*, void*, void*, void*) {
    unmove_voices();
    if (g_shadowRan) {
        emit(g_hl, g_hr, g_sl, g_sr);
    }
}

// --- Rumble ----------------------------------------------------------------------------------

HookAction on_pad_motor(ModContext*, void* args, void*, void*) {
    const u32 chan = mods::arg<u32>(args, 0);
    const u32 cmd = mods::arg<u32>(args, 1);
    const int64_t mode = opt(RumbleMode);
    if (chan != 0) {
        return HOOK_CONTINUE;
    }
    if (controller_owns_pad()) {
        // Private SDL instance: the game's SDL must not write to the controller; controller.cpp
        // plays motors itself when wanted.
        g_motorOn = cmd == PAD_MOTOR_RUMBLE;
        return HOOK_SKIP_ORIGINAL;
    }
    if (mode == RumbleMotors) {
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
int g_statsFrames = 0;
int g_statsLogCount = 0;
std::string g_deviceName;
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
    g_deviceName = name;
    set_status(fmt::format("Haptics and speaker active on '{}'", name));
}

}  // namespace

ModResult haptics_init(const char* native_dir, ModError* error) {
    const char* source = sdl_load(native_dir);
    mods::log::info("SDL audio: {}; gamepad effects: {}", source,
        sdl.SendGamepadEffect ? "available" : "unavailable");

    if (mods::hook::add_post<SeStartId>(on_se_start_id) != MOD_OK ||
        mods::hook::add_post<TrackChannelStart>(on_channel_start) != MOD_OK ||
        mods::hook::add_pre<DspRenderHook>(on_dsp_render_pre) != MOD_OK ||
        mods::hook::add_post<DspRenderHook>(on_dsp_render_post) != MOD_OK) {
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
    if (g_streamOpen && g_statsFrames-- <= 0) {
        g_statsFrames = 60;
        const auto pct = [](float v) { return static_cast<int>(v * 100.0f + 0.5f); };
        // Panel only (not logged): live proof that sounds are being picked up and sent.
        const std::string stats = fmt::format("Active on '{}'. Last second: {} haptic / {} "
                                              "speaker sounds (of {} tracked), peak {}% / {}%, "
                                              "{} renders, motor {}",
            g_deviceName, g_dbgHaptic.exchange(0), g_dbgSpeaker.exchange(0),
            g_dbgTracked.exchange(0), pct(g_dbgHapticPeak.exchange(0.0f)),
            pct(g_dbgSpeakerPeak.exchange(0.0f)), g_dbgRenders.exchange(0),
            g_motorOn.load() ? "on" : "off");
        settings_set_status(stats.c_str());
        if (++g_statsLogCount % 10 == 0) {
            mods::log::info("{}", stats);  // every ~10 s, for remote diagnosis
        }
    }
}

bool haptics_motor_on() {
    return g_motorOn.load(std::memory_order_relaxed);
}

bool haptics_stream_open() {
    return g_streamOpen.load(std::memory_order_relaxed);
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
