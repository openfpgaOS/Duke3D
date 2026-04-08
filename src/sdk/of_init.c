/*
 * of_init.c -- SDK runtime init
 *
 * Auto-linked into every openfpgaOS app via sdk.mk. Anything that
 * needs to run unconditionally before main() lives here.
 *
 * Currently:
 *   - Forces stdout to unbuffered. musl's default leaves stdout fully
 *     buffered (BUFSIZ ~= 4KB) when isatty() fails, and even with the
 *     OS's ioctl stub it stays line buffered — neither is what apps
 *     drawing direct cursor-positioned UI want, since printf calls
 *     without a trailing '\n' never reach the terminal until the
 *     buffer fills. Unbuffered matches the screen-update model every
 *     SDK demo actually uses.
 */

#include <stdio.h>

__attribute__((constructor))
static void of_stdio_init(void) {
    setbuf(stdout, NULL);
}
