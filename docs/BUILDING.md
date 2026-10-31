# Building guide

From your own copy of the disc to a Windows executable and/or a `.cia` for the New 3DS.
Every command runs from the project folder.

> [!IMPORTANT]
> Everything this guide produces (extracted files, generated code, builds, CIAs) comes
> from your ISO and **must not be shared**.

**Contents**
1. [Install the tools](#1-install-the-tools)
2. [Prepare the game](#2-prepare-the-game) (both platforms)
3. [Windows](#3-windows)
4. [New 3DS](#4-new-3ds)
5. [Something went wrong?](#5-something-went-wrong)

---

## 1. Install the tools

| Tool | Needed for | Notes |
|---|---|---|
| [Python](https://www.python.org/) 3.10+ | both | tick **"Add python.exe to PATH"** while installing |
| [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) or its Build Tools, with **"Desktop development with C++"** | Windows | found automatically, no setup needed |
| [devkitPro](https://devkitpro.org/wiki/Getting_Started), group **3ds-dev** | 3DS | brings the compiler, libctru and citro3d |
| `makerom.exe` from [Project_CTR](https://github.com/3DSGuy/Project_CTR/releases) | 3DS | extract it to `tools/bin/makerom.exe` |
| [Azahar](https://azahar-emu.org/) | optional | to try the 3DS build on your PC |

And the game: a *New Super Mario Bros. Wii* disc image, **USA rev 2 (SMNE01)**,
**unencrypted**. Other regions and revisions will not work.

## 2. Prepare the game

Put the ISO in the project folder as `nsmbwii.iso`, then:

**① Extract the disc** into `game/`:

```bash
python tools/extract_disc.py nsmbwii.iso game
```

> If it says the image is encrypted, convert it in Dolphin first:
> right click the game → *Convert File…* → ISO.

**② Recompile the game code** into `build/gen/` (a couple of minutes):

```bash
python -m recomp game build/gen
```

This also writes `build/gen/rel_image.bin`, which you will need next to the ISO.
You only do these two steps once (again only if `recomp/` or `config/` change).

## 3. Windows

**① Compile** (about 10 minutes the first time, then only what changed):

```bash
python tools/build_pc.py
```

**② Put the game data in `pcdata/`**:

```
pcdata/
├── game.iso          your nsmbwii.iso, renamed
└── rel_image.bin     copied from build/gen/
```

> 💡 To avoid a second copy of the ISO, link it instead, from the Command Prompt:
> `mklink /H pcdata\game.iso nsmbwii.iso`

**③ Play**:

```bash
build\pc\nsmbw_pc.exe
```

Saves go to `%LOCALAPPDATA%\NSMBWii\`. Controls are in the
[README](../README.md#controls).

## 4. New 3DS

> [!NOTE]
> Only a **New** 3DS / 2DS with custom firmware (Luma3DS). The Old 3DS does not have
> enough memory.

**① Open the devkitPro shell**: Start menu → *devkitPro* → **MSYS2**. Git Bash or
PowerShell will not work. Go to the project folder (adjust the path):

```bash
cd /c/Users/YOURNAME/Documents/GitHub/NSMBWii-3DS-Recomp
```

**② Compile.** Pick one:

| Command | You get | Game data |
|---|---|---|
| `make cia-full` ⭐ | `nsmbw3ds_full.cia` (~375 MB) | **inside the CIA**: nothing else to copy. Do [step 3 ②](#3-windows) first, it is taken from `pcdata/` |
| `make cia` | `nsmbw3ds.cia` (~16 MB) | on the SD card (see below) |
| `make cxi` | `nsmbw3ds.cxi` | on the SD card; opens directly in Azahar |

```bash
make cia-full
```

The first build takes a while: the generated code is big.

> [!WARNING]
> Ignore the `.3dsx` that a plain `make` produces: it only gets 64 MB and cannot run
> the game. Use the `.cia` or `.cxi`.

**③ Install on the console**
1. Copy the `.cia` to the SD card and install it with **FBI**.
2. Only with `make cia`: also copy the game data to the SD card:
   ```
   SD:/3ds/nsmbw/
   ├── game.iso        your nsmbwii.iso, renamed (FAT32: must be under 4 GB)
   └── rel_image.bin   copied from build/gen/
   ```
3. For audio, `SD:/3ds/dspfirm.cdc` must exist (dump it with **DSP1**).
4. Start it from the HOME Menu.

Saves go to `SD:/3ds/nsmbw/save/`, the log to `SD:/3ds/nsmbw/log.txt`.

**Or try it in Azahar**
1. *Emulation → Configure → System*: turn on **New 3DS mode**.
2. Set **CPU clock speed to 300%**. A real New 3DS runs at 804 MHz; at 100% Azahar
   emulates 268 MHz.
3. *File → Install CIA…*, or *File → Load File…* for the `.cxi`.
4. With `make cia` / `make cxi`, the data goes in `%APPDATA%\Azahar\sdmc\3ds\nsmbw\`.

## 5. Something went wrong?

| Problem | Fix |
|---|---|
| `build_pc.py` cannot find `vswhere` or `cl` | install Visual Studio with the C++ workload (step 1) |
| `make` complains about `DEVKITARM` | you are in the wrong shell: use devkitPro → MSYS2 |
| `makerom.exe: No such file` | put it in `tools/bin/makerom.exe` |
| the PC build closes saying `game.iso` is missing | check `pcdata/game.iso` |
| `bad header (rebuild with the same recompiler version)` | copy `rel_image.bin` again from `build/gen/` after recompiling |
| "not enough memory" on 3DS | you started the `.3dsx`, or it is an Old 3DS |
| very slow on 3DS | expected for now (see the [performance report](3DS_PERFORMANCE_REPORT.md)); in Azahar use 300% CPU |

Still stuck? Open an issue with `log.txt` (and `crash.txt` / `hang.txt` on 3DS, if
present).

---

**In short**

```bash
python tools/extract_disc.py nsmbwii.iso game
python -m recomp game build/gen
python tools/build_pc.py          # Windows
make cia-full                     # 3DS, from the devkitPro MSYS2 shell
```

Developer details (how it works, debug options): [DEVELOPMENT.md](DEVELOPMENT.md).
