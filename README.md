# DualSense Effects (Dusklight mod)

DualSense effects for Dusklight over **USB**. Every effect can be changed in
*Mods → DualSense Effects → Open Settings*.

| Effect | What it does | Settings |
|---|---|---|
| Sound-driven haptics | The game's own sound effects play on the haptic actuators. | On/off, overall strength, low-pass cutoff, high-frequency detail (clicks/chimes as taps), strength per sound group (Link's actions, footsteps, hits, enemies, objects, …) |
| Rumble | The game's rumble plays on the actuators, or on SDL's motor emulation, or not at all. | Mode (haptic buzz / original motors / off), buzz strength, buzz frequency |
| Controller speaker | Midna's call and voice play from the controller, plus the sounds the Wii version played on the Wii Remote (wolf senses, fishing reel, clawshot wind-up, spin attack ready, gale boomerang, spinner rails, riddles), the bow and slingshot, and optionally pickups. | On/off, volume, also-on-TV or controller-only, output (speaker / headphone jack / both), a toggle for each group |
| Lightbar | Fades from green to red as Link loses hearts. Tinted in wolf form and in the Twilight. Pulses at low health. Flashes on damage, on item gets and on heart pieces. | Mode, brightness, pulse threshold, flash toggles, all six colors |
| Midna's Lament | While Midna is gravely hurt (the Lament music, or the story stretch from Zant's ambush to her revival), the lightbar breathes slowly and unevenly in her teal with orange ember flickers, and the center player LED beats like a faint heartbeat. | On/off, both colors |
| Player LEDs | Show hearts or lantern oil on the five LEDs. | Off / hearts / oil |
| Mic LED | Pulses while Midna is calling. | On/off |
| Adaptive triggers | A GameCube-style click near full press when nothing else is active (DSX's GameCube mode). Target lock (L2). Shield weight and guard (R2). Iron Boots (both triggers). Bow tension and snap. Slingshot. A click when aiming the clawshot or boomerang. Fishing reel. Epona's gallop (L2). Spin-attack charge. | On/off, strength, a toggle for each effect |

## Requirements

- A DualSense or DualSense Edge on **USB**. Bluetooth audio haptics are not supported.
- The controller's audio device needs 4 channels, because channels 3 and 4 drive the actuators.
  On Linux with PipeWire or PulseAudio, set the controller's profile to **Analog Surround 4.0**
  (pavucontrol → Configuration). The mod panel tells you when the profile has too few channels.
- The trigger effects assume the default mapping: L2 = L (target) and R2 = R (shield).

## Troubleshooting

- The mods panel shows a live line: sounds picked up per second and output peaks. *Haptics →
  Log sound effects* also writes every sound effect ID and match counts to the log.
- "Controller audio is stalled": the controller's USB audio stopped playing (seen on Linux with
  PipeWire). Replug the controller, or switch its audio profile off and back on.

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

The release `.dusk` contains Linux (x86_64, arm64), macOS (arm64, x86_64) and Windows (amd64,
arm64) builds. All
of them build in CI. **Only Linux has been run on real hardware so far.**

- **Linux:** the mod uses the game's own SDL through its exported symbols.
- **Windows/macOS** (built, not yet run): the game doesn't export SDL there. Instead the mod
  ships its own SDL3 library and runs a private SDL instance that opens only the DualSense,
  through HIDAPI with every other backend and driver disabled. Triggers, LEDs, the lightbar,
  rumble and the haptics/speaker audio all go through that instance. The game's own writes to
  the controller are blocked so the two don't fight: `PADSetColor` on port 0, Dusklight's LED
  feature, and `PADControlMotor`.
  - CI bundles SDL3 3.4.10: `SDL3.dll` on Windows, and the universal library from the official
    `.dmg` on macOS. Local macOS builds take `-DDS_SDL3_RUNTIME=/path/to/libSDL3.0.dylib`, or
    fall back to a system copy (e.g. Homebrew).
  - The engine's `src/dusk/` code isn't in the Windows/macOS export lists. So the mod reaches the
    audio mixer (`dusk::audio::DspRender` and its globals), the audio-thread lock and
    Dusklight's LED feature by name through the symbol manifest, on every platform. If a name
    doesn't resolve, the log says `symbol '…' not found`. Sound-driven haptics and the speaker
    then turn off, while triggers, LEDs and the buzz keep working.
- `build/sdl_dyn_test` exercises the private-instance path on any OS:
  `cmake --build build --target sdl_dyn_test && ./build/sdl_dyn_test`.
