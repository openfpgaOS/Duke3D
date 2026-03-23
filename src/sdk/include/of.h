/*
 * of.h -- openfpgaOS Application API
 *
 * One header. Everything you need to make a game on Analogue Pocket.
 * On PC, compile with -DOF_PC and link SDL2. No of_sdl2.c needed.
 *
 *   #include "of.h"
 *
 *   int main(void) {
 *       of_video_init();
 *       while (1) {
 *           of_input_poll();
 *           if (of_btn(OF_BTN_A)) shoot();
 *           of_video_clear(0);
 *           draw_world();
 *           of_video_flip();
 *       }
 *   }
 *
 * Platform: Analogue Pocket FPGA, RISC-V (VexRiscv) @ 100 MHz
 * Video:    320x240, 8-bit indexed color, double-buffered
 * Audio:    YMF262 OPL3 FM synthesis (18 channels) + 48 kHz PCM FIFO
 * Input:    2 controllers, d-pad + ABXY + L/R + sticks + triggers
 * Memory:   64 MB SDRAM, 16 MB CRAM0, 16 MB CRAM1, 256 KB SRAM
 */

#ifndef OF_H
#define OF_H

#include <stdint.h>

/* Foundation headers */
#include "of_error.h"
#include "of_version.h"
#include "of_syscall_numbers.h"

#ifndef OF_PC
#include "of_syscall.h"
#endif

/* Per-module headers */
#include "of_video.h"
#include "of_audio.h"
#include "of_input.h"
#include "of_timer.h"
#include "of_file.h"
#include "of_save.h"
#include "of_link.h"
#include "of_analogizer.h"
#include "of_terminal.h"
#include "of_tile.h"
#include "of_cache.h"
#include "of_interact.h"
#include "of_mixer.h"
#include "of_codec.h"
#include "of_lzw.h"
#include "of_bram.h"
#include "of_midi.h"

/* ======================================================================
 * System
 * ====================================================================== */

#ifndef OF_PC

static inline void of_exit(void) {
    __of_syscall0(93); /* SYS_exit */
    __builtin_unreachable();
}

#else /* OF_PC */

void of_exit(void);

#endif /* OF_PC */

/* ======================================================================
 * Initialization (OF_PC only -- on FPGA, the kernel handles init)
 * ====================================================================== */

#ifdef OF_PC
static inline void of_init(void) {
    of_video_init();
}
#endif

#endif /* OF_H */
