# NSMBW → 3DS (static recompilation)

Port sperimentale di *New Super Mario Bros. Wii* (SMNE01, USA rev 2) su **New 3DS**,
tramite ricompilazione statica del codice PowerPC in C, compilato poi per ARM11.
Lo stesso runtime compila anche per Windows (MSVC) come build di debug.

> Il repository non contiene codice né asset Nintendo. Tutto ciò che deriva dal
> gioco (`game/`, `build/gen/`, `rel_image.bin`, `pcdata/`) viene generato in locale
> a partire dalla propria copia del disco e non va distribuito.

## Struttura

| Percorso | Contenuto |
|---|---|
| `tools/extract_disc.py` | estrae il filesystem da un'ISO Wii **non cifrata** |
| `recomp/binfmt.py` | parser DOL / REL, rilocazioni REL, decompressore LZ11 |
| `recomp/analyze.py` | DOL + 4 REL in uno spazio di indirizzi unico, scoperta funzioni e jump table |
| `recomp/lift.py` | lifter PowerPC (Gekko/Broadway, paired singles inclusi) → C |
| `recomp/symbols.py` | simboli della community (PAL v1) convertiti per USA rev 2 |
| `recomp/sigdb.py` | riconoscimento funzioni SDK con il database di firme di Dolphin |
| `config/symbols.txt` | funzioni identificate a mano (con la motivazione) |
| `config/hle.txt` | funzioni guest sostituite da codice nativo |
| `runtime/src/hle_os.c` | thread, contesti, interrupt, decrementer |
| `runtime/src/hle_ios.c` | IOS: `/dev/di` (dall'ISO), `/dev/fs` + NAND, ES, STM |
| `runtime/src/hle_input.c` | Wii Remote (KPAD/WPAD) dai tasti host |
| `runtime/src/hle_rel.c` | `OSLink` verso le REL pre-rilocate |
| `runtime/src/hw.c` | registri hardware: PI, VI, PE/CP, AI, EXI, FIFO GX, DMA della locked cache |
| `runtime/src/gx.c` | command processor GX: registri CP/XF/BP, display list, TLUT, copie EFB |
| `runtime/src/gx_vertex.c` | vertex loader, trasformazioni, illuminazione, texgen (su CPU) |
| `runtime/src/gx_texture.c` | decodifica texture GX (tutti i formati) con cache |
| `runtime/src/dsp.c` | DSP a livello di mailbox (boot ROM, protocollo AX) e DMA dell'AI |
| `runtime/src/ax_hle.c` | microcode AX in HLE: voci ADPCM/PCM, resampling, inviluppi, filtri, mixer |
| `runtime/src/gfx_gl.c` | backend OpenGL 3.3 (PC, predefinito): thread di rendering, TEV → GLSL |
| `runtime/src/gfx_soft.c` | renderer software di riferimento (PC): TEV completa, PNG dei frame |
| `runtime/src/gfx_c3d.c` | backend 3DS citro3d: EFB come render target, TEV → 6 combiner PICA |
| `runtime/src/platform_*.c` | 3DS (libctru) e PC (Win32, con watchdog per il debug) |
| `third_party/` | mappe simboli della community e `totaldb.dsy` di Dolphin |

## Uso

Guida passo passo per compilare e avviare (PC e 3DS): [docs/COMPILARE.md](docs/COMPILARE.md).

```bash
python tools/extract_disc.py nsmbwii.iso game
python -m recomp game build/gen
make cxi                      # 3DS: .cxi con la memoria estesa del New 3DS (178 MiB)
make cia                      # oppure .cia da installare (shell msys2 di devkitPro)
python tools/build_pc.py      # PC, --trace per il ring buffer delle funzioni
```

Il `.3dsx` non basta: riceve solo 64 MiB, meno della RAM della Wii. `makerom` va messo in
`tools/bin/` (release di github.com/3DSGuy/Project_CTR). Su Azahar: attivare New 3DS mode e
aprire `nsmbw3ds.cxi`; i dati vanno in `%APPDATA%\Azahar\sdmc\3ds\nsmbw\`.

Dati a runtime (`sdmc:/3ds/nsmbw/` su 3DS, `pcdata/` o `$NSMBW_DATA` su PC):
- `game.iso`: l'ISO non cifrata (il disco si legge direttamente da lì)
- `rel_image.bin`: generato dal recompiler
- `nand/`: creata al primo avvio (salvataggi, impostazioni)

Controlli su PC (finestra del gioco o console in primo piano), Wii Remote in orizzontale:
frecce = croce, `Z` = salto (2), `X` = corsa (1), `C` = scuotere, `Invio` = +,
`Backspace` = −, `A` / `B` = A / B.

Prestazioni e rendering: `NSMBW_RENDERER=soft` (renderer software invece di OpenGL),
`NSMBW_RES_SCALE=1..4` (risoluzione interna OpenGL, predefinita 2), `NSMBW_NOAUDIO=1`,
`NSMBW_NOWINDOW=1`, `NSMBW_PROFILE=<sec>` (profiler a campionamento nel log, utile per
trovare i colli di bottiglia), `NSMBW_RENDER_THREADS=<n>` (renderer software).

Variabili di debug: `NSMBW_DUMP_FRAMES=<N>` (PNG ogni N frame in `pcdata/frames/`,
`tools/crop.py` per ingrandire), `NSMBW_FAST=1` (rasterizza solo vicino ai dump),
`NSMBW_WATCHDOG=<sec>` (stack dei thread guest, da passare a `tools/stacks.py`),
`NSMBW_INPUT="vsync:mask,..."` (input scriptato: 16 = tasto 2, 512 = A), `NSMBW_MAX_FRAMES`,
`NSMBW_DEBUG_GX`, `NSMBW_DEBUG_DI`, `NSMBW_DEBUG_DSP`, `NSMBW_DEBUG_COPY`, `NSMBW_DUMP_MATS`, `NSMBW_TRACE_DEPTH`.

## Modello di esecuzione

- Ogni funzione guest diventa `void f_XXXXXXXX(PPCContext *c)`.
- Memoria guest (MEM1 24 MiB + MEM2 64 MiB) big-endian, mappata a pagine da 64 KiB.
- Le REL sono rilocate da `0x93400000`; i `bl` verso il DOL prendono il bersaglio dalle rilocazioni.
- **Thread**: lo scheduler originale resta ricompilato; ogni contesto guest vive su un
  thread host, `OSSaveContext` è un `setjmp` nel frame del chiamante.
- **Interrupt**: consegnati in punti sicuri chiamando i veri handler del gioco.
- **Grafica**: il FIFO GX viene interpretato, vertici trasformati su CPU (lo stato
  derivato dai registri CP/XF si ricalcola solo quando cambiano), poi il backend
  (OpenGL o software su PC, citro3d su 3DS) disegna.
- **Audio**: il DSP non viene eseguito; le command list AX vengono interpretate in C
  (frame da 3 ms a 32 kHz) e il buffer dell'AI va all'audio host (waveOut / ndsp).

## Stato

- [x] Ricompilazione completa (~38k funzioni), simboli (~2.1k nomi)
- [x] HLE OS, IOS (disco, NAND, ES, STM), Wii Remote, `OSLink`
- [x] GX: renderer software di riferimento su PC; strap screen, title screen con
      modelli 3D illuminati, selezione file e giocatori sono corretti
- [x] Backend citro3d per 3DS (compila; **da verificare su hardware/emulatore**)
- [x] Audio: DSP/AX in HLE, musica ed effetti su PC
- [x] Backend OpenGL su PC: 60 fps nei menu, ~30 fps nella cutscene iniziale (in ottimizzazione)
- [ ] Verifica di mappa del mondo e livelli
- [ ] SYSCONF / setting.txt nella NAND
- [ ] Build CIA con memoria estesa del New 3DS
