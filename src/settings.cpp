#include "settings.hpp"

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern const LogService* svc_log;
extern const ConfigService* svc_config;
extern const UiService* svc_ui;

namespace ds {

std::atomic<int64_t> g_opt[OptCount];

namespace {

enum Kind { Bool, Int, Select, Color };
enum Tab { TabHaptics, TabLights, TabTriggers, TabCount };

const char* const kRumbleModes[] = {"Haptic buzz", "Original motors", "Off"};
const char* const kLightbarModes[] = {"Off", "Health", "Health + form"};
const char* const kPlayerLeds[] = {"Off", "Hearts", "Lantern oil"};

struct Def {
    const char* name;
    Kind kind;
    int64_t def;
    const char* defColor;
    Tab tab;
    const char* section;  // starts a new section when set
    const char* label;
    const char* help;
    int64_t min = 0, max = 0, step = 1;
    const char* suffix = nullptr;
    const char* const* options = nullptr;
    size_t optionCount = 0;
};

#define CAT(var, n, d, label) \
    {n, Int, d, nullptr, TabHaptics, nullptr, label, \
     "Haptic strength for this group of game sounds (0 = never felt).", 0, 400, 10, "%"}

// clang-format off
const Def kDefs[OptCount] = {
    {"hapticsEnabled", Bool, 1, nullptr, TabHaptics, "Audio haptics",
     "Sound-driven haptics",
     "Plays the game's own sound effects through the DualSense actuators (USB only). The controller "
     "must expose 4 audio channels; on Linux use the 'Analog Surround 4.0' profile."},
    {"hapticGain", Int, 100, nullptr, TabHaptics, nullptr, "Overall strength",
     "Master haptic gain.", 0, 400, 10, "%"},
    {"hapticLowpass", Int, 350, nullptr, TabHaptics, nullptr, "Low-pass cutoff",
     "Removes the high end of sounds so the actuators thump instead of buzzing like a speaker. "
     "0 = full range.", 0, 2000, 25, " Hz"},
    CAT(CatSystem, "catSystem", 0, "Menus & items"),
    CAT(CatPlayerVoice, "catPlayerVoice", 0, "Link's voice"),
    CAT(CatPlayerSe, "catPlayerSe", 100, "Link's actions (sword, items)"),
    CAT(CatFootsteps, "catFootsteps", 60, "Footsteps & hooves"),
    CAT(CatCollision, "catCollision", 120, "Hits & impacts"),
    CAT(CatCharaVoice, "catCharaVoice", 0, "Character voices"),
    CAT(CatCharaSe, "catCharaSe", 40, "Character sounds"),
    CAT(CatEnemySe, "catEnemySe", 70, "Enemies"),
    CAT(CatObjectSe, "catObjectSe", 70, "Objects (doors, chests, switches)"),
    CAT(CatEnvSe, "catEnvSe", 0, "Environment & ambience"),
    CAT(CatOther, "catOther", 0, "Other"),
    {"rumbleMode", Select, RumbleBuzz, nullptr, TabHaptics, "Rumble",
     "Game rumble",
     "Haptic buzz: the game's rumble plays on the haptic actuators. Original motors: SDL's rumble "
     "emulation (disables audio haptics while active). Off: no rumble.",
     0, 0, 1, nullptr, kRumbleModes, 3},
    {"buzzStrength", Int, 60, nullptr, TabHaptics, nullptr, "Buzz strength",
     "Strength of the haptic buzz rumble.", 0, 200, 5, "%"},
    {"buzzFreq", Int, 70, nullptr, TabHaptics, nullptr, "Buzz frequency",
     "Lower feels like a rumble, higher like a buzz.", 20, 300, 5, " Hz"},

    {"lightbarMode", Select, LightbarHealthForm, nullptr, TabLights, "Lightbar",
     "Lightbar",
     "Health: fades from the healthy to the critical color as hearts drop. Health + form: also "
     "tints for wolf form and the Twilight.",
     0, 0, 1, nullptr, kLightbarModes, 3},
    {"lightbarBrightness", Int, 100, nullptr, TabLights, nullptr, "Brightness",
     "Lightbar brightness.", 0, 100, 5, "%"},
    {"lowHealthPulse", Bool, 1, nullptr, TabLights, nullptr, "Low health pulse",
     "Pulse the lightbar while health is low (like the heart beep)."},
    {"lowHealthHearts", Int, 3, nullptr, TabLights, nullptr, "Low health below",
     "Hearts at or below which the lightbar pulses.", 1, 10, 1, " hearts"},
    {"damageFlash", Bool, 1, nullptr, TabLights, nullptr, "Flash on damage",
     "Briefly flash the lightbar when Link is hurt."},
    {"eventFlash", Bool, 1, nullptr, TabLights, nullptr, "Flash on item get",
     "Briefly flash the lightbar on heart pieces, item gets and secrets."},
    {"colorHealthy", Color, 0, "20FF40", TabLights, nullptr, "Healthy color", "Full health."},
    {"colorCritical", Color, 0, "FF1000", TabLights, nullptr, "Critical color", "Near zero health."},
    {"colorWolf", Color, 0, "5030FF", TabLights, nullptr, "Wolf color", "Tint while in wolf form."},
    {"colorTwilight", Color, 0, "FF7010", TabLights, nullptr, "Twilight color",
     "Tint inside the Twilight."},
    {"colorDamage", Color, 0, "FF0000", TabLights, nullptr, "Damage flash color", "Damage flash."},
    {"colorEvent", Color, 0, "FFD040", TabLights, nullptr, "Item flash color", "Item-get flash."},
    {"playerLeds", Select, PlayerLedsHearts, nullptr, TabLights, "Other lights",
     "Player LEDs",
     "The five white LEDs under the touchpad show health or lantern oil.", 0, 0, 1, nullptr,
     kPlayerLeds, 3},
    {"midnaMic", Bool, 1, nullptr, TabLights, nullptr, "Midna mic LED",
     "The mic LED pulses while Midna is calling you."},

    {"triggersEnabled", Bool, 1, nullptr, TabTriggers, "Adaptive triggers",
     "Adaptive triggers",
     "L2/R2 resistance and vibration based on what Link is doing. Assumes L2 = L (target) and "
     "R2 = R (shield)."},
    {"triggerStrength", Int, 100, nullptr, TabTriggers, nullptr, "Trigger strength",
     "Scales every trigger effect.", 10, 100, 10, "%"},
    {"trigLockOn", Bool, 1, nullptr, TabTriggers, "Effects", "Target lock (L2)",
     "Firm resistance while locked onto a target."},
    {"trigShield", Bool, 1, nullptr, TabTriggers, nullptr, "Shield weight (R2)",
     "Light resistance with a shield, heavier while guarding."},
    {"trigBoots", Bool, 1, nullptr, TabTriggers, nullptr, "Iron boots (L2+R2)",
     "Heavy resistance on both triggers while wearing the Iron Boots."},
    {"trigBow", Bool, 1, nullptr, TabTriggers, nullptr, "Bow tension (R2)",
     "Bow-string tension and snap while aiming the bow."},
    {"trigClawshot", Bool, 1, nullptr, TabTriggers, nullptr, "Clawshot / aiming (R2)",
     "A trigger-break click while aiming the clawshot, boomerang or other aimed items."},
    {"trigFishing", Bool, 1, nullptr, TabTriggers, nullptr, "Fishing (R2)",
     "Reel vibration while fishing."},
    {"trigHorse", Bool, 1, nullptr, TabTriggers, nullptr, "Epona gallop (L2)",
     "Galloping hoofbeats while riding."},
    {"trigSpinCharge", Bool, 1, nullptr, TabTriggers, nullptr, "Spin attack charge (R2)",
     "Vibration while charging a spin attack."},
    {"trigSlingshot", Bool, 1, nullptr, TabTriggers, nullptr, "Slingshot (R2)",
     "Elastic tension while aiming the slingshot."},
};
// clang-format on
#undef CAT

ConfigVarHandle g_handles[OptCount] = {};
std::mutex g_statusMutex;
std::string g_status = "Waiting for a DualSense...";
std::string g_statusShown;
UiElementHandle g_statusElem = 0;
UiWindowHandle g_window = 0;

int64_t parse_color(const char* s) {
    if (*s == '#') {
        ++s;
    }
    char* end = nullptr;
    const long v = std::strtol(s, &end, 16);
    return (end == s) ? -1 : (v & 0xFFFFFF);
}

void add_control(UiElementHandle pane, const Def& d, ConfigVarHandle h) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.label = d.label;
    c.help_rml = d.help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = h;
    switch (d.kind) {
    case Bool:
        c.kind = UI_CONTROL_TOGGLE;
        break;
    case Int:
        c.kind = UI_CONTROL_NUMBER;
        c.min = d.min;
        c.max = d.max;
        c.step = d.step;
        c.suffix = d.suffix;
        break;
    case Select:
        c.kind = UI_CONTROL_SELECT;
        c.options = d.options;
        c.option_count = d.optionCount;
        break;
    case Color:
        c.kind = UI_CONTROL_COLOR;
        break;
    }
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

ModResult build_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void* ud,
    ModError*) {
    const Tab tab = static_cast<Tab>(reinterpret_cast<intptr_t>(ud));
    for (int i = 0; i < OptCount; ++i) {
        if (kDefs[i].tab != tab) {
            continue;
        }
        if (kDefs[i].section != nullptr) {
            svc_ui->pane_add_section(mod_ctx, left, kDefs[i].section);
        }
        add_control(left, kDefs[i], g_handles[i]);
    }
    return MOD_OK;
}

void on_window_closed(ModContext*, UiWindowHandle, void*) {
    g_window = 0;
}

void on_open(ModContext*, void*) {
    if (g_window != 0) {
        return;
    }
    static const char* const titles[TabCount] = {"Haptics", "Lights", "Triggers"};
    UiTabDesc tabs[TabCount];
    for (int i = 0; i < TabCount; ++i) {
        tabs[i] = UI_TAB_DESC_INIT;
        tabs[i].title = titles[i];
        tabs[i].build = build_tab;
        tabs[i].user_data = reinterpret_cast<void*>(static_cast<intptr_t>(i));
    }
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = TabCount;
    desc.on_closed = on_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_window) != MOD_OK) {
        svc_log->warn(mod_ctx, "failed to open DualSense settings window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_text(mod_ctx, panel, g_statusShown.c_str(), &g_statusElem);
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Open Settings";
    c.help_rml = "Haptics, lightbar, LEDs and adaptive trigger settings.";
    c.on_pressed = on_open;
    svc_ui->pane_add_control(mod_ctx, panel, &c, nullptr);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    std::lock_guard lock(g_statusMutex);
    if (g_statusElem != 0 && g_status != g_statusShown) {
        g_statusShown = g_status;
        svc_ui->elem_set_text(mod_ctx, g_statusElem, g_statusShown.c_str());
    }
    return MOD_OK;
}

}  // namespace

ModResult settings_register(ModError* error) {
    for (int i = 0; i < OptCount; ++i) {
        const Def& d = kDefs[i];
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = d.name;
        if (d.kind == Bool) {
            desc.type = CONFIG_VAR_BOOL;
            desc.default_bool = d.def != 0;
        } else if (d.kind == Color) {
            desc.type = CONFIG_VAR_STRING;
            desc.default_string = d.defColor;
        } else {
            desc.type = CONFIG_VAR_INT;
            desc.default_int = d.def;
        }
        if (svc_config->register_var(mod_ctx, &desc, &g_handles[i]) != MOD_OK) {
            return mods::set_error(error, MOD_ERROR, "failed to register DualSense option");
        }
        g_opt[i] = d.kind == Color ? parse_color(d.defColor) : d.def;
    }
    settings_refresh();
    g_statusShown = g_status;

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);
    return MOD_OK;
}

void settings_refresh() {
    for (int i = 0; i < OptCount; ++i) {
        const ConfigVarHandle h = g_handles[i];
        switch (kDefs[i].kind) {
        case Bool: {
            bool v = false;
            if (svc_config->get_bool(mod_ctx, h, &v) == MOD_OK) {
                g_opt[i] = v;
            }
            break;
        }
        case Color: {
            char buf[16] = {};
            if (svc_config->get_string(mod_ctx, h, buf, sizeof(buf), nullptr) == MOD_OK) {
                const int64_t c = parse_color(buf);
                if (c >= 0) {
                    g_opt[i] = c;
                }
            }
            break;
        }
        default: {
            int64_t v = 0;
            if (svc_config->get_int(mod_ctx, h, &v) == MOD_OK) {
                g_opt[i] = v;
            }
            break;
        }
        }
    }
}

void settings_set_status(const char* text) {
    std::lock_guard lock(g_statusMutex);
    g_status = text;
}

}  // namespace ds
