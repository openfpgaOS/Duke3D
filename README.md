# Duke Nukem 3D for openfpgaOS

This repository contains an Analogue Pocket openFPGA port of Duke Nukem 3D built on the [openfgpaSDK](https://github.com/openfpgaOS/openfgpaSDK).

The port runs the released Duke3D/BUILD engine code on a 32-bit RISC-V soft CPU and uses [openfpgaCore](https://github.com/openfpgaOS/openfpgaCore) services for video, input, audio, files, saves, and hardware GPU acceleration. Commercial Duke Nukem 3D game data is not included; you need your own legally obtained `duke3d.grp`.

## Requirements

- Analogue Pocket with openFPGA support.
- The Duke Nukem 3D Atomic Edition `duke3d.grp` data file.
- RISC-V embedded GCC toolchain for building from source.
- Optional: Analogue Dock for physical keyboard and mouse input.

## Build

Use the root Makefile for normal development. From the repository root, build Duke3D with:

```sh
make build CORE=duke3d
```

The release tree is generated at:

```text
build/duke3d/
```

Common targets:

```sh
make build CORE=duke3d        # build Duke3D
make copy CORE=duke3d         # build and copy to a Pocket SD card
make package CORE=duke3d      # create a distributable package
make debug CORE=duke3d        # build, push over UART, and stream console output
make clean CORE=duke3d        # remove Duke3D build artifacts
make clean                    # remove all build artifacts
```

For normal SD-card testing, the usual flow is:

```sh
make build CORE=duke3d
make copy CORE=duke3d
```

## SD Card Data

The Duke instance maps these data files:

| Slot | File | Purpose |
|------|------|---------|
| 1 | `os.bin` | openfpgaOS runtime |
| 2 | `duke3d.elf` | Duke3D application |
| 3 | `duke3d.grp` | Duke Nukem 3D game data |
| 4 | `bank.ofsf` | MIDI/SoundFont sample bank |
| 9 | `duke3d.cfg` | persistent Duke settings |
| 10-19 | `duke3d_0.sav` ... `duke3d_9.sav` | save-game slots |

Saves are exposed as normal files by openfpgaOS. Each game save slot stores Duke's save payload plus the 160x100 save preview thumbnail. Settings are stored in `duke3d.cfg`.

Place your legally obtained game data at:

```text
Assets/duke3d/common/duke3d.grp
```

For a locally generated tree, that is:

```text
build/duke3d/Assets/duke3d/common/duke3d.grp
```

The build copies the included MIDI/SoundFont bank to:

```text
build/duke3d/Assets/duke3d/common/bank.ofsf
```

The packaged bank is `runtime/bank.ofsf`. Users can replace it with another compatible `.ofsf` bank by keeping the filename `bank.ofsf` in the same data slot.

## Technical Capabilities

- Target hardware: VexiiRiscv `rv32imafc` soft CPU at 100 MHz with openfpgaOS services.
- Video: BUILD renders at 320x200 indexed color, letterboxed into the Pocket's 320x240 framebuffer.
- Display output: Pocket scaler mode is configured as 640x240 with 10:9 aspect.
- Frame pacing: triple-buffered framebuffer flow with GPU-triggered flips.
- GPU acceleration:
  - BUILD wall, mask, floor/ceiling, sprite, and translucent span paths are routed through the openfpgaOS GPU where supported.
  - Span grouping and command-stream batching are enabled by default.
  - GPU-side framebuffer clears, mirror blits, texture cache invalidation, palookup slots, and translucency LUT upload are used by the port.
  - Rotated-sprite paths that need exact software framebuffer behavior, including menus and save previews, are kept on the CPU.
- Palette support:
  - 8-bit indexed VGA palette behavior.
  - BUILD palookup shading and palette fades.
  - GPU support for multiple palookup slots and translucency tables.
- Audio:
  - 48 kHz stereo output through the openfpgaOS audio path.
  - Duke SFX are decoded and played through isolated SFX mixer voices.
  - Positional Duke SFX use stereo panning and distance attenuation through the mixer.
  - MIDI music uses the SDK MIDI/sample-bank path with `bank.ofsf`.
  - SFX operations are kept separate from MIDI mixer voices so menu/SFX stops do not globally stop or corrupt music playback.
- Input:
  - Pocket controls, analog sticks, dock keyboard, and dock mouse are supported.
  - Physical keyboard events are translated from USB HID usage IDs to Duke/DOS scancodes.
  - Physical mouse deltas are scaled before entering Duke's mouse path.
- Persistence:
  - Ten save slots.
  - Persistent settings file.
  - Nonvolatile settings and save slots are flushed when files are closed.
  - Save preview thumbnails in the load/save menu.
  - Save thumbnails are rendered through the software path to avoid GPU cache/readback artifacts in the tiny 100x160 capture.

## Pocket Controls

| Control | In Game |
|---------|---------|
| D-pad up/down | Move forward/backward |
| D-pad left/right | Turn left/right; with L1, strafe left/right |
| Left analog stick | Move forward/backward and turn left/right |
| Right analog stick | Mouse look |
| A | Fire; with R1, quick kick |
| B | Hold Run; tap and release within 500 ms for Open/Use; with R1, use inventory item |
| X | Jump; with R1, next weapon |
| Y | Crouch; with R1, previous weapon |
| L1 | D-pad strafe modifier |
| R1 | Face-button and inventory modifier |
| L2 | Strafe left |
| R2 | Strafe right |
| Start | Menu; with R1, next inventory item |
| Select | Automap; with R1, previous inventory item |

When a Duke menu is open, B is treated as Back/Escape instead of Open/Use.

B always holds Duke's Run key while pressed in-game. Open/Use is sent on release only when B was held for less than 500 ms, which makes tap-to-use and hold-to-run share the same button.

## Dock Keyboard and Mouse

The Analogue Dock keyboard path maps common USB HID keys to Duke/DOS scancodes. Letter keys, number keys, punctuation, arrows, Insert/Home/PageUp/Delete/End/PageDown, F1-F12, and left/right Ctrl/Shift/Alt are handled.

Default keyboard bindings come from Duke's standard config:

| Key | Action |
|-----|--------|
| Arrow keys | Move/turn |
| Left Ctrl / Right Ctrl | Fire |
| Space | Open/Use |
| Left Shift / Right Shift | Run |
| A | Jump |
| Z | Crouch |
| Tab | Automap |
| `,` / `.` | Strafe left/right |
| `;` / `'` | Previous/next weapon |
| 1-0 | Weapon select |
| Enter | Inventory |
| `[` / `]` | Inventory left/right |
| R | Steroids |
| C | Quick kick |
| F1-F10 | Duke menu shortcuts |
| F12 | Screenshot path, no-op on openfpgaOS |

Dock mouse support:

| Mouse Input | Action |
|-------------|--------|
| Movement | Duke mouse look / pointer movement |
| Left button | Fire |
| Right button | Open/Use |
| Middle button | Exposed as middle mouse button if mapped by Duke |

## Notes and Limitations

- Commercial game data is not distributed in this repository.
- Sleep is disabled in the core metadata.
- Multiplayer/network code from the original codebase is not the focus of this port.
- Some BUILD paths remain on CPU when exact software framebuffer behavior is required.
- Performance tracing is disabled by default so UART logging does not affect frame timing.

## Acknowledgments

- 3D Realms for releasing the Duke Nukem 3D source code.
- Todd Replogle, Charlie Wiederhold, and the original Duke Nukem 3D contributors.
- Ken Silverman for the BUILD engine.
- Ryan C. Gordon, Andrew Henderson, Dan Olson, Fabien Sanglard, and the wider Chocolate DukeNukem3D/xDuke port lineage referenced by this codebase.
- The [openfgpaSDK](https://github.com/openfpgaOS/openfgpaSDK) and [openfpgaCore](https://github.com/openfpgaOS/openfpgaCore) work used by this port.
- dyreschlock for the Analogue Pocket platform image used by the Duke3D core.

## License and Game Data

The Duke Nukem 3D source code in this repository follows the license headers in the original files. Duke Nukem 3D trademarks, artwork, levels, audio, and `duke3d.grp` game data remain owned by their respective rights holders and are not included here.
