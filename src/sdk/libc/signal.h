/* signal.h -- openfpgaOS stub (no signal support) */
#ifndef _OF_SIGNAL_H
#define _OF_SIGNAL_H

#ifdef OF_PC
#include_next <signal.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sig_handler_t)(int);

#define SIG_DFL ((sig_handler_t)0)
#define SIG_IGN ((sig_handler_t)1)
#define SIG_ERR ((sig_handler_t)-1)

#define SIGINT   2
#define SIGTERM  15
#define SIGSEGV  11

static inline sig_handler_t signal(int sig, sig_handler_t handler) {
    (void)sig; (void)handler;
    return SIG_DFL;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_SIGNAL_H */
