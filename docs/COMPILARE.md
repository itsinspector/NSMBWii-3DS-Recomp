# Come compilare NSMBW 3DS (PC e 3DS)

Questa guida porta dalla tua copia del disco a un eseguibile per PC e/o a un
`.cia` per New 3DS. Tutti i comandi si lanciano dalla cartella del progetto.

> Il repository contiene solo il ricompilatore e il runtime. Tutto ciò che
> deriva dal gioco (file estratti, codice generato, build, CIA) si genera in
> locale dalla tua ISO e **non va condiviso**.

## 0. Cosa ti serve

| Cosa | Per | Note |
|---|---|---|
| ISO di *New Super Mario Bros. Wii* **USA rev 2 (SMNE01)**, **non cifrata** | tutto | il ricompilatore è fatto per questa versione esatta |
| [Python](https://www.python.org/) 3.10 o più recente | tutto | spunta "Add python.exe to PATH" durante l'installazione |
| [Visual Studio 2022](https://visualstudio.microsoft.com/it/downloads/) (anche solo *Build Tools*) con il carico di lavoro **"Sviluppo di applicazioni desktop con C++"** | PC | lo script trova MSVC da solo con `vswhere` |
| [devkitPro](https://devkitpro.org/wiki/Getting_Started) con il gruppo **3ds-dev** | 3DS | include devkitARM, libctru, citro3d e picasso |
| `makerom.exe` da [Project_CTR](https://github.com/3DSGuy/Project_CTR/releases) (`makerom-…-win_x86_64.zip`) | 3DS | va messo in `tools/bin/makerom.exe` |
| [Azahar](https://azahar-emu.org/) (facoltativo) | provare il 3DS su PC | |

Per controllare che Python sia a posto, apri un terminale nella cartella del
progetto:

```bash
python --version
```

## 1. Estrai il disco

Copia l'ISO nella cartella del progetto (per esempio `nsmbwii.iso`) ed estraila
in `game/`:

```bash
python tools/extract_disc.py nsmbwii.iso game
```

Se lo script si lamenta della cifratura, la tua ISO è cifrata: va prima
decifrata (ad esempio con Dolphin, "Converti file…" in ISO).

## 2. Ricompila il codice del gioco

Traduce il codice PowerPC del gioco in C (`build/gen/`) e prepara
`build/gen/rel_image.bin`. Ci vogliono un paio di minuti:

```bash
python -m recomp game build/gen
```

Questo passaggio è comune a PC e 3DS. Va rifatto solo se cambia il
ricompilatore (`recomp/` o `config/`).

## 3. Build per PC (Windows)

### 3.1 Compila

```bash
python tools/build_pc.py
```

La prima volta compila tutto (circa 10 minuti); le volte successive solo i
file modificati. Il risultato è `build\pc\nsmbw_pc.exe`.

### 3.2 Prepara la cartella dei dati

Il gioco cerca i dati in `pcdata/` (oppure nella cartella indicata dalla
variabile `NSMBW_DATA`). Servono due file:

```bash
mkdir pcdata
```

```bash
copy nsmbwii.iso pcdata\game.iso
```

```bash
copy build\gen\rel_image.bin pcdata\rel_image.bin
```

(Al posto della copia dell'ISO puoi usare un collegamento per non occupare
altri 350 MB: `mklink /H pcdata\game.iso nsmbwii.iso` dal Prompt dei comandi.)

La NAND con i salvataggi (`pcdata/nand/`) viene creata al primo avvio.

### 3.3 Avvia

```bash
build\pc\nsmbw_pc.exe
```

Comandi (Wii Remote in orizzontale): frecce = croce, `Z` = salto (2),
`X` = corsa (1), `C` = scuoti, `Invio` = +, `Backspace` = −, `A`/`B` = A/B.

Le variabili d'ambiente utili (renderer, risoluzione, profiler, audio) sono
elencate nel [README](../README.md).

## 4. Build per New 3DS

### 4.1 Prepara makerom

Scarica lo zip di makerom per Windows dalle release di Project_CTR ed estrai
`makerom.exe` in `tools/bin/`:

```
tools/bin/makerom.exe
```

### 4.2 Compila dalla shell di devkitPro

I comandi `make` vanno lanciati dalla shell **MSYS2 di devkitPro** (nel menu
Start: *devkitPro → MSYS2*), non da Git Bash o PowerShell, altrimenti non
trova devkitARM. Nella shell spostati nella cartella del progetto, per esempio:

```bash
cd /c/Users/TUONOME/Desktop/Nsmbwii3dsport
```

Poi scegli il formato:

| Comando | Produce | Dati del gioco |
|---|---|---|
| `make cia` | `nsmbw3ds.cia` (~16 MB) | letti dalla SD: `sdmc:/3ds/nsmbw/game.iso` e `rel_image.bin` |
| `make cia-full` | `nsmbw3ds_full.cia` (~375 MB) | dentro il CIA (copiati da `pcdata/`, quindi fai prima il punto 3.2) |
| `make cxi` | `nsmbw3ds.cxi` | come `make cia`, comodo da aprire direttamente in Azahar |

Per esempio:

```bash
make cia-full
```

La prima compilazione richiede parecchi minuti (il codice generato è grande).

> Il `.3dsx` prodotto da `make` da solo **non funziona**: un `.3dsx` riceve
> 64 MB, meno della RAM della Wii. Servono `.cxi`/`.cia`, che chiedono la
> modalità memoria estesa del New 3DS (178 MB). Sull'Old 3DS il gioco non
> entra in memoria.

### 4.3 Installa su New 3DS (con CFW)

1. Copia il `.cia` sulla SD e installalo con **FBI**.
2. Se hai usato `make cia` (non `cia-full`), copia anche i dati sulla SD:
   `sdmc:/3ds/nsmbw/game.iso` e `sdmc:/3ds/nsmbw/rel_image.bin`.
3. Per l'audio serve `sdmc:/3ds/dspfirm.cdc` (si estrae dalla console con
   DSP1).
4. Avvia il gioco dal menu HOME. Salvataggi e log finiscono in
   `sdmc:/3ds/nsmbw/`.

### 4.4 Provalo in Azahar

1. *Emulazione → Configura → Sistema*: attiva **New 3DS mode**.
2. Porta la **velocità clock CPU al 300%**: il New 3DS vero va a 804 MHz,
   Azahar al 100% ne emula 268 e ignora la richiesta del gioco.
3. *File → Installa CIA…* e avvia dalla lista giochi, oppure *File → Carica
   file…* sul `.cxi`.
4. Con `make cia`/`make cxi` i dati vanno in
   `%APPDATA%\Azahar\sdmc\3ds\nsmbw\` (con `cia-full` non serve).
5. Il log del gioco è in `%APPDATA%\Azahar\sdmc\3ds\nsmbw\log.txt`.

## 5. Problemi comuni

| Sintomo | Causa e soluzione |
|---|---|
| `build_pc.py` non trova `vswhere` o `cl` | manca Visual Studio con il carico C++ (punto 0) |
| `make: … DEVKITARM` | stai usando la shell sbagliata: usa MSYS2 di devkitPro |
| `makerom.exe: No such file` | manca `tools/bin/makerom.exe` (punto 4.1) |
| il PC si chiude subito dicendo che manca `game.iso` | cartella dati non trovata: controlla `pcdata/` o `NSMBW_DATA` |
| `bad header (rebuild with the same recompiler version)` | `rel_image.bin` vecchio: ricopialo da `build/gen/` dopo aver ricompilato |
| sul 3DS "not enough memory" | stai avviando il `.3dsx`, o sei su un Old 3DS: usa `.cia`/`.cxi` su New 3DS |
| sul 3DS va molto lento | normale per ora; in Azahar alza il clock CPU al 300% |

## In breve

```bash
python tools/extract_disc.py nsmbwii.iso game
```

```bash
python -m recomp game build/gen
```

```bash
python tools/build_pc.py
```

E per il 3DS, dalla shell MSYS2 di devkitPro:

```bash
make cia-full
```
