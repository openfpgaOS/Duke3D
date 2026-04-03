/*
 * of_dma.h -- DMA engine API for openfpgaOS
 *
 * Hardware DMA for large memory-to-memory transfers, bypassing CPU.
 * The DMA engine reads/writes SDRAM directly — cache coherency is
 * handled automatically by the kernel (evicts overlapping D-cache lines).
 *
 * Use for bulk copies/fills where the CPU would otherwise stall on
 * cache misses. For small transfers (<1KB), memcpy/memset are faster
 * due to syscall + cache eviction overhead.
 *
 * Blocking API (of_dma_copy/fill/zero):
 *   Returns after DMA completes. Simple, safe.
 *
 * Non-blocking API (of_dma_copy_async/fill_async):
 *   Starts DMA and returns immediately. CPU can do other work while
 *   DMA runs. MUST call of_dma_wait() before reading dst or starting
 *   another DMA. Do NOT touch the dst region until of_dma_wait() returns.
 *
 * Constraints:
 *   - len must be word-aligned (multiple of 4)
 *   - src/dst must be in SDRAM (0x10000000+) or CRAM (0x30000000+)
 */

#ifndef OF_DMA_H
#define OF_DMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* ---- Blocking API ---- */

/* Copy len bytes from src to dst via DMA engine.
 * Cache-coherent: kernel flushes src and evicts dst lines.
 * len must be a multiple of 4. */
static inline void of_dma_copy(void *dst, const void *src, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_COPY, (long)dst, (long)src, (long)len);
}

/* Fill len bytes at dst with a 32-bit value via DMA engine.
 * Cache-coherent: kernel evicts dst lines.
 * len must be a multiple of 4. */
static inline void of_dma_fill(void *dst, uint32_t value, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_FILL, (long)dst, (long)value, (long)len);
}

/* Clear len bytes at dst to zero via DMA engine. */
static inline void of_dma_zero(void *dst, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_FILL, (long)dst, 0, (long)len);
}

/* ---- Non-blocking API ---- */

/* Start async DMA copy. Returns immediately.
 * MUST call of_dma_wait() before accessing dst or starting another DMA. */
static inline void of_dma_copy_async(void *dst, const void *src, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_COPY_ASYNC, (long)dst, (long)src, (long)len);
}

/* Start async DMA fill. Returns immediately.
 * MUST call of_dma_wait() before accessing dst or starting another DMA. */
static inline void of_dma_fill_async(void *dst, uint32_t value, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_FILL_ASYNC, (long)dst, (long)value, (long)len);
}

/* Start async DMA zero. Returns immediately. */
static inline void of_dma_zero_async(void *dst, uint32_t len) {
    __of_syscall3(OF_SYS_DMA_FILL_ASYNC, (long)dst, 0, (long)len);
}

/* Wait for in-flight DMA to complete. */
static inline void of_dma_wait(void) {
    __of_syscall0(OF_SYS_DMA_WAIT);
}

/* Check if DMA engine is busy. Returns 1 if busy, 0 if idle. */
static inline int of_dma_busy(void) {
    return (int)__of_syscall0(OF_SYS_DMA_BUSY);
}

#else /* OF_PC */

#include <string.h>

static inline void of_dma_copy(void *dst, const void *src, uint32_t len) {
    memcpy(dst, src, len);
}
static inline void of_dma_fill(void *dst, uint32_t value, uint32_t len) {
    uint32_t *p = (uint32_t *)dst;
    for (uint32_t i = 0; i < len / 4; i++) p[i] = value;
}
static inline void of_dma_zero(void *dst, uint32_t len) {
    memset(dst, 0, len);
}
static inline void of_dma_copy_async(void *dst, const void *src, uint32_t len) {
    memcpy(dst, src, len);
}
static inline void of_dma_fill_async(void *dst, uint32_t value, uint32_t len) {
    uint32_t *p = (uint32_t *)dst;
    for (uint32_t i = 0; i < len / 4; i++) p[i] = value;
}
static inline void of_dma_zero_async(void *dst, uint32_t len) {
    memset(dst, 0, len);
}
static inline void of_dma_wait(void) { }
static inline int of_dma_busy(void) { return 0; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_DMA_H */
