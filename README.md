# openfpgaOS SDK

Build games for the [Analogue Pocket](https://www.analogue.co/pocket) in C.

## Getting Started

### 1. Fork and clone

```bash
git clone https://github.com/YOUR_USERNAME/openfpgaOS-SDK.git
cd openfpgaOS-SDK
```

### 2. Check your toolchain

```bash
./setup.sh
```

You need a RISC-V GCC:
- **Arch:** `pacman -S riscv64-elf-gcc`
- **macOS:** `brew install riscv64-elf-gcc`
- **Ubuntu:** `apt install gcc-riscv64-unknown-elf`

### 3. Create your game

```bash
./customize.sh
```

Follow the prompts. This creates:
- `src/<gamename>/main.c` — a hello world stub to start from
- `dist/<gamename>/` — the core packaging config

### 4. Build and deploy

```bash
make               # builds all apps → build/sdk/
make deploy         # copies to Pocket SD card
```

## Writing Your Game

Edit `src/<gamename>/main.c`:

```c
#include "of.h"
#include <stdio.h>

int main(void) {
    of_video_init();

    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            fb[y * 320 + x] = x ^ y;

    of_video_flip();
    printf("Hello world!\n");

    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) {
            // handle input
        }
        of_delay_ms(16);
    }
}
```

### Standard C library

The SDK provides standard headers through the OS kernel's jump table:

```c
#include <stdio.h>    // printf, fprintf, sprintf, sscanf,
                      // fopen, fclose, fread, fwrite, fseek, ftell
#include <stdlib.h>   // malloc, free, calloc, realloc, atoi, atof,
                      // strtol, strtod, qsort, bsearch, rand
#include <string.h>   // memcpy, memset, strlen, strcmp, strdup,
                      // strcat, strtok, memchr, ...
#include <math.h>     // sinf, cosf, sqrtf, powf, logf, fabsf, ...
#include <ctype.h>    // toupper, tolower, isalpha, isdigit, isspace, ...
```

No musl or newlib needed — everything runs through the OS.

### API modules

Include `"of.h"` for the full API:

| Module | Description |
|--------|-------------|
| **Video** | 320×240 indexed framebuffer, 256-color palette, double buffering |
| **Audio** | 48 kHz stereo PCM, sample streaming, YM2151 FM synthesis |
| **Input** | D-pad, face buttons, shoulders, triggers, joystick (2 players) |
| **Timer** | Microsecond/millisecond timing, delays |
| **Save** | Up to 10 persistent save slots, up to 256KB each |
| **File** | Read data files from SD card (up to 4 data slots) |

### Save system

```c
of_save_write(0, data, 0, sizeof(data));
of_save_flush_size(0, sizeof(data));

of_save_read(0, data, 0, sizeof(data));
```

### PC build

Test on your computer with SDL2:

```bash
make pc
./app_pc
```

## Scripts

| Script | What it does |
|--------|-------------|
| `setup.sh` | Checks RISC-V toolchain is installed |
| `customize.sh` | Creates a new game: stub source + core config |
| `deploy.sh` | Copies `build/sdk/` to the Pocket SD card |
| `package.sh` | ZIPs a game core for distribution |

### Packaging a standalone game

After `customize.sh`, your game can be packaged as its own Pocket menu entry:

```bash
./package.sh GameName      # creates releases/GameName.zip
```

Users extract the ZIP to their SD card root.

## Updating

When a new SDK version is released:

```bash
git remote add upstream https://github.com/ThinkElastic/openfpgaOS-SDK.git
git fetch upstream
git rebase upstream/main
make clean && make
```

Your game source (`src/<gamename>/`) won't conflict — SDK files are clearly separated.

## Project Structure

```
openfpgaOS-SDK/
├── Makefile              ← build all apps, create build/sdk/
├── src/
│   ├── <gamename>/       ← YOUR game source (created by customize.sh)
│   ├── apps/             ← bundled example apps
│   └── sdk/              ← headers, libc, CRT, build rules
│       ├── include/      ← openfpgaOS API (of.h, of_video.h, ...)
│       ├── libc/         ← C standard library wrappers
│       ├── crt/          ← startup code + linker script
│       └── pc/           ← SDL2 shim for desktop builds
├── dist/
│   ├── sdk/              ← shared core configs
│   └── <gamename>/       ← standalone game core (from customize.sh)
├── runtime/              ← FPGA bitstream, OS binary, loader
├── build/                ← build output (gitignored)
├── customize.sh
├── deploy.sh
├── package.sh
└── setup.sh
```

## Reference

This SDK builds apps for [openfpgaOS](https://github.com/ThinkElastic/openfpgaOS) — a RISC-V operating system (VexRiscv rv32imafc, 100 MHz) running on the Analogue Pocket's Cyclone V FPGA. See the openfpgaOS repo for architecture details, FPGA design, and OS internals.
