/*
 * of_bram.h -- BRAM hot-path API for openfpgaOS
 *
 * Place performance-critical functions in BRAM for zero-latency execution.
 * BRAM is on-chip SRAM in the FPGA — no cache misses, no wait states.
 * SDRAM has ~10 cycle penalty on I-cache miss.
 *
 * Usage:
 *   OF_FASTTEXT void R_DrawColumn(void) {
 *       // This runs from BRAM — zero wait state
 *       ...
 *   }
 *
 * Build with app_bram.ld linker script (sdk_bram.mk) instead of app.ld.
 * The OS ELF loader copies .app_fasttext from the ELF to BRAM at load time.
 *
 * Available BRAM: ~23KB (0x2000-0x7E00). OS uses 0x0000-0x1FFF.
 * Top 512 bytes reserved for trap handler stack frame.
 */

#ifndef OF_BRAM_H
#define OF_BRAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Place a function in BRAM. noinline prevents the compiler from inlining
 * the function body into callers (which would copy it back to SDRAM). */
#define OF_FASTTEXT   __attribute__((section(".app_fasttext"), noinline))

/* Place initialized data in BRAM. Use for small lookup tables accessed
 * in hot loops (e.g., color remap tables, sine tables). */
#define OF_FASTDATA   __attribute__((section(".app_fastdata")))

/* Place read-only data in BRAM. */
#define OF_FASTRODATA __attribute__((section(".app_fastrodata")))

/* App BRAM region boundaries (must match hal/regs.h) */
#define OF_APP_BRAM_BASE   0x00002000
#define OF_APP_BRAM_END    0x00007E00
#define OF_APP_BRAM_SIZE   (OF_APP_BRAM_END - OF_APP_BRAM_BASE)  /* ~23KB */

#ifdef __cplusplus
}
#endif

#endif /* OF_BRAM_H */
