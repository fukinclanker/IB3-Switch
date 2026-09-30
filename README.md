<div align="center">

<img src="resources/icon.jpg" alt="Infinity Blade III" width="160">

# infinityblade3_nx

**Infinity Blade III on Nintendo Switch (experimental)**

An unofficial Nintendo Switch wrapper for the Android version of
**Infinity Blade**.

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Version](https://img.shields.io/badge/Version-1.0.0-4C8BF5?style=for-the-badge)](#)
[![Unreal Engine](https://img.shields.io/badge/Unreal_Engine-3-0E1128?style=for-the-badge&logo=unrealengine&logoColor=white)](#)

</div>

---

## About

`infinityblade3_nx` is a native wrapper that loads `libib3.so` from the Android
**Infinity Blade III** port (APK v1.2.1) on Nintendo Switch. It recreates the
Android, Bionic, audio, input and graphics services that library expects under
Horizon OS.

`libib3.so` is **not** the game compiled for Android. It is an iOS
compatibility runtime (an ARM64 guest CPU plus high-level emulation of the
iOS frameworks) that runs the original iOS release of Infinity Blade III
(v1.4.4) from a user-supplied IPA. The wrapper therefore needs two inputs: the
APK (for the runtime library) and your own IPA (for the game).

> **Status: untested.** This retarget was produced without a Switch or a
> devkitPro toolchain. Static import coverage is complete (345/345), but
> nothing has been compiled or run. Expect a debugging cycle driven by
> `infinityblade3_nx.log`. See `docs/PORTING.md` for known risks.

The repository does not include the game, APK, IPA, libraries or assets. You
must provide your own legally obtained copies.

---

## Controls

Controller inputs are translated into the taps and swipes used by the original
mobile interface. The touchscreen remains available in handheld mode.

| Input | Normal mode | Cursor mode |
| --- | --- | --- |
| **Touchscreen** | Native touch controls | Native touch controls |
| **Left Stick** | Short combat swipe | Move the cursor |
| **Right Stick** | Short camera swipe | Shorter camera swipe |
| **D-Pad** | Directional swipe | Directional swipe |
| **A** | Tap the centre | Click at the cursor |
| **B** | Hold shield | Hold the bottom-right control |
| **Y / X** | Sword / magic | Sword / magic |
| **ZL / ZR** | Dodge left / right | Click at the cursor |
| **+ / –** | Pause / unpause | Pause / unpause |
| **L** | — | Recenter the cursor |
| **R** | Enable cursor mode | Return to normal mode |

Each stick or D-Pad flick sends one swipe and must return to centre before
another swipe is sent. Hold-style mappings remain pressed until the physical
button is released.

---

## Build

### Requirements

* devkitPro
* devkitA64 and libnx
* Switch Mesa and libdrm_nouveau
* Switch SDL2, mpg123, zlib and libpng
* GNU Make

Install the required devkitPro packages:

```bash
pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-mpg123 switch-zlib switch-libpng
```

Compile the wrapper:

```bash
cd infinityblade_nx
make -j
```

For a clean rebuild:

```bash
make clean
make -j
```

---

## Running

Prepare the runtime directory on a PC (this also validates the APK's
`libib3.so` by size and SHA-256 and checks the IPA is Infinity Blade III):

```bash
python3 tools/prepare_runtime.py InfinityBladeIII-Android-1_2_1.apk \
    --ipa "Infinity Blade III.ipa" --output runtime --nro infinityblade3_nx.nro
```

Copy the result to the SD card:

```text
sd:/switch/infinityblade3_nx/
├── infinityblade3_nx.nro
├── cursor.png
├── libib3.so
├── game/Payload/SwordGame.app/...
└── SaveData/
```

The game needs roughly 3 GB on the SD card. Launch the NRO through title
override for full application memory: hold **R** while opening an installed
game, then start **Infinity Blade III** from the Homebrew Menu.

---

## Status

Not yet verified on hardware. Known open items:

* Guest memory: Horizon has no `mmap`; mappings are served from the process
  heap (see `source/ib3_shim.c`). Large lazy reservations will fail.
* Movie/cutscene playback: NDK `AMediaCodec`/`AMediaExtractor` are stubbed as
  unavailable.
* Signals are accepted but never delivered.
* Controller mapping is inherited from Infinity Blade 1 and is unverified
  against IB3's layout.

---

## Credits

**Infinity Blade Nintendo Switch port** — aks796 (original IB1 wrapper this project is adapted from)

**Infinity Blade Android port** — InfinityBladeGuy

The loader and compatibility layer derive from the open-source Switch `.so`
loader work by Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita and Switch loader work. The inherited wrapper code is
MIT-licensed.

**Infinity Blade** was developed by Chair Entertainment and Epic Games and
published by Epic Games.

---

## Contributing

Bug reports and tested improvements are welcome. Include the build version,
steps to reproduce and the relevant `infinityblade_nx.log` when reporting an
issue.

---

## Disclaimer

This is an unofficial fan project and is not affiliated with, sponsored by or
endorsed by Chair Entertainment or Epic Games. Infinity Blade and all related
artwork, audio, trademarks and game assets belong to their respective owners.

This repository contains only the compatibility code required by the Nintendo
Switch port and does not distribute proprietary game files.
