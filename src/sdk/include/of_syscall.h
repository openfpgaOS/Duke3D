/*
 * of_syscall.h -- RISC-V ecall mechanism for openfpgaOS
 *
 * Inline assembly wrappers for the ecall instruction.
 * Only available on RISC-V (not defined when OF_PC is set).
 */

#ifndef OF_SYSCALL_H
#define OF_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OF_PC

static inline long __of_syscall0(long n) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long __of_syscall1(long n, long arg0) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long __of_syscall2(long n, long arg0, long arg1) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1) : "memory");
    return a0;
}

static inline long __of_syscall3(long n, long arg0, long arg1, long arg2) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

static inline long __of_syscall4(long n, long arg0, long arg1,
                                     long arg2, long arg3) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3) : "memory");
    return a0;
}

static inline long __of_syscall5(long n, long arg0, long arg1,
                                     long arg2, long arg3, long arg4) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
    return a0;
}

#endif /* !OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SYSCALL_H */
