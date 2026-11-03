# DualSense Effects (Dusklight mod)

DualSense effects for Dusklight over **USB**. Every effect can be changed in
*Mods → DualSense Effects → Open Settings*.

| Effect | What it does | Settings |
|---|---|---|
| Sound-driven haptics | The game's own sound effects play on the haptic actuators. | On/off, overall strength, low-pass cutoff, strength per sound group (Link's actions, footsteps, hits, enemies, objects, …) |
| Rumble | The game's rumble plays on the actuators, or on SDL's motor emulation, or not at all. | Mode (haptic buzz / original motors / off), buzz strength, buzz frequency |
| Controller speaker | Midna's call and voice play from the controller, plus the sounds the Wii version played on the Wii Remote (wolf senses, fishing reel, clawshot wind-up, spin attack ready, gale boomerang, spinner rails, riddles), the bow and slingshot, and optionally pickups. | On/off, volume, also-on-TV or controller-only, a toggle for each group |
| Lightbar | Fades from green to red as Link loses hearts. Tinted in wolf form and in the Twilight. Pulses at low health. Flashes on damage, on item gets and on heart pieces. | Mode, brightness, pulse threshold, flash toggles, all six colors |
| Midna's Lament | While Midna is gravely hurt (the Lament music, or the story stretch from Zant's ambush to her revival), the lightbar breathes slowly and unevenly in her teal with orange ember flickers, and the center player LED beats like a faint heartbeat. | On/off, both colors |
| Player LEDs | Show hearts or lantern oil on the five LEDs. | Off / hearts / oil |
| Mic LED | Pulses while Midna is calling. | On/off |
| Adaptive triggers | Target lock (L2). Shield weight and guard (R2). Iron Boots (both triggers). Bow tension and snap. Slingshot. A click when aiming the clawshot or boomerang. Fishing reel. Epona's gallop (L2). Spin-attack charge. | On/off, strength, a toggle for each effect |

## Requirements

- A DualSense or DualSense Edge on **USB**. Bluetooth audio haptics are not supported.
- The controller's audio device needs 4 channels, because channels 3 and 4 drive the actuators.
  On Linux with PipeWire or PulseAudio, set the controller's profile to **Analog Surround 4.0**
  (pavucontrol → Configuration). The mod panel tells you when the profile has too few channels.
- The trigger effects assume the default mapping: L2 = L (target) and R2 = R (shield).

## Build

```sh
cmake -S . -B build -DDUSKLIGHT_DIR=/path/to/dusklight -DDUSK_GAME_EXE=/path/to/dusklight/binary
cmake --build build                      # -> build/mods/dualsense_mod.dusk
cmake --build build --target ds5_test && ./build/ds5_test
```

Copy `dualsense_mod.dusk` into the mods folder (on Linux, `~/.local/share/TwilitRealm/Dusklight/mods`).

## How haptics work

`dusk::audio::DspRender` is replaced. On each audio subframe, the mod picks the voices that belong
to sound effects with a non-zero gain. It snapshots their DSP state and mutes every other voice,
then re-runs the game's own mixer into a scratch buffer with reverb and HRTF off. The speaker
sounds get a second pass of their own. In controller-only mode those voices are kept out of the
TV mix for that subframe. Finally it
restores the snapshot, so the normal mix is unchanged. The scratch mix is low-passed and sent to
the controller's audio device.

## Platform notes

- **Linux:** fully supported. The mod uses the game's own SDL through its exported symbols.
- **Windows and macOS:** untested. The game does not export SDL there, so trigger, player LED and
  mic LED effects are disabled, while the lightbar still works through Aurora's `PADSetColor`.
  Audio falls back to a system `SDL3` library if one can be loaded. On Windows, sound-driven
  haptics are compiled out because the engine's `DuskDsp.hpp` globals are not marked
  `DUSK_GAME_DATA`. The buzz rumble still works.
