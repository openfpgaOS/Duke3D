/*
 * openfpgaOS App CRT startup
 * Minimal _start that calls main()
 */

.section .text.start
.global _start
_start:
    /* For bare apps (no musl), just call main directly.
     * Stack and arguments are already set up by the OS loader. */
    jal ra, main

    /* If main returns, exit via syscall */
    li a7, 93       /* SYS_exit */
    mv a0, zero
    ecall
1:
    j 1b
