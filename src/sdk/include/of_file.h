/*
 * of_file.h -- File I/O API for openfpgaOS (APF Data Slots)
 *
 * Preferred API: use standard C fopen/fread/fwrite/fclose.
 *   fopen("duke3d.grp", "rb")  -- opens data file via FTAB lookup
 *   fopen("save_0", "wb")      -- opens save slot 0 for writing
 *   fclose(f)                   -- auto-flushes saves with actual size
 *
 * The Chip32 loader populates the filename→slot table (FTAB) at boot,
 * so manual of_file_slot_register() calls are no longer needed.
 */

#ifndef OF_FILE_H
#define OF_FILE_H

#include <stdint.h>

#define OF_PATH_SLOT_PREFIX  "slot:"
#define OF_PATH_SAVE_PREFIX  "save:"
#define OF_PATH_MAX          256

#define OF_FILE_SLOT_NAME_MAX  32

typedef struct {
    uint32_t slot_id;
    char     filename[OF_FILE_SLOT_NAME_MAX];
} of_file_slot_t;

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_file_read(uint32_t slot_id, uint32_t offset,
                               void *dest, uint32_t length) {
    return (int)__of_syscall4(OF_SYS_FILE_READ,
                              slot_id, offset, (long)dest, length);
}

static inline long of_file_size(uint32_t slot_id) {
    return __of_syscall1(OF_SYS_FILE_SIZE, slot_id);
}

static inline int of_file_slot_count(void) {
    return (int)__of_syscall0(OF_SYS_FILE_SLOT_COUNT);
}

static inline int of_file_slot_get(int index, of_file_slot_t *slot) {
    return (int)__of_syscall2(OF_SYS_FILE_SLOT_GET, index, (long)slot);
}

/* Deprecated: FTAB auto-populates from Chip32 loader. Use fopen("filename") instead. */
static inline void of_file_slot_register(uint32_t slot_id, const char *filename) {
    __of_syscall2(OF_SYS_FILE_SLOT_REGISTER, slot_id, (long)filename);
}

/* Register an idle hook — called by the OS during DMA/bridge waits.
 * Use for background work (audio pump, input) during file I/O.
 * Set to NULL to disable. */
static inline void of_set_idle_hook(void (*hook)(void)) {
    __of_syscall1(OF_SYS_SET_IDLE_HOOK, (long)hook);
}

#else /* OF_PC */

int  of_file_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length);
long of_file_size(uint32_t slot_id);
static inline int of_file_slot_count(void) { return 0; }
static inline int of_file_slot_get(int index, of_file_slot_t *slot) {
    (void)index; (void)slot; return -1;
}
static inline void of_file_slot_register(uint32_t slot_id, const char *filename) {
    (void)slot_id; (void)filename;
}
static inline void of_set_idle_hook(void (*hook)(void)) {
    (void)hook;
}

#endif /* OF_PC */

#endif /* OF_FILE_H */
