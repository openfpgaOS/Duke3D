/*
 * of_save.h -- Save file API for openfpgaOS
 *
 * Persistent storage backed by APF save slots.
 */

#ifndef OF_SAVE_H
#define OF_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_save_read(int slot, void *buf,
                               uint32_t offset, uint32_t len) {
    return (int)__of_syscall4(OF_SYS_SAVE_READ, slot,
                              (long)buf, offset, len);
}

static inline int of_save_write(int slot, const void *buf,
                                uint32_t offset, uint32_t len) {
    return (int)__of_syscall4(OF_SYS_SAVE_WRITE, slot,
                              (long)buf, offset, len);
}

static inline void of_save_flush(int slot) {
    __of_syscall1(OF_SYS_SAVE_FLUSH, slot);
}

static inline int of_save_flush_size(int slot, uint32_t size) {
    return (int)__of_syscall2(OF_SYS_SAVE_FLUSH_SIZE, slot, size);
}

static inline void of_save_erase(int slot) {
    __of_syscall1(OF_SYS_SAVE_ERASE, slot);
}

#else /* OF_PC */

int  of_save_read(int slot, void *buf, uint32_t offset, uint32_t len);
int  of_save_write(int slot, const void *buf, uint32_t offset, uint32_t len);
void of_save_flush(int slot);
void of_save_erase(int slot);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SAVE_H */
