// DualSense output report pieces sent through SDL_SendGamepadEffect.
// Layout mirrors SDL's DS5EffectsState_t (src/joystick/hidapi/SDL_hidapi_ps5.c); trigger encodings
// follow https://controllers.fandom.com/wiki/Sony_DualSense#FFB_Trigger_Modes.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ds {

struct Ds5Effects {
    uint8_t enableBits1;           // 0: 0x04 right trigger, 0x08 left trigger
    uint8_t enableBits2;           // 1: 0x01 mic LED, 0x04 lightbar, 0x10 player LEDs
    uint8_t rumbleRight;           // 2
    uint8_t rumbleLeft;            // 3
    uint8_t headphoneVolume;       // 4
    uint8_t speakerVolume;         // 5
    uint8_t micVolume;             // 6
    uint8_t audioEnableBits;       // 7
    uint8_t micLightMode;          // 8: 0 off, 1 on, 2 pulse
    uint8_t audioMuteBits;         // 9
    uint8_t rightTrigger[11];      // 10
    uint8_t leftTrigger[11];       // 21
    uint8_t unknown1[6];           // 32
    uint8_t enableBits3;           // 38
    uint8_t unknown2[2];           // 39
    uint8_t ledAnim;               // 41
    uint8_t ledBrightness;         // 42
    uint8_t padLights;             // 43
    uint8_t ledRed, ledGreen, ledBlue;  // 44
};
static_assert(sizeof(Ds5Effects) == 47);

using Trigger = std::array<uint8_t, 11>;

// Strengths are 1..8, zones 0..9 (0 = trigger released, 9 = fully pulled).
inline uint8_t clamp_strength(int s) { return static_cast<uint8_t>(std::clamp(s, 1, 8)); }

inline Trigger trigger_off() {
    Trigger t{};
    t[0] = 0x05;
    return t;
}

// DSX "GameCube": legacy section mode, a wall between 144 and 160 like the GC's analog-to-digital click.
inline Trigger trigger_gamecube(uint8_t force = 0xFF) {
    Trigger t{};
    t[0] = 0x02;
    t[1] = 0x90;
    t[2] = 0xA0;
    t[3] = force;
    return t;
}

inline Trigger zones_packed(uint8_t mode, const std::array<uint8_t, 10>& strengths, uint8_t freq) {
    Trigger t{};
    t[0] = mode;
    uint16_t active = 0;
    uint32_t packed = 0;
    for (int i = 0; i < 10; ++i) {
        if (strengths[i] > 0) {
            active |= static_cast<uint16_t>(1u << i);
            packed |= static_cast<uint32_t>((clamp_strength(strengths[i]) - 1) & 7) << (3 * i);
        }
    }
    t[1] = active & 0xFF;
    t[2] = active >> 8;
    t[3] = packed & 0xFF;
    t[4] = (packed >> 8) & 0xFF;
    t[5] = (packed >> 16) & 0xFF;
    t[6] = (packed >> 24) & 0xFF;
    t[9] = freq;
    return t;
}

// Constant resistance from `start` to the end of travel.
inline Trigger trigger_feedback(int start, int strength) {
    std::array<uint8_t, 10> s{};
    for (int i = std::clamp(start, 0, 9); i < 10; ++i) {
        s[i] = clamp_strength(strength);
    }
    return zones_packed(0x21, s, 0);
}

// Vibration from `start`, amplitude 1..8, frequency in Hz.
inline Trigger trigger_vibration(int start, int amplitude, int freq) {
    std::array<uint8_t, 10> s{};
    for (int i = std::clamp(start, 0, 9); i < 10; ++i) {
        s[i] = clamp_strength(amplitude);
    }
    return zones_packed(0x26, s, static_cast<uint8_t>(std::clamp(freq, 1, 255)));
}

// Resistance between start and end, then a click as the wall breaks.
inline Trigger trigger_weapon(int start, int end, int strength) {
    Trigger t{};
    const int a = std::clamp(start, 2, 7);
    const int b = std::clamp(end, a + 1, 8);
    const uint16_t zones = static_cast<uint16_t>((1u << a) | (1u << b));
    t[0] = 0x25;
    t[1] = zones & 0xFF;
    t[2] = zones >> 8;
    t[3] = clamp_strength(strength) - 1;
    return t;
}

// Tension that builds up to `end`, then snaps back.
inline Trigger trigger_bow(int start, int end, int strength, int snap) {
    Trigger t{};
    const int a = std::clamp(start, 1, 8);
    const int b = std::clamp(end, a + 1, 8);
    const uint16_t zones = static_cast<uint16_t>((1u << a) | (1u << b));
    t[0] = 0x22;
    t[1] = zones & 0xFF;
    t[2] = zones >> 8;
    t[3] = static_cast<uint8_t>(((clamp_strength(strength) - 1) & 7) |
                                (((clamp_strength(snap) - 1) & 7) << 3));
    return t;
}

// Two "hoof" taps per cycle; freq in Hz.
inline Trigger trigger_galloping(int start, int end, int first_foot, int second_foot, int freq) {
    Trigger t{};
    const int a = std::clamp(start, 0, 8);
    const int b = std::clamp(end, a + 1, 9);
    const int f1 = std::clamp(first_foot, 0, 6);
    const int f2 = std::clamp(second_foot, f1 + 1, 7);
    const uint16_t zones = static_cast<uint16_t>((1u << a) | (1u << b));
    t[0] = 0x23;
    t[1] = zones & 0xFF;
    t[2] = zones >> 8;
    t[3] = static_cast<uint8_t>((f2 & 7) | ((f1 & 7) << 3));
    t[4] = static_cast<uint8_t>(std::clamp(freq, 1, 255));
    return t;
}

}  // namespace ds
