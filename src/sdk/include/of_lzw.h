/*
 * of_lzw.h -- LZW Compression API for openfpgaOS
 *
 * Build engine compatible LZW compress/uncompress.
 */

#ifndef OF_LZW_H
#define OF_LZW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int32_t of_lzw_compress(const uint8_t *in, int32_t in_len,
                                      uint8_t *out) {
    return (int32_t)__of_syscall3(OF_SYS_LZW_COMPRESS, (long)in, in_len, (long)out);
}

static inline int32_t of_lzw_uncompress(const uint8_t *in, int32_t comp_len,
                                        uint8_t *out) {
    return (int32_t)__of_syscall3(OF_SYS_LZW_UNCOMPRESS, (long)in, comp_len, (long)out);
}

#else /* OF_PC */

static inline int32_t of_lzw_compress(const uint8_t *in, int32_t in_len,
                                      uint8_t *out) {
    (void)in; (void)in_len; (void)out;
    return -1;
}

static inline int32_t of_lzw_uncompress(const uint8_t *in, int32_t comp_len,
                                        uint8_t *out) {
    (void)in; (void)comp_len; (void)out;
    return -1;
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_LZW_H */
