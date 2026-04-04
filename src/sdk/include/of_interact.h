/*
 * of_interact.h -- Platform menu/settings API
 *
 * Read option values set by the platform menu (Pocket menu button,
 * MiSTer OSD, etc.). Variables are defined in dist/interact.json.
 *
 * Each variable occupies 4 bytes. Use OF_INTERACT_ADDR(n) to compute
 * the bridge address for interact.json.
 */

#ifndef OF_INTERACT_H
#define OF_INTERACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_INTERACT_MAX_VARS    64

/* Bridge address for interact.json authoring.
 * Use in interact.json: "address": "0x03FExxxx" */
#define OF_INTERACT_BRIDGE_BASE 0x03FE0000
#define OF_INTERACT_ADDR(n)     (OF_INTERACT_BRIDGE_BASE + (n) * 4)

/* Read an interact variable by index (0-63).
 * Returns the 32-bit value set by the platform menu. */
#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline uint32_t of_interact_get(int index) {
    return (uint32_t)__of_syscall1(OF_SYS_INTERACT_GET, index);
}

#else /* OF_PC */

static inline uint32_t of_interact_get(int index) {
    (void)index;
    return 0;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* OF_INTERACT_H */
