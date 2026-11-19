# Duke3D 1.0.6 for Analogue Pocket

2026-11-18. Pocket only; the MiSTer package stays at 1.0.4.

## Changes

- Louder output. The bundled openfpgaOS 0.9.1 core divides the final audio
  mix by 4 instead of 16 (+12 dB). Sound effects gain the full 12 dB. The
  top of the music slider maps to half the previous MIDI master level so
  dense passages keep headroom, which leaves music about 6 dB louder than
  before. The music adjustment applies only when the kernel reports 0.9.1
  or later.
- GPU commands go directly into the GPU's command ring instead of being
  written to SDRAM and read back. The bundled os25 core no longer has the
  old readback path, so the executable and the core must be installed
  together.
- Faster CPU in the bundled os25 core: shift instructions complete in one
  pipeline stage instead of two, which saves 7.5% of cycles in the core's
  CPU test programs. The core also gathers masked framebuffer writes into
  SDRAM bursts and reads each video line in shorter bursts.
- Time queries call the kernel's service table instead of trapping.

## Verification

The packaged core, kernel and executable were booted and played on a
Pocket. Frame rate was not measured, the new levels were not measured for
clipping, and no long-session soak was made. Duke3D has no desktop build,
so there are no host checks beyond the packaging check.

## Install

Extract the ZIP at the SD-card root and merge its folders. Put `duke3d.grp`
in `Assets/duke3d/common/`. Install the whole ZIP: `duke3d.elf`,
`os25.rbf_r` and `os.bin` belong together and should not be mixed with
files from 1.0.5.

The bundled os25 core stays at the requested 100 MHz and does not close
main-clock setup timing: seed 28 has setup slack -0.601 ns and worst hold
+0.092 ns. This remains an overclocked build.
