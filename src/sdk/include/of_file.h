/*
 * of_file.h -- File I/O for openfpgaOS
 *
 * Use standard C fopen/fread/fwrite/fclose/fseek/ftell.
 *   fopen("slot:3", "rb")     -- opens data slot 3
 *   fopen("save:0", "wb")     -- opens save slot 0 for writing
 *   fclose(f)                  -- auto-flushes saves
 *
 * Use opendir/readdir to discover available files.
 */

#ifndef OF_FILE_H
#define OF_FILE_H

/* No of_file_* functions — use POSIX file I/O.
 * Async reads below are for advanced use (background loading). */

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Start a non-blocking file read from a data slot.
 * dest must be in CRAM1 (direct DMA target).
 * callback(token, result) fires when DMA completes: result=0 success, <0 error.
 * Only one async read in flight at a time (bridge limitation).
 * Returns token >= 0 on success, < 0 if busy or error. */
static inline int of_file_read_async(int slot_id, uint32_t offset,
                                     void *dest, uint32_t length,
                                     void (*callback)(int token, int result)) {
    return (int)__of_syscall5(OF_SYS_FILE_READ_ASYNC,
                              slot_id, (long)offset, (long)dest,
                              (long)length, (long)callback);
}

/* Poll async read progress. Call from your main loop.
 * Returns 1 if a read completed (callback invoked), 0 otherwise. */
static inline int of_file_async_poll(void) {
    return (int)__of_syscall0(OF_SYS_FILE_ASYNC_POLL);
}

/* Check if an async read is in flight. */
static inline int of_file_async_busy(void) {
    return (int)__of_syscall0(OF_SYS_FILE_ASYNC_BUSY);
}

#endif /* OF_PC */

#endif /* OF_FILE_H */
