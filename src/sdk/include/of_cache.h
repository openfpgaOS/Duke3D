/*
 * of_cache.h -- Cache management API for openfpgaOS
 *
 * Exposes D-cache flush and I-cache invalidate for advanced users.
 * Most apps do not need to call these directly.
 */

#ifndef OF_CACHE_H
#define OF_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Flush D-cache for the draw buffer (same as of_video_flush). */
static inline void of_cache_flush_video(void) {
    __of_syscall0(OF_SYS_VIDEO_FLUSH_CACHE);
}

/* Invalidate I-cache (fence.i). */
static inline void of_cache_invalidate_icache(void) {
    __asm__ volatile("fence");
    __asm__ volatile(".word 0x0000100f"); /* fence.i */
}

#else /* OF_PC */

static inline void of_cache_flush_video(void) { /* no-op on PC */ }
static inline void of_cache_invalidate_icache(void) { /* no-op on PC */ }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_CACHE_H */
