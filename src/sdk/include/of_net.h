/*
 * of_net.h -- Host/client networking API for openfpgaOS
 *
 * Byte-stream communication between devices.
 * Pocket: link cable (1 host, 1 client).
 * MiSTer: user port. PC: sockets.
 *
 * Usage (client):
 *   of_net_join();
 *   of_net_send(data, len);
 *   of_net_recv(buf, sizeof(buf));
 *
 * Usage (host):
 *   of_net_host_start();
 *   of_net_broadcast(data, len);
 *   of_net_recv_from(0, buf, sizeof(buf));
 */

#ifndef OF_NET_H
#define OF_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_NET_DISCONNECTED  0
#define OF_NET_HOSTING       1
#define OF_NET_JOINED        2

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_net_host_start(void) {
    return (int)__of_syscall0(OF_SYS_NET_HOST_START);
}
static inline int of_net_join(void) {
    return (int)__of_syscall0(OF_SYS_NET_JOIN);
}
static inline void of_net_stop(void) {
    __of_syscall0(OF_SYS_NET_STOP);
}
static inline int of_net_status(void) {
    return (int)__of_syscall0(OF_SYS_NET_STATUS);
}
static inline int of_net_client_count(void) {
    return (int)__of_syscall0(OF_SYS_NET_CLIENT_COUNT);
}
static inline int of_net_send_to(int client, const void *data, size_t len) {
    return (int)__of_syscall3(OF_SYS_NET_SEND_TO, client, (long)data, len);
}
static inline int of_net_recv_from(int client, void *data, size_t len) {
    return (int)__of_syscall3(OF_SYS_NET_RECV_FROM, client, (long)data, len);
}
static inline int of_net_broadcast(const void *data, size_t len) {
    return (int)__of_syscall2(OF_SYS_NET_BROADCAST, (long)data, len);
}
static inline int of_net_send(const void *data, size_t len) {
    return (int)__of_syscall2(OF_SYS_NET_SEND, (long)data, len);
}
static inline int of_net_recv(void *data, size_t len) {
    return (int)__of_syscall2(OF_SYS_NET_RECV, (long)data, len);
}
static inline int of_net_poll(void) {
    return (int)__of_syscall0(OF_SYS_NET_POLL);
}

#else /* OF_PC */

static inline int of_net_host_start(void) { return -1; }
static inline int of_net_join(void) { return -1; }
static inline void of_net_stop(void) {}
static inline int of_net_status(void) { return OF_NET_DISCONNECTED; }
static inline int of_net_client_count(void) { return 0; }
static inline int of_net_send_to(int c, const void *d, size_t l) {
    (void)c; (void)d; (void)l; return -1;
}
static inline int of_net_recv_from(int c, void *d, size_t l) {
    (void)c; (void)d; (void)l; return -1;
}
static inline int of_net_broadcast(const void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_send(const void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_recv(void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_poll(void) { return 0; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_NET_H */
