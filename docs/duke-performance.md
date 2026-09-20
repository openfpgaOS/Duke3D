# Doom and Quake optimizations applied to Duke3D

The September 19, 2026 review used the neighboring Doom and Quake working
trees, including their uncommitted optimizations. The Duke baseline is
`70fcda8` with the existing local SDK/runtime updates retained.

## Changes

- **GPU lanes in wire order.** Adapted Quake's September round-two affine
  builder to both Duke affine spans and column lists. Each append writes
  adjacent words with already-packed count/light/palette metadata; submission
  copies words without gathering parallel arrays. Batch payloads no longer
  need clearing. This follows Quake's `vid_of.c` and the validated emitter
  approach in Doom's `r_gpu.c`.
- **Preserved command behavior.** Native commands still contain at most four
  lanes, batches still contain at most eight, and all reserve/kick boundaries
  are retained. Per-chunk live bits preserve the SDK's all-zero suppression.
  Counts still clamp at 4095, and palette/light bits use the same masks.
  Affine/column transitions, translucent barriers, and CPU/GPU synchronization
  retain their existing ordering. Compile-time assertions guard the wire
  format assumptions.
- **Capability checks once at initialization.** The private emitters rely on
  initialization checking compact affine support and probing optional column
  support. Previously, a core advertising parametric spans but lacking compact
  affine spans could enable Duke's GPU path while the SDK silently rejected
  its draws. That core now uses the software renderer.
- **Exact wall division on RV32.** Adapted Doom's integer quotient/remainder
  strategy to BUILD's 20.12 division. Small numerators use native 32-bit
  division; larger ones generate twelve fractional bits. Signed results,
  `INT_MIN`, truncation toward zero, and low-word overflow behavior match the
  original 64-bit calculation. Other fixed-point shifts retain their existing
  implementation. The helper stays outside APP_BRAM.
- **Incremental builds.** Engine/game headers invalidate application objects,
  and the application Makefile invalidates the ELF, so inline math and build
  changes reach incremental builds.

The GPU header, MIDI engine, sample-voice engine, sample bank, and SDK build
rules were already byte-identical to both reference repositories. That covers
their shared command transport, header/span packing, fence initialization,
and audio improvements; those pre-existing local files were left intact.

Doom's visplane, perspective wall/plane setup, and renderer object ordering
depend on its renderer. Quake's BSP/edge walker, QuakeC interpreter, entity
interpolation, lightmap, and floating-point trigonometry changes have no direct
equivalent in BUILD's integer sector renderer and game code. They were not
copied. No new compiler tuning, FPGA build, or release is required.

## Validation

`python3 tools/check_duke_optimizations.py` compiles extracted production
functions with host AddressSanitizer and UndefinedBehaviorSanitizer. The
hardware SDK emitters are explicitly selected; desktop no-op stubs cannot
satisfy the command comparison. Leak checking is disabled for ptrace-based
sandboxes; the fixture uses static/stack storage.

- 12,000 sequences / 3,072,000 input lanes: **20,983,346 identical command
  words** and **1,577,358 identical reserve/kick/staging events** against the
  public SDK emitters. Cases cover both command types, affine fallback, partial
  and full batches, changing headers, zero/oversized counts, negative strides,
  poisoned unused storage, simulated reservation failures, and recovery.
- **6,097,904 exact division comparisons**, including sign combinations,
  `INT_MIN`, overflow, threshold boundaries, small exhaustive inputs and
  randomized 32-bit inputs.
- **160 initialization cases** cover missing capabilities/GPU, the renderer
  disable switch, and successful/failed column probes.
- Pocket and MiSTer application ELFs build without warnings using the installed
  `riscv64-elf-gcc 15.2.0`. Docker's socket was inaccessible, so these checks
  used `USE_SDK_CONTAINER=0` and isolated object directories.
- APP_BRAM usage falls from **13,712 to 13,380 bytes** of 14,336. SDRAM text
  grows by 1,536 bytes; BSS is unchanged.

The existing desktop build fails on missing GPU constants/MMIO definitions
in its `OF_PC` path; compiling the saved baseline GPU source confirms the
same class of errors. Full-game qsim validation stops in the unchanged
baseline at unsupported `OF_FILE_FID_SLOT_FIND` (vendor EID `0xc0de000e`,
FID 5), before any frames. Hardware gameplay and frame-rate improvements
remain unmeasured.

## Isolated RISC-V measurements

The local SDK's qsim executes the queue fixture and arithmetic fixture as
RV32 ELFs. Queue measurements include the same mock ring/services and driver
overhead in both versions. They are executed instruction counts, excluding
GPU/DMA execution, cache timing, interrupts, and full gameplay.

| Fixture | Before instructions | After instructions | Reduction |
|---|---:|---:|---:|
| Mixed GPU queue/flush sequences | 724,857,123 | 538,485,782 | 25.71% |
| 16,384 small-numerator divisions | 1,278,937 | 262,370 | 79.49% |
| 16,384 full-range divisions | 1,282,249 | 894,920 | 30.21% |

These synthetic distributions do not estimate a game's mix of work. The
percentages cannot be added or interpreted as FPS gains.

Reproduce from the repository root (qsim is an optional external tool):

```sh
mkdir -p build/optimization-check
git show 70fcda8:src/duke3d/d3d_gpu.c > build/optimization-check/before-gpu.c
python3 tools/check_duke_optimizations.py
python3 tools/benchmark_duke_optimizations.py \
  --qsim ../openfpgaSDK/tools/qsim/qsim \
  --reference-gpu build/optimization-check/before-gpu.c

make -C src/duke3d USE_SDK_CONTAINER=0 TARGET=pocket \
  OBJ_DIR="$PWD/.obj/optimization-after" "$PWD/.obj/optimization-after/app.elf" -j6
make -C src/duke3d USE_SDK_CONTAINER=0 TARGET=mister \
  OBJ_DIR="$PWD/.obj/optimization-mister" "$PWD/.obj/optimization-mister/app.elf" -j6
```

The benchmark writes logs and `results.json` under
`build/optimization-check/bench`. The host checks require a C compiler with
ASan/UBSan; the optional benchmark also requires a RISC-V compiler and qsim.
No game assets are included or required by these fixtures.
