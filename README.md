# Duke Nukem 3D for openfpgaOS

Play Duke Nukem 3D on [openfpgaOS](https://github.com/ThinkElastic/openfpgaOS) — the full BUILD engine running natively on RISC-V via openfpgaOS.

## Features

- Full Duke Nukem 3D gameplay (shareware or registered)
- 320×240 rendering at up to 60fps
- 10 save game slots (256KB each, LZW compressed)
- Sound effects (VOC playback)
- MIDI music support
- Runs as a standalone core — own entry in the openfpgaOS menu

## Installation

1. Download the [latest release](https://github.com/thinkelastic/DukeNukem3D/releases)
2. Extract the ZIP to your SD card root
3. Copy `duke3d.grp` to `Assets/duke3d/common/` on the SD card
4. The game appears in the menu under "Duke Nukem 3D"

### Where to get duke3d.grp

- **Shareware:** Download from [3D Realms](https://3drealms.com) or various abandonware sites
- **Registered:** From your original Duke Nukem 3D purchase (Steam, GOG, etc.)

The `.grp` file is the game data archive — it is not included in this release.

## Building from Source

### Requirements

- RISC-V GCC targeting `rv32imafc` / `ilp32f`
  - **Arch:** `pacman -S riscv64-elf-gcc`
  - **macOS:** `brew install riscv64-elf-gcc`
  - **Ubuntu:** `apt install gcc-riscv64-unknown-elf`

### Build

```bash
git clone https://github.com/thinkelastic/DukeNukem3D.git
cd DukeNukem3D
make
```

This builds:
- `build/Duke3D/` — standalone Duke3D core (copy to SD card)
- `build/sdk/` — openfpgaOS shared core with bundled demo apps

### Deploy to SD card

```bash
make deploy
```

Auto-detects the SD card and copies everything. You still need to manually copy `duke3d.grp`.

### Package for distribution

```bash
./package.sh Duke3D
# Creates releases/Duke3D-v1.0.0.zip
```

## SD Card Layout

After installation, your SD card should have:

```
SD Card/
├── Cores/ThinkElastic.Duke3D/
│   ├── bitstream.rbf_r
│   ├── loader.bin
│   ├── core.json
│   └── data.json
├── Assets/duke3d/common/
│   ├── duke3d.elf          ← game binary
│   ├── duke3d.grp          ← game data (you provide this)
│   └── os.bin              ← openfpgaOS kernel
├── Platforms/
│   └── duke3d.json
└── Saves/duke3d/common/
    └── duke3d_0.sav ...    ← created automatically
```

## Controls

| Button | Duke3D |
|--------|--------|
| D-pad | Move / Turn |
| A | Fire |
| B | Open / Use |
| X | Jump |
| Y | Crouch |
| L | Previous weapon |
| R | Next weapon |
| Start | Menu / Pause |
| Select | Map |

## Project Structure

```
DukeNukem3D/
├── src/
│   ├── duke3d/              ← Duke3D source code
│   │   ├── Engine/src/      ← BUILD engine (rendering, file I/O)
│   │   ├── Game/src/        ← Duke3D game logic, menus, AI
│   │   ├── d3d_save.c       ← Save system (openfpgaOS save slots)
│   │   ├── d3d_audio.c      ← Audio (VOC playback via of_mixer)
│   │   └── posix_shim.c     ← POSIX I/O → openfpgaOS syscalls
│   ├── apps/                ← Bundled openfpgaOS demo apps
│   └── sdk/                 ← openfpgaOS SDK (headers, libc, CRT)
├── dist/
│   ├── Duke3D/              ← Standalone core packaging config
│   └── sdk/                 ← Shared openfpgaOS core config
├── runtime/                 ← FPGA bitstream, Chip32 loader, OS binary
└── Makefile
```

## Updating from SDK

This repo tracks the [openfpgaOS SDK](https://github.com/ThinkElastic/openfpgaOS-SDK) as an upstream remote. To pull SDK updates:

```bash
git fetch sdk-upstream
git merge sdk-upstream/main
make clean && make
```

## Technical Details

- **CPU:** VexRiscv RISC-V (rv32imafc, 100 MHz) on Cyclone V FPGA
- **Video:** 320×240 8-bit indexed color, 256-entry palette, double-buffered
- **Audio:** 48 kHz stereo PCM, 4-channel mixer
- **Memory:** 64MB SDRAM, 2.5MB CRAM1 PSRAM (saves)
- **Engine:** BUILD engine (Ken Silverman), adapted for bare-metal RISC-V
- **OS:** [openfpgaOS](https://github.com/ThinkElastic/openfpgaOS) — RISC-V operating system for FPGA platforms

## Acknowledgements

- **Ken Silverman** — BUILD engine
- **3D Realms / Apogee** — Duke Nukem 3D
- **Jonathon Fowler** — JFDuke3D (modern source port this is based on)
- **dyreschlock** — Platform image
- **[openfpgaOS](https://github.com/ThinkElastic/openfpgaOS)** — RISC-V operating system for FPGA platforms
- **[openfpgaOS SDK](https://github.com/ThinkElastic/openfpgaOS-SDK)** — Build system and API

## License

Duke Nukem 3D source code is released under the GPL. The BUILD engine is released under the Ken Silverman license. openfpgaOS components are under the openfpgaOS license. Game data (`duke3d.grp`) is not included and must be obtained separately.
