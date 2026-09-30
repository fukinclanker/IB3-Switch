 ## About

 `infinityblade_nx` is a native wrapper that runs the ARM64 Android build of\
 **Infinity Blade** on Nintendo Switch. It recreates the Android, Bionic, audio,\
 input and graphics services expected by the game under Horizon OS.

 This release targets **Infinity Blade Android v1.0.10**, based on Unreal Engine\
 3 and ARM64. Other versions are not expected to work without source changes.

 The repository does not include the game, APK, libraries or assets. You must\
 provide your own legally obtained compatible copy.

---

 ## Controls

 Controller inputs are translated into the taps and swipes used by the original\
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
| **\+ / –** | Pause / unpause | Pause / unpause |
| **L** | — | Recenter the cursor |
| **R** | Enable cursor mode | Return to normal mode |

Each stick or D-Pad flick sends one swipe and must return to centre before\
 another swipe is sent. Hold-style mappings remain pressed until the physical\
 button is released.

---

 ## Build

 ### Requirements

 - devkitPro
- devkitA64 and libnx
- Switch Mesa and libdrm\_nouveau
- Switch SDL2, mpg123, zlib and libpng
- GNU Make

 Install the required devkitPro packages:

```
pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-mpg123 switch-zlib switch-libpng
```

 Compile the wrapper:

```
cd infinityblade_nx
make -j
```

 For a clean rebuild:

```
make clean
make -j
```

---

 ## Preparing the Runtime Files

 Use the included `prepare_runtime.py` script to extract and prepare the required\
 runtime files from your own legally obtained game files:

```
python3 tools/prepare_runtime.py InfinityBladeIII-Android-1_2_1.apk \
  --ipa "Infinity Blade III.ipa" --output runtime --nro infinityblade3_nx.nro
```

 The prepared files will be placed in the `runtime/` directory.

---

 ## Running

 Extract the Android game files from your own compatible copy and create this\
 folder on the SD card:

```
sd:/switch/infinityblade_nx/
├── infinityblade_nx.nro
├── cursor.png
├── libIB1.so
├── libopenal.so
├── assets/
└── SaveData/
```

 Copy the complete `assets/` directory without changing its layout. Launch the\
 NRO through title override for full application memory: hold **R** while opening\
 an installed game, then start **Infinity Blade** from the Homebrew Menu.

---

 ## Status

 Gameplay, saves, assets, audio, dialogue, cutscenes, touchscreen input and\
 controller input are working. Startup and scene transitions can still take\
 longer than on the original platforms.

 The wrapper is built specifically for **Infinity Blade Android v1.0.10**.\
 Libraries and assets from another release are not supported.

---

 ## Credits

 **Infinity Blade Nintendo Switch port** — aks796

 **Infinity Blade Android port** — InfinityBladeGuy

 The loader and compatibility layer derive from the open-source Switch `.so`\
 loader work by Andy Nguyen, fgsfds and ChanseyIsTheBest, building on\
 TheOfficialFloW's Vita and Switch loader work. The inherited wrapper code is\
 MIT-licensed.

 **Infinity Blade** was developed by Chair Entertainment and Epic Games and\
 published by Epic Games.

---

 ## Contributing

 Bug reports and tested improvements are welcome. Include the build version,\
 steps to reproduce and the relevant `infinityblade_nx.log` when reporting an\
 issue.

---

 ## Disclaimer

 This is an unofficial fan project and is not affiliated with, sponsored by or\
 endorsed by Chair Entertainment or Epic Games. Infinity Blade and all related\
 artwork, audio, trademarks and game assets belong to their respective owners.

 This repository contains only the compatibility code required by the Nintendo\
 Switch port and does not distribute proprietary game files.
