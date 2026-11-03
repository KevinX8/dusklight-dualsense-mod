// Config vars and the mods panel. Values are refreshed once per frame (settings_refresh) into
// atomics so the audio thread can read them too.
#pragma once

#include "mods/svc/config.h"

#include <atomic>
#include <cstdint>

namespace ds {

enum Opt : int {
    // Haptics
    HapticsEnabled,
    HapticGain,
    HapticLowpass,
    CatSystem,
    CatPlayerVoice,
    CatPlayerSe,
    CatFootsteps,
    CatCollision,
    CatCharaVoice,
    CatCharaSe,
    CatEnemySe,
    CatObjectSe,
    CatEnvSe,
    CatOther,
    RumbleMode,
    BuzzStrength,
    BuzzFreq,
    // Speaker
    SpeakerEnabled,
    SpeakerVolume,
    SpeakerMode,
    SpkMidnaCall,
    SpkMidnaVoice,
    SpkWiiRemote,
    SpkBow,
    SpkPickups,
    // Lights
    LightbarMode,
    LightbarBrightness,
    LowHealthPulse,
    LowHealthHearts,
    DamageFlash,
    EventFlash,
    ColorHealthy,
    ColorCritical,
    ColorWolf,
    ColorTwilight,
    ColorDamage,
    ColorEvent,
    PlayerLeds,
    MidnaMic,
    MidnaLament,
    ColorLament,
    ColorLamentEmber,
    // Triggers
    TriggersEnabled,
    TriggerStrength,
    TrigLockOn,
    TrigShield,
    TrigBoots,
    TrigBow,
    TrigClawshot,
    TrigFishing,
    TrigHorse,
    TrigSpinCharge,
    TrigSlingshot,
    OptCount
};

enum RumbleModeValue { RumbleBuzz = 0, RumbleMotors = 1, RumbleOff = 2 };
enum LightbarModeValue { LightbarOff = 0, LightbarHealth = 1, LightbarHealthForm = 2 };
enum SpeakerModeValue { SpeakerMirror = 0, SpeakerMove = 1 };
enum PlayerLedsValue { PlayerLedsOff = 0, PlayerLedsHearts = 1, PlayerLedsOil = 2 };

// Ints/bools as-is; colors as 0xRRGGBB.
extern std::atomic<int64_t> g_opt[OptCount];

inline int64_t opt(Opt o) { return g_opt[o].load(std::memory_order_relaxed); }
inline bool opt_on(Opt o) { return opt(o) != 0; }

ModResult settings_register(ModError* error);
void settings_refresh();
// Free-form status line shown in the panel (device detection results).
void settings_set_status(const char* text);

}  // namespace ds
