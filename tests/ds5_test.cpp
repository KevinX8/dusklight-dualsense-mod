// Checks the trigger encoders against byte patterns from the DSX Linux port / DualSense docs.
#undef NDEBUG
#include "ds5.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>

int main() {
    using namespace ds;
    assert(offsetof(Ds5Effects, rightTrigger) == 10);
    assert(offsetof(Ds5Effects, leftTrigger) == 21);
    assert(offsetof(Ds5Effects, padLights) == 43);

    Trigger off = trigger_off();
    assert(off[0] == 0x05 && off[1] == 0);

    // DSX Windows ControllerDevice.cs, TriggerType.GAMECUBE.
    Trigger gc = trigger_gamecube();
    assert(gc[0] == 2 && gc[1] == 144 && gc[2] == 160 && gc[3] == 255 && gc[4] == 0 && gc[10] == 0);

    // Feedback from zone 2, strength 2: zones 2..9 active, each packed (2-1)=1.
    Trigger fb = trigger_feedback(2, 2);
    assert(fb[0] == 0x21 && fb[1] == 0xFC && fb[2] == 0x03);
    uint32_t packed = fb[3] | fb[4] << 8 | fb[5] << 16 | uint32_t(fb[6]) << 24;
    for (int i = 0; i < 10; ++i) {
        assert(((packed >> (3 * i)) & 7) == (i >= 2 ? 1u : 0u));
    }

    Trigger w = trigger_weapon(2, 8, 8);
    assert(w[0] == 0x25 && w[1] == 0x04 && w[2] == 0x01 && w[3] == 7);

    Trigger b = trigger_bow(1, 8, 8, 8);
    assert(b[0] == 0x22 && b[1] == 0x02 && b[2] == 0x01 && b[3] == 0x3F);

    Trigger v = trigger_vibration(0, 8, 30);
    assert(v[0] == 0x26 && v[1] == 0xFF && v[2] == 0x03 && v[9] == 30);

    Trigger g = trigger_galloping(0, 9, 0, 1, 8);
    assert(g[0] == 0x23 && g[1] == 0x01 && g[2] == 0x02 && g[3] == 0x01 && g[4] == 8);

    // Out-of-range input is clamped, never wraps.
    Trigger c = trigger_feedback(-5, 99);
    assert(c[1] == 0xFF && c[2] == 0x03 && ((c[3] & 7) == 7));

    std::puts("ds5_test: all checks passed");
    return 0;
}
