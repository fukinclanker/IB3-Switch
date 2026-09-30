<div align="center"> <img src="resources/icon.jpg" alt="Infinity Blade III" width="140">
infinityblade3_nx

Infinity Blade III on Nintendo Switch

Unofficial Nintendo Switch wrapper for the Android libib3.so runtime.

Port by her_kirby

</div>
About

infinityblade3_nx adapts the Android Infinity Blade III runtime to Nintendo Switch/Horizon OS.

libib3.so comes from the Android port and provides the runtime used to run the original iOS version of Infinity Blade III. The port requires both the Android APK and a user-supplied IPA.

No game files, APKs, IPAs, libraries, or assets are included.

Status: Working on Nintendo Switch. Minor visual glitches may occur but do not affect gameplay.

Controls
Input	Action
Touchscreen	Native touch controls
Left Stick	Combat swipe / cursor movement
Right Stick	Camera swipe
D-Pad	Directional swipe
A	Tap / cursor click
B	Shield
X / Y	Sword / magic
ZL / ZR	Dodge / cursor click
+ / –	Pause
L	Recenter cursor
R	Toggle cursor mode
Build
Requirements

devkitPro / devkitA64

libnx

Switch Mesa / libdrm_nouveau

Switch SDL2

mpg123

zlib

libpng

GNU Make

Install dependencies:

pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-mpg123 switch-zlib switch-libpng


Build:

make -j


Clean rebuild:

make clean
make -j

Runtime Setup

Prepare the runtime with your own APK and IPA:

python3 tools/prepare_runtime.py InfinityBladeIII-Android-1_2_1.apk \
    --ipa "Infinity Blade III.ipa" \
    --output runtime \
    --nro infinityblade3_nx.nro


Copy the resulting files to:

sd:/switch/infinityblade3_nx/
├── infinityblade3_nx.nro
├── cursor.png
├── libib3.so
├── game/
└── SaveData/


Requires approximately 3 GB of SD card space.

For full application memory, launch through title override by holding R while starting an installed game, then launch the NRO from the Homebrew Menu.

Credits

Infinity Blade III Switch port: her_kirby

Original Infinity Blade Switch wrapper: aks796

Infinity Blade Android port: InfinityBladeGuy

The loader and compatibility code also builds on open-source Switch .so loader work by Andy Nguyen, fgsfds, ChanseyIsTheBest, and TheOfficialFloW.

Infinity Blade was developed by Chair Entertainment and Epic Games.

Disclaimer

This is an unofficial fan project and is not affiliated with or endorsed by Chair Entertainment or Epic Games.

This repository does not distribute proprietary game files or assets. Users must provide their own legally obtained copies.
