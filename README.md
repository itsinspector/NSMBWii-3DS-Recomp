<div align="center">

# NSMBWii-3DS-Recomp

**New Super Mario Bros. Wii, running natively on the New Nintendo 3DS and on Windows**

*A static recompilation: the game's PowerPC code is translated to C ahead of time
and compiled for the 3DS's ARM11, no emulator involved.*

![New 3DS](https://img.shields.io/badge/New%203DS-in%20progress-orange)
![Windows](https://img.shields.io/badge/Windows-playable-brightgreen)
![Status](https://img.shields.io/badge/status-experimental-yellow)

</div>

> [!IMPORTANT]
> **No game included.** This repository holds only the recompiler and the runtime.
> You need your own copy of the disc. Everything built from it (extracted files,
> generated code, `rel_image.bin`, `.cia`/`.cxi` files) stays on your computer and
> **must not be shared**.

---

## What works

| | 🖥️ Windows | 🎮 New 3DS |
|---|---|---|
| Menus, title screen, file select | ✅ 60 fps | ✅ |
| World map and levels | ✅ 60 fps | ⚠️ runs, slow |
| Graphics (lighting, water, animated blocks) | ✅ | ✅ mostly |
| Audio | ✅ | ⚠️ needs `dspfirm.cdc`, untested |
| Save data | ✅ | ✅ |
| Old 3DS / 2DS | — | ❌ not enough memory |

**How fast is the 3DS build?** Measured in the Azahar emulator: about 6–14 fps in
menus and light scenes and 1–3 fps in the heaviest ones. A real New 3DS should do
better than Azahar, but it is still far from full speed. The work so far, and what
comes next, is in the [performance report](docs/3DS_PERFORMANCE_REPORT.md).

## Get started

You need:
- your own *New Super Mario Bros. Wii* disc image, **USA rev 2 (SMNE01)**, unencrypted;
- **Python 3.10+** and, depending on the target, **Visual Studio** (PC) or
  **devkitPro** (3DS).

Then it is four steps:

```bash
python tools/extract_disc.py nsmbwii.iso game     # 1. extract the disc
python -m recomp game build/gen                   # 2. recompile the game code
python tools/build_pc.py                          # 3a. build for Windows
make cia-full                                     # 3b. or for the 3DS (devkitPro shell)
```

👉 **Step-by-step guide, with every tool and where each file goes:
[docs/BUILDING.md](docs/BUILDING.md)**

## Where the files go

**Windows**: next to the project, in `pcdata/`:

```
pcdata/
├── game.iso          your disc image
└── rel_image.bin     from build/gen/
```

Save data is kept in `%LOCALAPPDATA%\NSMBWii\`.

**3DS**: with `nsmbw3ds_full.cia` the game data is already inside, nothing else to
copy. With the plain `nsmbw3ds.cia`, put this on the SD card:

```
SD:/3ds/
├── dspfirm.cdc       for audio (dump it from your console with DSP1)
└── nsmbw/
    ├── game.iso      your disc image (FAT32: must be under 4 GB)
    └── rel_image.bin from build/gen/
```

Save data goes to `SD:/3ds/nsmbw/save/`, the log to `SD:/3ds/nsmbw/log.txt`.

## Controls

The Wii Remote is held sideways, as the game expects.

| Wii Remote | 🖥️ Keyboard | 🎮 3DS |
|---|---|---|
| D-pad | arrow keys | D-pad / Circle Pad |
| 2 (jump) | `Z` | A / B |
| 1 (run, fireball) | `X` | Y |
| Shake (spin, pick up) | `C` | X / L / R |
| + (pause) | `Enter` | START |
| − | `Backspace` | SELECT |
| A / B (menus) | `A` / `B` | — |
| Quit | close the window | START + SELECT |

## FAQ

<details>
<summary><b>Does it work on an Old 3DS or 2DS?</b></summary>

No. The game needs about 125 MB (the Wii's 88 MB of RAM plus the recompiled code),
and an Old 3DS gives an application at most 96 MB. Its CPU is also three times slower
than the New 3DS's.
</details>

<details>
<summary><b>Why not a <code>.3dsx</code> for the Homebrew Launcher?</b></summary>

A `.3dsx` only gets 64 MB. The `.cia`/`.cxi` ask for the New 3DS's extended memory
mode (178 MB), which the game needs.
</details>

<details>
<summary><b>Which version of the game?</b></summary>

Only **USA rev 2 (SMNE01)**. The recompiler targets this exact executable; other
regions or revisions have different code.
</details>

<details>
<summary><b>The game crashed or froze. What should I send?</b></summary>

On 3DS: `SD:/3ds/nsmbw/log.txt`, plus `crash.txt` / `hang.txt` from the same folder
if they exist, or the Luma3DS dump from `SD:/luma/dumps/arm11/`. On Windows:
`pcdata/log.txt`. Say which build you used: crash addresses are only readable with
the matching `.elf`.
</details>

<details>
<summary><b>Is this an emulator?</b></summary>

No. An emulator reads the game's PowerPC instructions while it runs. Here every game
function was translated to C beforehand and compiled to native code. The runtime
replaces the Wii's operating system and hardware: graphics (GX → OpenGL or the 3DS
GPU), audio, disc, saves, Wii Remote. Technical details are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
</details>

## Documentation

| | |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | building for Windows and 3DS, step by step |
| [docs/3DS_PERFORMANCE_REPORT.md](docs/3DS_PERFORMANCE_REPORT.md) | where the 3DS time goes, what was optimized, what is next |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | how the recompiler and runtime work, project layout, debug options |

## Credits

- [NSMBW-Decomp](https://github.com/NSMBW-Community/NSMBW-Decomp) and the Newer team,
  for the symbol maps used to name functions.
- [Dolphin](https://dolphin-emu.org/), for `totaldb.dsy` (GPL) and its documentation
  of the Wii hardware.
- [devkitPro](https://devkitpro.org/), libctru and citro3d for the 3DS toolchain;
  [Project_CTR](https://github.com/3DSGuy/Project_CTR) for makerom.

*New Super Mario Bros. Wii is a trademark of Nintendo. This project is not affiliated
with or endorsed by Nintendo.*
