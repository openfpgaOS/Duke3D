/* Bare RV32 fixture: run with qsim, no SDK/kernel services required.
 * Measures executed instructions, not FPGA cycles or gameplay FPS. */
#include <stdint.h>

int32_t build_divscale12(int32_t n, int32_t d);
static __attribute__((noinline, noclone)) int32_t reference(int32_t n, int32_t d)
{
    return (int32_t)(((int64_t)n * 4096) / d);
}
static inline uint32_t instructions(void)
{
    uint32_t n;
    __asm__ volatile(".insn i 0x73, 2, %0, x0, -1022" : "=r"(n) :: "memory");
    return n;
}
static void print(const char *s, unsigned n)
{
    register unsigned a0 __asm__("a0") = 1;
    register const char *a1 __asm__("a1") = s;
    register unsigned a2 __asm__("a2") = n;
    register unsigned a7 __asm__("a7") = 64;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}
static void number(uint32_t n)
{
    char buf[16];
    unsigned i = sizeof(buf);
    buf[--i] = ' ';
    do { buf[--i] = '0' + n % 10; n /= 10; } while (n);
    print(buf + i, sizeof(buf) - i);
}
static uint32_t seed = 0x78435abfu;
static uint32_t rnd(void)
{ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

int run(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        uint32_t before = 0, after = 0, checksum = 0;
        for (unsigned i = 0; i < 16384; i++) {
            int32_t n = mode ? (int32_t)rnd() : (int32_t)(rnd() & 0x1fffff) - 1048576;
            int32_t d = (int32_t)rnd() | 1;
            uint32_t t0 = instructions();
            int32_t a = reference(n, d);
            uint32_t t1 = instructions();
            int32_t b = build_divscale12(n, d);
            uint32_t t2 = instructions();
            if (a != b) { print("FAIL\n", 5); return 1; }
            before += t1 - t0; after += t2 - t1;
            checksum = checksum * 33u + (uint32_t)b;
        }
        print("MATH mode/calls/before/after/hash: ",
              sizeof("MATH mode/calls/before/after/hash: ") - 1);
        number(mode); number(16384); number(before); number(after); number(checksum);
        print("\n", 1);
    }
    print("BENCH_COMPLETE\n", 15);
    return 0;
}
__asm__(".section .text.start\n.globl _start\n_start:\n"
        ".option push\n.option norelax\nla gp, __global_pointer$\n.option pop\n"
        "call run\nli a7, 93\necall\n");
