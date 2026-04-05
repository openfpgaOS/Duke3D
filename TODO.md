# DukeNukem3D — openfpgaOS Integration Review

## Platform Bridge Files

| File | Lines | Role |
|---|---|---|
| `display_of.c` | 738 | Video, input, timer — replaces SDL2 |
| `of_compat.h` | 113 | POSIX compat: byte order, min/max, type stubs |
| `d3d_audio.c/h` | 85+14 | Sound effects — bridges to of_mixer/of_codec |
| `d3d_save.c/h` | 267+36 | Save system — LZW compress via fopen/fwrite |
| `midi_of.c` | 247 | MIDI music — bridges MUSIC_* to of_midi |
| `posix_shim.c` | 65 | Stubs: multiplayer, DSL, SDL, POSIX utils |
| `of_posix.c` | 197 | POSIX/libc runtime (SDK-provided) |

Total platform-specific code: ~1,800 lines across 8 files, plus ~200 lines of
`#ifdef OPENFPGA` blocks in original game code.

---

## Stubs & No-ops

### Multiplayer (posix_shim.c) — 9 functions
- `sendpacket`, `sendlogoff`, `getpacket`, `initmultiplayers`,
  `uninitmultiplayers`, `setpackettimeout`, `getoutputcirclesize`,
  `genericmultifunction`, `sendlogon`, `flushpackets`

### DSL Audio (posix_shim.c) — 7 functions
- `DSL_ErrorString` (returns "OS mixer"), `DSL_Init`, `DSL_Shutdown`,
  `DSL_BeginBufferedPlayback`, `DSL_StopPlayback`,
  `DSL_GetPlaybackRate` (returns 48000), `DSL_PumpAudio`

### SDL (posix_shim.c) — 6 functions
- `SDL_CreateMutex` (returns NULL), `SDL_DestroyMutex`, `SDL_mutexP`,
  `SDL_mutexV`, `SDL_GetRelativeMouseMode` (returns 1),
  `SDL_SetRelativeMouseMode`

### Display (display_of.c)
- `_joystick_init/deinit/update/axis/hat/button`
- `_uninitengine`, `screencapture`, `fullscreen_toggle_and_change_driver`

### Other (posix_shim.c)
- `STUBBED()`, `getch()` (returns 'y'), `Z_AvailHeap()` (returns 32MB),
  `lastPalette[768]`

---

## Workarounds

| Workaround | File:Line | Limitation | Solution |
|---|---|---|---|
| Skip CRC32 | filesystem.c:165 | 11MB GRP read too slow for DMA | Skip validation entirely |
| malloc vs allocache | filesystem.c:621+, sounds.c:240 | No cache eviction system | Use malloc directly |
| Hardcode GRP name | game.c:8142 | No directory scanning | Always use "duke3d.grp" |
| Skip version detect | game.c:8210 | Multiple fopen() calls | Hardcode to SHAREWARE_GRP13 |
| Cap tile cache | tiles.c:283 | 64MB SDRAM total | Limit to 16MB |
| No config persistence | config.c:104, 814 | No writable filesystem | Hardcode settings at startup |
| Simplified 3D audio | sounds.c:255 | No FX_Manager/spatializer | Distance falloff only, no panning |
| No sound stopping | sounds.c:490 | Mixer auto-expires voices | stopsound() is no-op |
| Stack-to-static | menues.c:283, 672 | Limited stack on openfpgaOS | static int32 ptrbuf[MAXTILES] (36KB) |
| getch() returns 'y' | posix_shim.c:25 | No stdin | Auto-accept prompts |

---

## Extern Declarations Without Headers

| Declaration | File:Line | Notes |
|---|---|---|
| `extern void of_progress(int)` | tiles.c:266 | Engine→game cross-layer callback, no header exists |

---

## BRAM Hot-Path Placement

### Inner Loops (OF_FASTTEXT, draw.c)
- `hlineasm4` — floor/ceiling spans
- `vlineasm4` — 4-column wall rendering
- `mvlineasm4` — 4-column masked walls
- `tvlineasm2` — dual transparent columns
- `DrawSpriteVerticalLine` — sprite pixel loop
- `slopevlin` — sloped surfaces
- `vlineasm1` — single wall column
- `mvlineasm1` — single masked column
- `tvlineasm1` — single transparent column
- `prevlineasm1` — edge column setup
- `mhlineskipmodify` — masked floor/ceiling
- `thlineskipmodify` — transparent floor/ceiling

### Dispatchers (OF_FASTTEXT, engine.c)
- `wallscan`, `maskwallscan`, `ceilscan`, `florscan`, `grouscan`,
  `transmaskwallscan`

### Data (OF_FASTDATA)
- `reciptable[2048]` (8KB) — perspective reciprocals (engine.c)
- All draw.c statics: `bytesperline`, `mach3_al`, `machmv`, `machxbits_al`,
  `bitsSetup`, `textureSetup`, `tran2shr`, `tran2pal_ebx/ecx`,
  `tspal`, `tsmach_*`, `adder`, `mmach_*`, `tmach_*`, `mshift_*`,
  `tshift_*`, `slopemach_*`, `asm2_f`, `transrev`

### Usage
- .app_fasttext: 17,186 bytes (16.8KB) — code
- .app_fastdata: 112 bytes — data
- Total: 17,298 bytes (33% of 51KB BRAM)

---

## TODO

- [ ] Load FAKE.TMB timbre bank from GRP at startup for authentic Duke3D OPL3 sound
- [ ] Add music volume to interact.json menu so users can adjust in-game
- [ ] Investigate MUSIC_FadeVolume — currently snaps to target, should fade over time
- [ ] Consider moving of_progress() declaration to a shared header
- [ ] Port celeste and fbdemo from SDL2 to of_video API so they build for FPGA
- [ ] Test MIDI looping behavior on level transitions (stop→play should be clean)
- [ ] Profile BRAM impact — measure frame time with/without OF_FASTTEXT
- [ ] Evaluate remaining 26KB BRAM for additional hot data (ylookup[], palookup pointers)
