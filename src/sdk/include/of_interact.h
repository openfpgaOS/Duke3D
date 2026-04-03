/*
 * of_interact.h -- Analogue Pocket interact menu API
 *
 * The APF interact system writes option values directly to SDRAM
 * at fixed addresses. Apps read these values to get user settings
 * from the Pocket menu (opened by pressing the menu button).
 *
 * Each variable occupies 4 bytes at INTERACT_BASE + (index * 4).
 * Variables are defined in dist/interact.json with addresses
 * pointing to bridge offsets of these SDRAM locations.
 *
 * Bridge address = CPU address - 0x10000000
 * e.g. INTERACT_BASE 0x103FE000 = bridge 0x03FE0000
 */

#ifndef OF_INTERACT_H
#define OF_INTERACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Base addresses */
#define OF_INTERACT_BASE        0x103FE000
#define OF_INTERACT_UNCACHED    0x503FE000
#define OF_INTERACT_BRIDGE_BASE 0x03FE0000
#define OF_INTERACT_MAX_VARS    64

/* Read an interact variable by index (0-63).
 * Returns the 32-bit value set by the Pocket menu. */
static inline uint32_t of_interact_get(int index) {
    if (index < 0 || index >= OF_INTERACT_MAX_VARS) return 0;
    volatile uint32_t *vars = (volatile uint32_t *)OF_INTERACT_UNCACHED;
    return vars[index];
}

/* Compute the bridge address for interact variable N.
 * Use this in interact.json: "address": "0x03FExxxx" */
#define OF_INTERACT_ADDR(n) (OF_INTERACT_BRIDGE_BASE + (n) * 4)

#ifdef __cplusplus
}
#endif

#endif /* OF_INTERACT_H */
