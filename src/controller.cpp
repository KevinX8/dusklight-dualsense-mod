#include "controller.hpp"

#include "ds5.hpp"
#include "sdl_dyn.hpp"
#include "settings.hpp"

#include "mods/service.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/hook.h"
extern const HookService* svc_hook;
#include "mods/svc/hook.hpp"

#include "Z2AudioLib/Z2SeMgr.h"
#include "Z2AudioLib/Z2SeqMgr.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/d_attention.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dusk/gamepad_color.h"
#include "haptics.hpp"
#include <dolphin/pad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

DEFINE_HOOK(&Z2SeMgr::seStart, SeStart);
DEFINE_HOOK(&Z2SeqMgr::subBgmStart, SubBgmStart);
DEFINE_HOOK(PADSetColor, PadSetColor);
DEFINE_HOOK(dusk::input::handleGamepadColor, GameGamepadColor);

namespace ds {
namespace {

constexpr u32 kSonyVid = 0x054C;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double g_midnaCallUntil = 0.0;
double g_eventFlashStart = -10.0;

void on_se_start(ModContext*, void* args, void*, void*) {
    const u32 id = static_cast<u32>(mods::arg<JAISoundID>(args, 1));
    if (id == Z2SE_NAVI_CALLVOICE || id == Z2SE_NAVI_BLINK) {
        g_midnaCallUntil = now() + 6.0;
    } else if (id == Z2SE_NAVI_TALK_START) {
        g_midnaCallUntil = 0.0;
    } else if (id == Z2SE_HEART_PIECE_GET) {
        g_eventFlashStart = now();
    }
}

void on_sub_bgm_start(ModContext*, void* args, void*, void*) {
    switch (mods::arg<u32>(args, 1)) {
    case Z2BGM_ITEM_GET:
    case Z2BGM_ITEM_GET_MINI:
    case Z2BGM_ITEM_GET_ME:
    case Z2BGM_ITEM_GET_ME_S:
    case Z2BGM_HEART_GET:
    case Z2BGM_OPEN_BOX:
    case Z2BGM_FISHING_GET1:
    case Z2BGM_FISHING_GET2:
    case Z2BGM_FISHING_GET3:
    case Z2BGM_ITEM_GET_INSECT:
    case Z2BGM_ITEM_GET_POU:
        g_eventFlashStart = now();
        break;
    default:
        break;
    }
}

// --- Colors ----------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};

Rgb color(Opt o) {
    const int64_t c = opt(o);
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}

Rgb mix(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// --- State -----------------------------------------------------------------------------------

bool g_haveOriginalColor = false;
u8 g_origColor[3] = {};
int g_lastColor = -1;
u16 g_prevLife = 0;
double g_damageFlashStart = -10.0;

Ds5Effects g_lastSent = {};
bool g_sentOnce = false;
SDL_Gamepad* g_lastPad = nullptr;
int g_resendFrames = 0;
// Which effect groups we own and must reset when turned off.
bool g_ownTriggers = false, g_ownPadLights = false, g_ownMic = false;

bool is_dualsense(u32 vid, u32 pid) {
    return vid == kSonyVid && (pid == 0x0CE6 || pid == 0x0DF2);
}

// Private SDL instance (Windows/macOS): our own handle to the DualSense.
SDL_Gamepad* g_ownPad = nullptr;
int g_ownPadPoll = 0;
bool g_rumbling = false;
int g_rumbleRefresh = 0;
// Lightbar color carried in our effect report when the private instance owns the controller.
bool g_reportLed = false;
u8 g_reportRgb[3] = {};

bool private_pad() {
    return sdl.own_gamepad && g_ownPad != nullptr;
}

void poll_own_pad() {
    sdl.UpdateGamepads();
    if (g_ownPad != nullptr && !sdl.GamepadConnected(g_ownPad)) {
        sdl.CloseGamepad(g_ownPad);
        g_ownPad = nullptr;
        g_lastPad = nullptr;
    }
    if (g_ownPad != nullptr || g_ownPadPoll-- > 0) {
        return;
    }
    g_ownPadPoll = 60;
    int count = 0;
    SDL_JoystickID* ids = sdl.GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count && g_ownPad == nullptr; ++i) {
        if (is_dualsense(sdl.GetGamepadVendorForID(ids[i]), sdl.GetGamepadProductForID(ids[i]))) {
            g_ownPad = sdl.OpenGamepad(ids[i]);
        }
    }
    if (ids != nullptr) {
        sdl.Free(ids);
    }
    if (g_ownPad != nullptr) {
        mods::log::info("opened the DualSense on the private SDL instance");
    }
}

// The game's rumble never reaches the DualSense in private mode (see haptics.cpp); play it here
// when the user wants motors, or as the fallback when buzz mode has no haptic audio device.
void update_own_rumble() {
    const int64_t mode = opt(RumbleMode);
    const bool want = haptics_motor_on() &&
                      (mode == RumbleMotors || (mode == RumbleBuzz && !haptics_stream_open()));
    if (want == g_rumbling && (!want || g_rumbleRefresh-- > 0)) {
        return;
    }
    sdl.RumbleGamepad(g_ownPad, want ? 32767 : 0, want ? 32767 : 0, want ? 1000 : 0);
    g_rumbling = want;
    g_rumbleRefresh = 30;
}

// Game writes to the lightbar would fight ours: Dusklight's own LED feature always, and in
// private mode any PADSetColor (it goes through the game's SDL to the same controller).
HookAction on_game_gamepad_color(ModContext*, void*, void*, void*) {
    return opt(LightbarMode) != LightbarOff ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction on_pad_set_color(ModContext*, void* args, void*, void*) {
    return private_pad() && mods::arg<u32>(args, 0) == 0 ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

void set_lightbar(Rgb c) {
    const auto to8 = [](float v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const u8 r = to8(c.r), g = to8(c.g), b = to8(c.b);
    const int packed = (r << 16) | (g << 8) | b;
    if (packed == g_lastColor) {
        return;
    }
    g_lastColor = packed;
    if (private_pad()) {
        g_reportLed = true;
        g_reportRgb[0] = r;
        g_reportRgb[1] = g;
        g_reportRgb[2] = b;
        return;
    }
    if (!g_haveOriginalColor) {
        g_haveOriginalColor =
            PADGetColor(0, &g_origColor[0], &g_origColor[1], &g_origColor[2]) != FALSE;
    }
    PADSetColor(0, r, g, b);
}

void restore_lightbar() {
    g_reportLed = false;
    if (!private_pad() && g_haveOriginalColor && g_lastColor != -1) {
        PADSetColor(0, g_origColor[0], g_origColor[1], g_origColor[2]);
    }
    g_lastColor = -1;
}

// Midna's Lament (Z2BGM_MIDNA_SOS): the music itself, or the story window it belongs to, from
// Zant's ambush (M_071) until Midna is revived at Hyrule Castle (F_0250). Same test as Z2SceneMgr.
bool lament_active() {
    if (Z2SeqMgr* seq = Z2GetSeqMgr(); seq != nullptr && seq->checkBgmIDPlaying(Z2BGM_MIDNA_SOS)) {
        return true;
    }
    return dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[104]) &&
           !dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[250]);
}

// Slow, uneven breathing (~5 s) that never quite reaches full brightness.
float lament_breath(double t) {
    const float slow = 0.5f - 0.5f * std::cos(static_cast<float>(t) * 2.0f * 3.14159f / 5.0f);
    const float waver = 0.85f + 0.15f * std::sin(static_cast<float>(t) * 1.7f) *
                                    std::sin(static_cast<float>(t) * 0.53f);
    return 0.08f + 0.72f * slow * slow * waver;
}

// Short ember flickers of her hair color, a few seconds apart.
float lament_ember(double t) {
    const double period = 6.5;
    const double phase = std::fmod(t, period);
    // Irregular: skip some cycles based on the cycle number.
    const int cycle = static_cast<int>(t / period);
    if ((cycle * 2654435761u >> 28) % 3 == 0 || phase > 0.9) {
        return 0.0f;
    }
    const float k = static_cast<float>(1.0 - phase / 0.9);
    const float flicker = 0.6f + 0.4f * std::sin(static_cast<float>(phase) * 47.0f);
    return k * k * flicker;
}

bool g_lament = false;

void update_lightbar(daAlink_c* link, double t) {
    const int64_t mode = opt(LightbarMode);
    if (mode == LightbarOff || link == nullptr) {
        restore_lightbar();
        return;
    }
    const u16 life = dComIfGs_getLife();
    const u16 maxLife = std::max<u16>(dComIfGs_getMaxLifeGauge(), 4);
    if (life < g_prevLife) {
        g_damageFlashStart = t;
    }
    g_prevLife = life;

    const float health = std::clamp(static_cast<float>(life) / maxLife, 0.0f, 1.0f);
    Rgb c = mix(color(ColorCritical), color(ColorHealthy), health);
    if (mode == LightbarHealthForm) {
        if (link->checkWolf()) {
            c = mix(c, color(ColorWolf), 0.65f);
        }
        if (dKy_darkworld_check()) {
            c = mix(c, color(ColorTwilight), 0.5f);
        }
    }

    float brightness = static_cast<float>(opt(LightbarBrightness)) / 100.0f;
    if (g_lament) {
        const float ember = lament_ember(t);
        c = mix(color(ColorLament), color(ColorLamentEmber), ember);
        brightness *= std::max(lament_breath(t), ember);
    } else if (opt_on(LowHealthPulse) && life > 0 && life <= opt(LowHealthHearts) * 4) {
        const float beat = 0.5f + 0.5f * std::sin(static_cast<float>(t) * 2.0f * 3.14159f * 1.2f);
        brightness *= 0.25f + 0.75f * beat;
    }
    if (opt_on(EventFlash) && t - g_eventFlashStart < 1.2) {
        const float k = 1.0f - static_cast<float>((t - g_eventFlashStart) / 1.2);
        c = mix(c, color(ColorEvent), k);
        brightness = std::max(brightness, k);
    }
    if (opt_on(DamageFlash) && t - g_damageFlashStart < 0.4) {
        const float k = 1.0f - static_cast<float>((t - g_damageFlashStart) / 0.4);
        c = mix(c, color(ColorDamage), k);
        brightness = std::max(brightness, k);
    }
    set_lightbar({c.r * brightness, c.g * brightness, c.b * brightness});
}

// --- Triggers --------------------------------------------------------------------------------

bool proc_in(u16 proc, std::initializer_list<int> procs) {
    return std::find(procs.begin(), procs.end(), proc) != procs.end();
}

void choose_triggers(daAlink_c* link, Trigger& left, Trigger& right) {
    left = trigger_off();
    right = trigger_off();
    if (link == nullptr) {
        return;
    }
    const float scale = static_cast<float>(opt(TriggerStrength)) / 100.0f;
    const auto s = [scale](int strength) {
        return std::max(1, static_cast<int>(std::lround(strength * scale)));
    };
    const u16 proc = link->mProcID;

    if (opt_on(TrigBoots) && link->checkEquipHeavyBoots()) {
        left = right = trigger_feedback(0, s(8));
        return;
    }

    // R2
    const bool aimingBow = proc_in(proc, {daAlink_c::PROC_BOW_SUBJECT, daAlink_c::PROC_BOW_MOVE,
                                             daAlink_c::PROC_HORSE_BOW_SUBJECT,
                                             daAlink_c::PROC_CANOE_BOW_SUBJECT});
    const bool slingshot = link->mEquipItem == dItemNo_PACHINKO_e;
    if (aimingBow && slingshot) {
        if (opt_on(TrigSlingshot)) {
            right = trigger_bow(1, 6, s(3), s(4));
        }
    } else if (aimingBow) {
        if (opt_on(TrigBow)) {
            right = trigger_bow(1, 8, s(6), s(8));
        }
    } else if (proc_in(proc, {daAlink_c::PROC_HOOKSHOT_SUBJECT,
                                 daAlink_c::PROC_HORSE_HOOKSHOT_SUBJECT,
                                 daAlink_c::PROC_SWIM_HOOKSHOT_SUBJECT,
                                 daAlink_c::PROC_CANOE_HOOKSHOT_SUBJECT,
                                 daAlink_c::PROC_BOOMERANG_SUBJECT,
                                 daAlink_c::PROC_HORSE_BOOMERANG_SUBJECT,
                                 daAlink_c::PROC_CANOE_BOOMERANG_SUBJECT,
                                 daAlink_c::PROC_COPY_ROD_SUBJECT,
                                 daAlink_c::PROC_IRON_BALL_SUBJECT, daAlink_c::PROC_HAWK_SUBJECT})) {
        if (opt_on(TrigClawshot)) {
            right = trigger_weapon(3, 6, s(6));
        }
    } else if (proc == daAlink_c::PROC_CANOE_FISHING_REEL) {
        if (opt_on(TrigFishing)) {
            right = trigger_vibration(0, s(6), 25);
        }
    } else if (proc_in(proc, {daAlink_c::PROC_FISHING_CAST, daAlink_c::PROC_FISHING_FOOD,
                                 daAlink_c::PROC_CANOE_FISHING_WAIT,
                                 daAlink_c::PROC_CANOE_FISHING_GET})) {
        if (opt_on(TrigFishing)) {
            right = proc == daAlink_c::PROC_CANOE_FISHING_GET ? trigger_feedback(0, s(7))
                                                               : trigger_vibration(3, s(2), 10);
        }
    } else if (proc == daAlink_c::PROC_CUT_TURN_CHARGE) {
        if (opt_on(TrigSpinCharge)) {
            right = trigger_vibration(0, s(5), 40);
        }
    } else if (opt_on(TrigShield) && !link->checkWolf() && daPy_py_c::checkShieldGet()) {
        right = trigger_feedback(1, link->checkPlayerGuard() ? s(6) : s(2));
    }

    // L2
    if (opt_on(TrigHorse) && link->checkHorseRide()) {
        if (daHorse_c* horse = dComIfGp_getHorseActor()) {
            const float speed = std::fabs(horse->speedF);
            if (speed > 1.0f) {
                left = trigger_galloping(0, 9, 0, 1, std::clamp(static_cast<int>(speed / 5.0f), 2, 9));
                return;
            }
        }
    }
    if (opt_on(TrigLockOn)) {
        dAttention_c* attention = dComIfGp_getAttention();
        if (attention != nullptr && attention->Lockon()) {
            left = trigger_feedback(3, s(5));
        }
    }
}

// --- Effect report ---------------------------------------------------------------------------

u8 fill_leds(float fraction) {
    const int n = std::clamp(static_cast<int>(std::ceil(fraction * 5.0f - 1e-4f)), 0, 5);
    return static_cast<u8>((1u << n) - 1);
}

void update_effects(daAlink_c* link, double t, SDL_Gamepad* pad) {
    Ds5Effects fx = {};

    if (opt_on(TriggersEnabled)) {
        Trigger left, right;
        choose_triggers(link, left, right);
        fx.enableBits1 |= 0x04 | 0x08;
        std::memcpy(fx.rightTrigger, right.data(), right.size());
        std::memcpy(fx.leftTrigger, left.data(), left.size());
        g_ownTriggers = true;
    } else if (g_ownTriggers) {
        const Trigger off = trigger_off();
        fx.enableBits1 |= 0x04 | 0x08;
        std::memcpy(fx.rightTrigger, off.data(), off.size());
        std::memcpy(fx.leftTrigger, off.data(), off.size());
        g_ownTriggers = false;
    }

    const int64_t leds = opt(PlayerLeds);
    if (g_lament) {
        // One faint heartbeat on the center LED: two beats, then a long pause.
        fx.enableBits2 |= 0x10;
        const double beat = std::fmod(t, 2.2);
        fx.padLights = (beat < 0.12 || (beat > 0.3 && beat < 0.42)) ? 0x04 : 0x00;
        g_ownPadLights = true;
    } else if (leds != PlayerLedsOff && link != nullptr) {
        fx.enableBits2 |= 0x10;
        if (leds == PlayerLedsHearts) {
            const u16 maxLife = std::max<u16>(dComIfGs_getMaxLifeGauge(), 4);
            fx.padLights = fill_leds(static_cast<float>(dComIfGs_getLife()) / maxLife);
        } else {
            const u16 maxOil = dComIfGs_getMaxOil();
            fx.padLights = maxOil ? fill_leds(static_cast<float>(dComIfGs_getOil()) / maxOil) : 0;
        }
        g_ownPadLights = true;
    } else if (g_ownPadLights) {
        fx.enableBits2 |= 0x10;
        fx.padLights = 0x04;  // SDL's player 1 pattern
        g_ownPadLights = false;
    }

    if (opt_on(MidnaMic) && link != nullptr) {
        fx.enableBits2 |= 0x01;
        fx.micLightMode = t < g_midnaCallUntil ? 2 : 0;
        g_ownMic = true;
    } else if (g_ownMic) {
        fx.enableBits2 |= 0x01;
        fx.micLightMode = 0;
        g_ownMic = false;
    }

    if (g_reportLed) {
        fx.enableBits2 |= 0x04;
        fx.ledRed = g_reportRgb[0];
        fx.ledGreen = g_reportRgb[1];
        fx.ledBlue = g_reportRgb[2];
    }

    const bool force = pad != g_lastPad || g_resendFrames-- <= 0;
    if (fx.enableBits1 == 0 && fx.enableBits2 == 0) {
        return;
    }
    if (!force && g_sentOnce && std::memcmp(&fx, &g_lastSent, sizeof(fx)) == 0) {
        return;
    }
    if (sdl.SendGamepadEffect(pad, &fx, sizeof(fx))) {
        g_lastSent = fx;
        g_sentOnce = true;
        g_lastPad = pad;
        g_resendFrames = 180;  // re-assert after controller reconnects or SDL resets
    }
}

}  // namespace

ModResult controller_init(ModError*) {
    if (mods::hook::add_post<SeStart>(on_se_start) != MOD_OK) {
        mods::log::warn("Z2SeMgr::seStart hook unavailable; Midna mic LED and heart flash disabled");
    }
    if (mods::hook::add_post<SubBgmStart>(on_sub_bgm_start) != MOD_OK) {
        mods::log::warn("Z2SeqMgr::subBgmStart hook unavailable; item-get flash disabled");
    }
    if (mods::hook::add_pre<GameGamepadColor>(on_game_gamepad_color) != MOD_OK) {
        mods::log::warn("handleGamepadColor hook unavailable; Dusklight's LED option may fight ours");
    }
    if (sdl.own_gamepad && mods::hook::add_pre<PadSetColor>(on_pad_set_color) != MOD_OK) {
        mods::log::warn("PADSetColor hook unavailable; the game may override the lightbar");
    }
    return MOD_OK;
}

void controller_update() {
    const double t = now();
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    g_lament = link != nullptr && opt_on(MidnaLament) && lament_active();

    if (sdl.own_gamepad) {
        poll_own_pad();
        if (g_ownPad != nullptr) {
            update_own_rumble();
            update_lightbar(link, t);
            update_effects(link, t, g_ownPad);
        }
        return;
    }

    u32 vid = 0, pid = 0;
    if (PADGetIndexForPort(0) < 0) {
        g_lastPad = nullptr;
        g_lastColor = -1;
        return;
    }
    PADGetVidPid(0, &vid, &pid);
    if (vid == kSonyVid) {
        update_lightbar(link, t);
    }
    if (is_dualsense(vid, pid) && sdl.SendGamepadEffect != nullptr) {
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(PADGetIndexForPort(0))) {
            update_effects(link, t, pad);
        }
    }
}

bool controller_owns_pad() {
    return private_pad();
}

void controller_shutdown() {
    restore_lightbar();
    if (g_lastPad != nullptr && sdl.SendGamepadEffect != nullptr) {
        u32 vid = 0, pid = 0;
        PADGetVidPid(0, &vid, &pid);
        SDL_Gamepad* pad = private_pad()                 ? g_ownPad
                           : PADGetIndexForPort(0) >= 0 ? PADGetSDLGamepadForIndex(PADGetIndexForPort(0))
                                                        : nullptr;
        if (pad == g_lastPad && (private_pad() || is_dualsense(vid, pid))) {
            Ds5Effects fx = {};
            const Trigger off = trigger_off();
            fx.enableBits1 = 0x04 | 0x08;
            fx.enableBits2 = 0x10 | 0x01;
            std::memcpy(fx.rightTrigger, off.data(), off.size());
            std::memcpy(fx.leftTrigger, off.data(), off.size());
            fx.padLights = 0x04;
            sdl.SendGamepadEffect(pad, &fx, sizeof(fx));
        }
    }
    if (g_ownPad != nullptr) {
        if (g_rumbling) {
            sdl.RumbleGamepad(g_ownPad, 0, 0, 0);
        }
        sdl.CloseGamepad(g_ownPad);
        g_ownPad = nullptr;
    }
    g_rumbling = false;
    g_lastPad = nullptr;
    g_sentOnce = false;
    g_ownTriggers = g_ownPadLights = g_ownMic = false;
}

}  // namespace ds
