/*
 * test_opl_xtu.c — OPL3 register writes from a separate compilation unit
 *
 * This file is compiled separately from test_audio.c. It exists to
 * prove that cross-translation-unit OPL3 writes via the services
 * table produce identical hardware behavior to in-unit writes.
 *
 * If OP.21 (this test's results) differs from OP.20 (in-unit version),
 * the bug is in cross-TU symbol resolution, link-time optimization, or
 * some compiler effect that breaks indirect function calls.
 */

#include <stdint.h>
#include "of.h"

/* Hard-noinline helper for the cross-TU register write path. */
__attribute__((noinline,noclone))
void xtu_opl_write(uint16_t reg, uint8_t val) {
    /* volatile asm barrier prevents the compiler from reordering or
     * eliding writes when the function is called via indirect dispatch. */
    __asm__ volatile("" ::: "memory");
    OF_SVC->opl_write(reg, val);
    __asm__ volatile("" ::: "memory");
}

/* Program a 2-op buzzy guitar instrument on channel 0 of the given bank. */
__attribute__((noinline,noclone))
void xtu_program_guitar(int bank) {
    int base = bank ? 0x100 : 0;
    /* Modulator: op 0 (ch 0 op1 = slot 0) */
    xtu_opl_write(base + 0x20, 0x21);   /* EG=1, MULT=1 */
    xtu_opl_write(base + 0x40, 0x10);   /* TL=16 */
    xtu_opl_write(base + 0x60, 0xF0);   /* AR=15, DR=0 */
    xtu_opl_write(base + 0x80, 0x0F);   /* SL=0, RR=15 */
    xtu_opl_write(base + 0xE0, 0x01);   /* WS=1 (half-sine) */
    /* Carrier: op 3 (ch 0 op2 = slot 3) */
    xtu_opl_write(base + 0x23, 0x21);
    xtu_opl_write(base + 0x43, 0x00);   /* TL=0 (max) */
    xtu_opl_write(base + 0x63, 0xF0);
    xtu_opl_write(base + 0x83, 0x0F);
    xtu_opl_write(base + 0xE3, 0x01);   /* WS=1 */
    /* FB/CNT: high feedback, additive, L+R */
    xtu_opl_write(base + 0xC0, 0x3D);   /* FB=6, CNT=1, L+R */
}

__attribute__((noinline,noclone))
void xtu_key_on_a4(int bank) {
    int base = bank ? 0x100 : 0;
    xtu_opl_write(base + 0xA0, 0x41);   /* F-num low */
    xtu_opl_write(base + 0xB0, 0x32);   /* key on, block 4 */
}

__attribute__((noinline,noclone))
void xtu_key_off(int bank) {
    int base = bank ? 0x100 : 0;
    xtu_opl_write(base + 0xB0, 0x12);
}
