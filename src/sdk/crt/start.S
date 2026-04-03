/*
 * openfpgaOS App CRT startup
 * Calls C++ static constructors, then main(), then destructors.
 */

.section .text.start
.global _start
_start:
    /* Stack and arguments are already set up by the OS loader. */

    /* ── Call C++ static constructors (.init_array) ───────────── */
    la   s0, __init_array_start
    la   s1, __init_array_end
.Linit_loop:
    bgeu s0, s1, .Linit_done
    lw   s2, 0(s0)
    jalr ra, s2, 0
    addi s0, s0, 4
    j    .Linit_loop
.Linit_done:

    /* ── Call main() ──────────────────────────────────────────── */
    jal  ra, main
    mv   s3, a0             /* save return value */

    /* ── Call C++ static destructors (.fini_array, reverse) ──── */
    la   s0, __fini_array_end
    la   s1, __fini_array_start
.Lfini_loop:
    bgeu s1, s0, .Lfini_done
    addi s0, s0, -4
    lw   s2, 0(s0)
    jalr ra, s2, 0
    j    .Lfini_loop
.Lfini_done:

    /* ── Exit via syscall ─────────────────────────────────────── */
    li   a7, 93             /* SYS_exit */
    mv   a0, s3             /* exit with main()'s return value */
    ecall
1:
    j    1b
