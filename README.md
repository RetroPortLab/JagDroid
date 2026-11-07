# JagDroid (experimental)

An original Atari Jaguar emulator written from scratch for Android, aimed at
Snapdragon handhelds. Not derived from BigPEmu or any other emulator; the only
third-party code is Musashi (68000 core, MIT licence, `third_party/Musashi`).

## Status

| Part | State |
|---|---|
| 68000 (Musashi), memory map, TOM/JERRY interrupts | Working |
| Boot without BIOS (jumps to the cart's `$800404` entry) | Working |
| Optional real BIOS (128 KB, you supply it) | Working: logo, cart check, hands over to the game |
| GPU / DSP RISC interpreter, banks, delay slots, interrupts | Working on the test cart |
| Object processor: bitmap, scaled, branch, GPU, stop objects + write-back | Working |
| Video modes: RGB16, RGB24, mixed; CRY | CRY built from the manual's colour geometry, calibrated on known colours; close, not bit-exact |
| Blitter: pixel/phrase, LFU, pattern, transparency, bit expansion, clipping, Gouraud, Z-buffer | Working in Cybermorph |
| Audio: DSP I2S interrupt + LTXD/RTXD capture, PIT timers | Working on the test cart |
| Joypad matrix (port 1) | Working |
| Cart EEPROM saves, save states | Working |
| Jaguar CD: Butch controller, DSA drive commands, multi-session .cue/.bin, boot via the real CD BIOS | Battle Morph boots to its intro and title; other discs untested |
| Jaguar CD saves (the CD unit save chip, shared by all discs) | Yes, kept in `jaguar-cd-memory-track.bin` |
| Memory Track cartridge (the 128 KB CD save cartridge, emulated at the NVM BIOS level), kept in `jaguar-cd-memory-track-cartridge.bin` | Yes |
| Memory Track manager (Settings > Saves: list, delete, erase) | Yes |
| Jaguar CD audio tracks, .cdi images, the original on-console manager screen | Not yet |
| Cycle-accurate timing | Approximate |
| Display enhancements: smooth Gouraud shading, 2x render scale | Optional, off by default; never change emulation |

## Download and updates

Get the latest APK from the [Releases page](https://github.com/RetroPortLab/jagdroid/releases)
(arm64, Android 8 or newer). From v0.27 on, JagDroid can update itself:
*Settings > Updates > Check for updates* downloads the newest release and hands
it to Android's installer, and with *Check automatically* on, the library shows
a banner when a new version is out. The first time, Android asks you to allow
JagDroid to install apps. Games, saves and settings are kept across updates.

No games, BIOS files or other copyrighted material are included or distributed;
you supply your own.

Commercial games will need iteration: run your own ROM, report what happens,
and the core gets fixed against it.

## Jaguar CD

Put each disc's `.cue` and `.bin` files in your games folder (or a folder of
their own inside it), or leave the disc zipped (cue and bins in one `.zip`).
Zipped discs whose tracks are stored uncompressed play straight from the zip;
compressed ones are unpacked into the app's cache the first time (only the
most recent zipped disc is kept). Cartridges can be zipped too. Then pick the Jaguar CD BIOS (256 KB, you supply it) in
Settings > Jaguar CD BIOS. Redump-style cue sheets with one file per track and
`REM SESSION` lines are supported. Desktop: `jagrun <cd-bios> --cd disc.cue` (or `disc.zip`); `jagrun game.zip` for zipped carts.

## Display enhancements

*Smooth Gouraud shading* keeps the 8 bits of intensity the blitter computes
but throws away, for every Gouraud-shaded pixel, and rebuilds those surfaces
at 16-bit intensity dithered to the screen. *Render scale 2x* draws the
picture at double size and interpolates shading between pixels inside smooth
surfaces. Polygon edges, flat-shaded facets, textures and 2D art are copied
pixel for pixel: Jaguar games compute their spans in whole pixels on the GPU,
so real higher-resolution geometry would need per-game patches.
Desktop: `jagrun <cart> --smooth 2` also writes `<out>.smooth.ppm`.

*HDR* (Android, HDR screens only) sends the picture as extended-range linear
light (scRGB half floats). Everything up to mid-bright is unchanged; the top of
the range is stretched to the chosen highlight boost (2x-4x normal white), and
the brightness that scanlines take away is restored. If the screen or driver
refuses the HDR buffer, the app falls back to normal output and says so.

## Controls (Retroid / Xbox-style naming)

| Pad | Jaguar |
|---|---|
| D-pad / left stick | D-pad |
| X (left face) / A (bottom) / B (right) | A / B / C |
| Start / Select or Y | Pause / Option |
| L1 / R1 / L2 / R2 | Keypad 1 / 2 / 3 / 4 |
| L3 / R3 | Keypad 0 / # |
| Select + Start or Back | Return to menu |

Edit `mapKey()` in `MainActivity.java` to change them.

## Building

**APK:** `cd android && gradle assembleRelease` (needs the Android SDK, NDK 27 and
CMake 3.22), or run the `jagdroid-android` workflow from `codemagic.yaml`.
Output: `android/app/build/outputs/apk/release/app-release.apk` (arm64-v8a).
Official releases are signed with the project's release key, read from
`android/keystore.properties` (not in the repo); without it the build falls back
to the debug key, and Android won't install that over an official release.

**Desktop test runner (Linux/macOS):**

```
cmake -S . -B build && cmake --build build -j
python3 test/make_testcart.py build/testcart.j64
./build/jagrun build/testcart.j64 --frames 60 --out frame.ppm -v
./build/jagrun yourgame.j64 --frames 600 --out shot.ppm -v      # your own ROM
```

## Notes for the Retroid Pocket / Turnip

Video is copied into the window buffer and scaled by the display hardware, so the
GPU driver (stock Adreno or Turnip) isn't involved and doesn't matter. The load is
all CPU: the core is a plain interpreter and ran the test cart at ~275 fps on a
desktop CPU, so a modern Snapdragon should keep 60 fps with headroom.
