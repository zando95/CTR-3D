# CTR 3D

A New Nintendo 3DS homebrew port of Crash Team Racing (PS1, 1999).

This is a fork of [CTR Native](https://github.com/CTR-tools/ctr-native), the native PC port of the game built on the [CTR-ModSDK](https://github.com/CTR-tools/CTR-ModSDK) decompilation. It starts from CTR Native beta 7.1 and adds a 3DS platform layer and a renderer for the 3DS GPU. The game runs at full speed on a New 3DS, with stereoscopic 3D on the slider, a genuinely widescreen top screen, and the map on the bottom screen.

As far as we can determine, this is the first PlayStation decompilation ported to 3DS homebrew. Fittingly, CTR was Nintendo's internal codename for the 3DS.

The PC port is still here and still builds; see [PC builds](#pc-builds-windows-and-linux) at the bottom. Everything above that section is about the 3DS.

## Philosophy

The goal is a game that could pass for a retail 3DS port: faithful to the original where it counts, and willing to leave PlayStation hardware quirks behind where the 3DS can do better.

- The track and the karts are transformed and textured by the 3DS GPU, so the PS1's vertex wobble and texture warping are gone.
- Performance wins over pixel-for-pixel parity. The close-up "mosaic" texture tier is not rendered and the full-screen blur effect is gone, because a steady frame rate matters more.
- Clear bugs in the original get fixed. The mirrored tires in ice reflections sit where the modellers intended, not where a PS1 branch delay slot put them.
- Enhancements are options, not mandates. Texture filtering exists, and defaults off, because players preferred crisp textures to mud.
- Nothing of the game ships with this project. You bring your own disc image.

## Requirements

- A **New** Nintendo 3DS, New 3DS XL, or New 2DS XL. The original 3DS and 2DS do not have the CPU for it and are not supported.
- Custom firmware with the Homebrew Launcher ([Luma3DS](https://github.com/LumaTeam/Luma3DS)). If your console is not modded yet, [3ds.hacks.guide](https://3ds.hacks.guide/) is the place to start.
- A one-time DSP firmware dump, or the game will be silent: open the Rosalina menu (default **L + Down + Select**), choose Miscellaneous options, then Dump DSP firmware.
- Your own NTSC-U (North American) retail CTR disc image, in the common raw PSX BIN layout: MODE2/2352 sectors, data track starting at byte 0. A cooked 2048-byte `.iso` drops the XA and STR sectors that music, voices and video need.

## Install

1. Download the zip from the Releases page and extract it onto the root of your SD card. It mirrors the card's layout, so the game lands at `/3ds/ctr_native/ctr_native.3dsx`.
2. Copy your disc image to `/3ds/ctr_native/assets/ctr-u.bin`. The `assets` folder contains a text file marking the spot.
3. Launch it from the Homebrew Launcher, where it appears as CTR 3D.

Saves are written to `/3ds/ctr_native/memcards/`, in the same files the PC port uses, so progress can be copied between console and PC.

The Sony logo screen holds for several seconds after launch while the game's audio is cached; a `Loading audio` line shows its progress. On a slow SD card it takes longer. It happens once per launch.

## Playing

Buttons follow their position on the PlayStation pad, not their letter:

| 3DS | PS1 | In a race | In menus |
|---|---|---|---|
| B | Cross | Accelerate | Confirm |
| A | Circle | Use weapon | Confirm |
| Y | Square | Brake, reverse | Back |
| X | Triangle | Switch the top screen between speedometer and map | Back |
| L, R | L1, R1 | Hop, power-slide | |
| ZL | L2 | Change camera view | |
| ZR | R2 | Hold to look behind | |
| Circle Pad, D-Pad | D-pad | Steer | Navigate |
| START | Start | Pause | |

The screens:

- The bottom screen always shows the in-race map, enlarged. The top screen starts in speedometer mode; X puts the map there too, as Triangle does on PS1. In Adventure mode the hub map moves to the bottom screen as well.
- A frame-rate readout sits in the top-right corner of the bottom screen.
- The 3D slider works in real time.

The main menu has an **OPTIONS** entry with four toggles: PAL PENTA (Penta Penguin's PAL/Japanese stats), RESERVES METER (the boost-reserves bar), BOTTOM MAP, and TEXTURE FILTER. They return to their defaults on every launch.

Below the toggles is a **LANGUAGE** row: confirm cycles through English, French, German, Italian, Spanish and Dutch, and the game text changes immediately. Unlike the toggles, the choice is remembered in `/3ds/ctr_native/language.cfg`; delete that file to go back to English. The OPTIONS menu itself is translated too (title, every option, and ON/OFF as SI/NO, JA/NEIN, OUI/NON and so on). Voices stay English unless you install PAL voices (see below), because the NTSC-U disc has no other voice tracks. VS and Battle modes are not on the menu; one console has one controller.

### PAL voices

The NTSC-U disc only has English speech. To hear the other languages, extract the voices from a PAL disc image of the game:

1. Install Python 3.11 or newer.
2. Put your PAL Crash Team Racing `.bin` dump (raw 2352-byte sectors) next to `tools/extract_pal_voices.py`.
3. Run `python extract_pal_voices.py YOUR_PAL_DUMP.bin`. Add `--language SPN` (and `FRN`, `GRM`, `ITL`, `DCH`) to extract only the languages you want and save SD space. English is not needed.
4. Copy the generated `pal-voices` folder to the SD card so the final path is `/3ds/ctr_native/mods/pal-voices`, with `XA/` directly inside it (for example `/3ds/ctr_native/mods/pal-voices/XA/SPN.XNF` and `/3ds/ctr_native/mods/pal-voices/XA/SPN/GAME/S01.XA`).

Pick a language in OPTIONS and the voices follow. A language whose pack is not installed, and the few lines the PAL disc does not have, play in English. The voices of the language you had selected at launch are preloaded into RAM with the rest of the audio; if you switch language later, the new language's voices are read from the SD card until the next launch, which can add a short hitch when a voice line starts. `sdmc:/ctr3ds.log` records which packs were found (lines starting with `[PALVOICE]`).

The extractor is the one from the PS Vita port ([Rinnegatamante/Crash-Team-Racing-High-Octane](https://github.com/Rinnegatamante/Crash-Team-Racing-High-Octane)), which also defined the PAL voice mapping used here.

## Reporting a bug

Open an issue on the repository's Issues page. The bug-report form asks for what it needs: console model, Luma3DS version, SD card make and size, the version string shown under the game's name in the Homebrew Launcher, and what you were doing.

To attach a log, create an empty file named `enable-log.txt` next to `ctr_native.3dsx`, launch the game, reproduce the problem, then attach `/ctr3ds.log` from the SD card root. Logging costs a small stutter every couple of seconds, so delete `enable-log.txt` afterwards.

## Changes from the original game

Everything here is on by default unless it says otherwise. The toggles are the OPTIONS menu rows.

- **Widescreen.** The top screen renders a wider field of view, not a stretched 4:3 image.
- **Stereoscopic 3D** on the slider.
- **The map on the bottom screen**, with the top-screen slot switching between speedometer and map. Toggle.
- **Penta Penguin has his PAL and Japanese stats**, the maximum in every class, instead of the NTSC-U ones. Toggle.
- **A boost-reserves meter** next to the power-slide meter, following the community ReservesMeter mod's design. Toggle.
- **Texture filtering.** Toggle, off by default.
- **Ice reflections no longer merge each kart's mirrored wheel pairs onto its centreline.** That is a bug in the shipped PS1 game.
- **The full-screen feedback blur** used by the clock item and the intro flyby's ghost trail is removed. The clock's flash and slowdown remain.
- **The close-up "mosaic" texture tier is not rendered.** Near surfaces use the standard texture.
- **The Spyro 2 demo cheat code does something else now**, since a native build cannot launch the demo off the disc. One new cheat code has been added.
- **Language selection** (English, French, German, Italian, Spanish, Dutch), using the localized text files that are already on the NTSC-U disc. The n-tilde, inverted `¡` `¿`, the ordinal `º` and quotation marks that those files rely on are drawn from existing font icons, because the NTSC-U font code does not know them. Remembered between launches.
- **Voices in the selected language** (French, German, Italian, Spanish, Dutch) from PAL voice packs on the SD card. English keeps the NTSC-U disc's own voices.
- **VS and Battle modes** are not offered.

The PC build from this repository carries the changes that are not 3DS-specific: PAL Penta, the reserves meter, the ice-reflection fix and the cheat codes. On PC they are always on; only the 3DS build has the OPTIONS menu.

## Building from source

### New Nintendo 3DS

Install [devkitPro](https://devkitpro.org/) with the `3ds-dev` package group, then:

```
cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds
```

Output: `build-3ds/ctr_native.3dsx`. That build includes the internal debug surface; add `-DCTR_INTERNAL=OFF` to the configure line to build what the release ships.

On Windows, run both commands from devkitPro's MSYS2 shell using the `msys` `cmake` and `make` packages, not the mingw ones. From plain Git Bash the cross-compiler fails with a temporary-file error.

## PC builds (Windows and Linux)

Ignore this section if you just want to play on 3DS. The PC port is CTR Native beta 7.1 plus the changes listed above.

### Prerequisites

Windows:

1. Install [MSYS2](https://www.msys2.org/)
2. In an MSYS2 terminal:
   ```
   pacman -Syu
   pacman -S --needed git mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-make
   ```
   If the update asks you to close the terminal, reopen MSYS2 and run the install command.
3. Add `C:\msys64\mingw32\bin` to your system PATH

Linux (Debian/Ubuntu):

```
sudo apt install gcc-multilib
sudo apt install libx11-dev libxext-dev libgl1-mesa-dev libasound2-dev libudev-dev libdbus-1-dev
```

SDL3 is compiled from vendored source, so nothing else needs installing.

### Building

```
build.bat            # Windows
chmod +x build.sh
./build.sh           # Linux
```

The first build compiles SDL3 and caches it as a static library in `build/`; later builds recompile only touched sources. For a clean rebuild, delete `build/` first.

Output: `build/ctr_native.exe` (Windows) or `build/ctr_native` (Linux).

### Running

A release build needs only the executable and your disc image beside it:

```
CTR-3D/
  ctr_native.exe
  assets/
    ctr-u.bin
```

For a development build run from `build/`, put `assets/` at the repository root, next to `build/`. The disc image rules are the same as for the 3DS.

Extracted asset files are supported for development and modding, and override the disc image when present: `assets/BIGFILE.BIG`, `assets/SOUNDS/KART.HWL`, `assets/TEST.STR`, `assets/XA/ENG.XNF`, and the `XA/ENG/EXTRA`, `XA/ENG/GAME` and `XA/MUSIC` streams.

Internal builds can record and replay input for bug reports; see [docs/REPLAYS.md](docs/REPLAYS.md).

## Architecture and documentation

```
main.c (entrypoint; one unity translation unit)
  +-- platform/native_*.c      native platform: audio, input, memcard, CD, renderer, PSX facade glue
  +-- platform/n3ds/           3DS backend: libctru/citro3d renderer, audio and render threads
  +-- game/game_unity.h
        +-- game/**            decompiled game source (CTR-ModSDK derived)
              +-- include/**   structs, globals, declarations, platform facade headers
```

The game code talks to the host only through the PlayStation library surface and a small platform contract; `CTR_NATIVE` marks native-side code. The 3DS renderer reimplements the PC renderer's contract against the PICA200's fixed-function GPU, which has neither fragment shaders nor an indexed-texture format, so palette lookup happens in a CPU-side decode cache.

- [docs/3DS_PORT.md](docs/3DS_PORT.md): how the 3DS port works, from threads to renderer to audio.
- [docs/MEMORY_MODEL.md](docs/MEMORY_MODEL.md), [docs/DATA_SECTIONS.md](docs/DATA_SECTIONS.md), [docs/OVERLAYS.md](docs/OVERLAYS.md): how the PS1-shaped memory model survives on a native host, inherited from CTR Native.

## License

GPL-3.0, inherited from CTR Native and CTR-ModSDK; see [LICENSE](LICENSE). Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No game data is included or distributed; you need your own copy of the game.

## Credits

- [CTR Native](https://github.com/CTR-tools/ctr-native) — the native PC port this project is a fork of
- [CTR-ModSDK](https://github.com/CTR-tools/CTR-ModSDK) — the decompilation project both are built on
- [ReservesMeter](https://github.com/CTR-tools/CTR-ModSDK/tree/main/mods/Modules/ReservesMeter) by Superstarxalien — the in-race boost-reserves meter's design (geometry, fill scale, colors) follows this community mod
- [PsyCross](https://github.com/OpenDriver2/PsyCross) — original PS1 compatibility code from which parts of the owned platform layer and PsyQ facade headers are derived
- [SDL3](https://github.com/libsdl-org/SDL) — cross-platform multimedia for the PC build
- [devkitPro](https://devkitpro.org/), libctru and citro3d — the 3DS homebrew toolchain and GPU library the 3DS port is built on
- Crash Team Racing is a trademark of Sony Computer Entertainment / Naughty Dog
