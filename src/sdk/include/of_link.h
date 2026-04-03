/*
 * of_link.h -- Link cable API for openfpgaOS
 */

#ifndef OF_LINK_H
#define OF_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_link_send(uint32_t data) {
    return (int)__of_syscall1(OF_SYS_LINK_SEND, data);
}

static inline int of_link_recv(uint32_t *data) {
    return (int)__of_syscall1(OF_SYS_LINK_RECV, (long)data);
}

static inline uint32_t of_link_status(void) {
    return (uint32_t)__of_syscall0(OF_SYS_LINK_GET_STATUS);
}

#else /* OF_PC */

int      of_link_send(uint32_t data);
int      of_link_recv(uint32_t *data);
uint32_t of_link_status(void);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_LINK_H */
