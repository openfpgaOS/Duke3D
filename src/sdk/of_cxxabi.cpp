/*
 * of_cxxabi.cpp -- Minimal C++ ABI support for freestanding openfpgaOS
 *
 * Provides operator new/delete, pure virtual handler, static guard
 * helpers, and __cxa_atexit for global destructor registration.
 * No exceptions, no RTTI — compile with -fno-exceptions -fno-rtti.
 */

extern "C" {

/* Use syscalls directly for malloc/free to avoid header conflicts */
static inline long __cxx_syscall1(long n, long a0) {
    register long a7 __asm__("a7") = n;
    register long _a0 __asm__("a0") = a0;
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(a7) : "memory");
    return _a0;
}

/* OF_SYS_MALLOC = 0x10C0, OF_SYS_FREE = 0x10C1 */
static void *cxx_malloc(unsigned int size) {
    return (void *)__cxx_syscall1(0x10C0, (long)size);
}

static void cxx_free(void *ptr) {
    __cxx_syscall1(0x10C1, (long)ptr);
}

/* ── Pure virtual handler ─────────────────────────────────────── */

void __cxa_pure_virtual(void) {
    __cxx_syscall1(93 /* SYS_exit */, 1);
    __builtin_unreachable();
}

/* ── DSO handle (required by __cxa_atexit) ────────────────────── */

void *__dso_handle = 0;

/* ── Static destructor registration ───────────────────────────── */

#define CXA_ATEXIT_MAX 32

static struct {
    void (*destructor)(void *);
    void *arg;
    void *dso;
} __cxa_atexit_funcs[CXA_ATEXIT_MAX];

static int __cxa_atexit_count = 0;

int __cxa_atexit(void (*destructor)(void *), void *arg, void *dso_handle) {
    if (__cxa_atexit_count >= CXA_ATEXIT_MAX)
        return -1;
    __cxa_atexit_funcs[__cxa_atexit_count].destructor = destructor;
    __cxa_atexit_funcs[__cxa_atexit_count].arg        = arg;
    __cxa_atexit_funcs[__cxa_atexit_count].dso         = dso_handle;
    __cxa_atexit_count++;
    return 0;
}

void __cxa_finalize(void *dso) {
    for (int i = __cxa_atexit_count - 1; i >= 0; i--) {
        if (dso == 0 || __cxa_atexit_funcs[i].dso == dso) {
            if (__cxa_atexit_funcs[i].destructor) {
                __cxa_atexit_funcs[i].destructor(__cxa_atexit_funcs[i].arg);
                __cxa_atexit_funcs[i].destructor = 0;
            }
        }
    }
}

/* ── Thread-safe static init guards (single-threaded stubs) ──── */

int __cxa_guard_acquire(long long *guard) {
    return !*(char *)guard;
}

void __cxa_guard_release(long long *guard) {
    *(char *)guard = 1;
}

void __cxa_guard_abort(long long *guard) {
    (void)guard;
}

} /* extern "C" */

/* ── operator new / delete ────────────────────────────────────── */

typedef __SIZE_TYPE__ size_t;

void *operator new(size_t size)              { return cxx_malloc(size); }
void *operator new[](size_t size)            { return cxx_malloc(size); }
void  operator delete(void *ptr) noexcept    { cxx_free(ptr); }
void  operator delete[](void *ptr) noexcept  { cxx_free(ptr); }
void  operator delete(void *ptr, size_t) noexcept   { cxx_free(ptr); }
void  operator delete[](void *ptr, size_t) noexcept  { cxx_free(ptr); }

/* ── iostream global stream objects ──────────────────────────────────── */

#include <iostream>

namespace std {
    ostream cout(1);   /* stdout (fd 1) */
    ostream cerr(2);   /* stderr (fd 2) */
    istream cin;       /* stdin  (fd 0) */
}
