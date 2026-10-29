# Building NSMBWii-3DS-Recomp (PC and 3DS)

This guide takes you from your own copy of the disc to a PC executable and/or a
`.cia` for the New 3DS. Run every command from the project folder.

> The repository only contains the recompiler and the runtime. Everything derived
> from the game (extracted files, generated code, builds, CIAs) is generated
> locally from your ISO and **must not be shared**.

## 0. What you need

| What | For | Notes |
|---|---|---|
| *New Super Mario Bros. Wii* disc image, **USA rev 2 (SMNE01)**, **unencrypted** | everything | the recompiler targets this exact version |
| [Python](https://www.python.org/) 3.10 or newer | everything | tick "Add python.exe to PATH" when installing |
| [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) (Build Tools are enough) with the **"Desktop development with C++"** workload | PC | the build script finds MSVC through `vswhere` |
| [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the **3ds-dev** group | 3DS | includes devkitARM, libctru, citro3d and picasso |
| `makerom.exe` from [Project_CTR](https://github.com/3DSGuy/Project_CTR/releases) (`makerom-…-win_x86_64.zip`) | 3DS | goes in `tools/bin/makerom.exe` |
| [Azahar](https://azahar-emu.org/) (optional) | trying the 3DS build on PC | |

To check that Python works, open a terminal in the project folder:

```bash
python --version
```

## 1. Extract the disc

Copy the ISO into the project folder (for example as `nsmbwii.iso`) and extract it
to `game/`:

```bash
python tools/extract_disc.py nsmbwii.iso game
```

If the script says the image is encrypted, decrypt it first (for example in
Dolphin: right click → Convert File… → ISO).

## 2. Recompile the game code

This translates the game's PowerPC code to C (`build/gen/`) and writes
`build/gen/rel_image.bin`. It takes a couple of minutes:

```bash
python -m recomp game build/gen
```

This step is shared by the PC and 3DS builds. Redo it only when the recompiler
(`recomp/` or `config/`) changes.

## 3. PC build (Windows)

### 3.1 Compile

```bash
python tools/build_pc.py
```

The first run compiles everything (about 10 minutes); later runs only rebuild what
changed. The result is `build\pc\nsmbw_pc.exe`.

### 3.2 Set up the data folder

The game looks for its data in `pcdata/` (or in the folder named by the
`NSMBW_DATA` environment variable). It needs two files:

```bash
mkdir pcdata
```

```bash
copy nsmbwii.iso pcdata\game.iso
```

```bash
copy build\gen\rel_image.bin pcdata\rel_image.bin
```

Instead of copying the ISO you can hard-link it to save 350 MB:
`mklink /H pcdata\game.iso nsmbwii.iso` from the Command Prompt.

The NAND with the saves (`pcdata/nand/`) is created on first run.

### 3.3 Run

```bash
build\pc\nsmbw_pc.exe
```

Controls (Wii Remote held sideways): arrows = D-pad, `Z` = jump (2), `X` = run (1),
`C` = shake, `Enter` = +, `Backspace` = −, `A`/`B` = A/B.

The useful environment variables (renderer, resolution, profiler, audio) are listed
in the [README](../README.md).

## 4. New 3DS build

### 4.1 Get makerom

Download the Windows makerom zip from the Project_CTR releases and extract
`makerom.exe` to `tools/bin/`:

```
tools/bin/makerom.exe
```

### 4.2 Compile from the devkitPro shell

Run `make` from the devkitPro **MSYS2** shell (Start menu: *devkitPro → MSYS2*),
not from Git Bash or PowerShell, or it will not find devkitARM. In that shell,
change to the project folder, for example:

```bash
cd /c/Users/YOURNAME/Documents/GitHub/NSMBWii-3DS-Recomp
```

Then pick a format:

| Command | Produces | Game data |
|---|---|---|
| `make cia` | `nsmbw3ds.cia` (~16 MB) | read from the SD card: `sdmc:/3ds/nsmbw/game.iso` and `rel_image.bin` |
| `make cia-full` | `nsmbw3ds_full.cia` (~375 MB) | inside the CIA (copied from `pcdata/`, so do step 3.2 first) |
| `make cxi` | `nsmbw3ds.cxi` | like `make cia`; handy to open directly in Azahar |

For example:

```bash
make cia-full
```

The first build takes a good while (the generated code is large).

> The `.3dsx` that a plain `make` produces **does not work**: a `.3dsx` gets 64 MB,
> less than the Wii's RAM. You need the `.cxi`/`.cia`, which request the New 3DS
> extended memory mode (178 MB). The game does not fit on an Old 3DS.

### 4.3 Install on a New 3DS (custom firmware)

1. Copy the `.cia` to the SD card and install it with **FBI**.
2. If you used `make cia` (not `cia-full`), also copy the data to the SD card:
   `sdmc:/3ds/nsmbw/game.iso` and `sdmc:/3ds/nsmbw/rel_image.bin`.
3. Audio needs `sdmc:/3ds/dspfirm.cdc` (dump it from your console with DSP1).
4. Start the game from the HOME Menu. Saves and the log go to `sdmc:/3ds/nsmbw/`.

### 4.4 Try it in Azahar

1. *Emulation → Configure → System*: enable **New 3DS mode**.
2. Set the **CPU clock speed to 300%**: a real New 3DS runs at 804 MHz, while
   Azahar emulates 268 MHz at 100% and ignores the game's request for more.
3. *File → Install CIA…* and start it from the game list, or *File → Load File…*
   on the `.cxi`.
4. With `make cia`/`make cxi`, the data goes in
   `%APPDATA%\Azahar\sdmc\3ds\nsmbw\` (not needed with `cia-full`).
5. The game's log is `%APPDATA%\Azahar\sdmc\3ds\nsmbw\log.txt`.

## 5. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `build_pc.py` cannot find `vswhere` or `cl` | Visual Studio with the C++ workload is missing (step 0) |
| `make: … DEVKITARM` | wrong shell: use the devkitPro MSYS2 shell |
| `makerom.exe: No such file` | `tools/bin/makerom.exe` is missing (step 4.1) |
| the PC build closes at once saying `game.iso` is missing | data folder not found: check `pcdata/` or `NSMBW_DATA` |
| `bad header (rebuild with the same recompiler version)` | stale `rel_image.bin`: copy it again from `build/gen/` after recompiling |
| "not enough memory" on 3DS | you started the `.3dsx`, or you are on an Old 3DS: use the `.cia`/`.cxi` on a New 3DS |
| very slow on 3DS | expected for now; in Azahar raise the CPU clock to 300% |

## In short

```bash
python tools/extract_disc.py nsmbwii.iso game
```

```bash
python -m recomp game build/gen
```

```bash
python tools/build_pc.py
```

And for the 3DS, from the devkitPro MSYS2 shell:

```bash
make cia-full
```
