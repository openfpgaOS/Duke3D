//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_fastram.h -- Fast memory section attributes
 *
 * Place performance-critical code in fast RAM (BRAM on Pocket, SRAM on
 * MiSTer, no-op on PC).
 *
 * What the region actually buys -- it is NOT "zero-latency":
 *
 *   OF_FASTTEXT   Code still executes out of the I-cache.  The region is
 *                 marked I/O in the CPU's memory map, which only changes
 *                 where a line REFILL comes from: BRAM instead of SDRAM,
 *                 so a miss is cheaper and never queues behind GPU,
 *                 scanout or audio traffic on the SDRAM bus.  Cache hits
 *                 cost the same as anywhere else.  Worth it for inner
 *                 loops that get evicted and re-fetched often.
 *
 *   OF_FASTDATA / OF_FASTRODATA
 *                 SLOWER than ordinary memory for anything read in a hot
 *                 loop.  I/O-mapped data bypasses the D-cache entirely:
 *                 every load and store is a non-speculative bus
 *                 transaction (several cycles, and it stalls the
 *                 pipeline) where a D-cache hit costs one.  Use it only
 *                 for state that must not depend on the SDRAM bus --
 *                 interrupt-context state such as the MIDI voice tables --
 *                 never for lookup tables or per-span scalars.
 *
 * Usage:
 *   OF_FASTTEXT static int hot_function(int x) { ... }
 *   OF_FASTDATA static int lookup_table[256] = { ... };
 *   OF_FASTRODATA static const int constants[16] = { ... };
 *
 * Build with the BRAM-enabled linker script (app.ld already supports this).
 * Apps that don't use these attributes are unaffected.
 */

#ifndef OF_FASTRAM_H
#define OF_FASTRAM_H

/* Place function in fast RAM. noinline prevents the compiler from
 * inlining the function body back into slow memory callers.
 *
 * align-functions/align-loops are forced DOWN to 4 here: the SDK builds
 * with -falign-functions=64 so SDRAM-resident code starts on I$-line
 * boundaries; inside the small APP_BRAM region that padding costs more
 * than it returns.  With every hot function padded to 64 bytes the 14 KB
 * region loses hundreds of bytes to alignment alone (Duke3D overflowed it
 * on padding, not code).  Fast-RAM code is still served through the I$
 * (see the file header), so this trades a little line-alignment for
 * room; it is not free in principle, just the right call at this size. */
#define OF_FASTTEXT   __attribute__((section(".app_fasttext"), noinline, \
                                     optimize("align-functions=4", "align-loops=4")))

/* Place initialized data in fast RAM.  Bypasses the D-cache: slower than
 * normal memory in hot loops -- see the file header before using. */
#define OF_FASTDATA   __attribute__((section(".app_fastdata")))

/* Place read-only data in fast RAM.  Same caveat as OF_FASTDATA. */
#define OF_FASTRODATA __attribute__((section(".app_fastrodata")))

#endif /* OF_FASTRAM_H */
