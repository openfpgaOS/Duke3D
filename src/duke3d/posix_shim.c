/*
 * posix_shim.c -- Duke3D-specific stubs for openfpgaOS
 *
 * POSIX I/O, libc symbols, and printf come straight from upstream musl
 * (statically linked by sdk.mk). This file only contains game-specific
 * glue: filelength, STUBBED, getch, Z_AvailHeap, the global min/max
 * functions the BUILD audiolib expects, etc.
 */

#include <stdint.h>
#include <unistd.h>

/* BUILD/audiolib uses min/max as plain functions (not standard C).
 * Define them globally so multivoc.c et al. link cleanly. */
int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }

/* Duke3D's filelength() -- get file size via lseek */
long filelength(int fd) {
    long long cur = lseek(fd, 0, 1);  /* SEEK_CUR */
    long long end = lseek(fd, 0, 2);  /* SEEK_END */
    lseek(fd, cur, 0);                /* SEEK_SET */
    return (long)end;
}

/* Duke3D control.c uses STUBBED() macro */
void STUBBED(const char *msg, const char *file, int line) {
    (void)msg; (void)file; (void)line;
}

/* getch() used by filesystem.c for "press any key" prompts */
int getch(void) { return 'y'; }

/* Z_AvailHeap — Duke3D memory check */
long Z_AvailHeap(void) { return 32 * 1024 * 1024; }

/* lastPalette — display state referenced by engine.c */
unsigned char lastPalette[768];

/* Multiplayer stubs */
void sendpacket(int other, const unsigned char *buf, int len) { (void)other; (void)buf; (void)len; }
void sendlogoff(void) {}
int getpacket(int *from, unsigned char *buf) { (void)from; (void)buf; return 0; }
void initmultiplayers(int argc, char **argv, int a, int b, int c) {
    (void)argc; (void)argv; (void)a; (void)b; (void)c;
}
void uninitmultiplayers(void) {}
void setpackettimeout(int a, int b) { (void)a; (void)b; }
int getoutputcirclesize(void) { return 0; }
void genericmultifunction(int a, const unsigned char *b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
void sendlogon(void) {}
void flushpackets(void) {}

/* DSL stubs — Multivoc disabled, all audio via OS mixer */
char *DSL_ErrorString(int n) { (void)n; return "OS mixer"; }
int DSL_Init(void) { return 0; }
void DSL_Shutdown(void) {}
int DSL_BeginBufferedPlayback(char *b, int s, int n, unsigned r, int m, void (*c)(void)) {
    (void)b; (void)s; (void)n; (void)r; (void)m; (void)c; return 0;
}
void DSL_StopPlayback(void) {}
unsigned DSL_GetPlaybackRate(void) { return 48000; }
void DSL_PumpAudio(void) {}

/* SDL stubs — referenced by multivoc.c / menues.c.  Only the SDL 1.x
 * names the SDK's of_sdl2.c does NOT provide; mutex create/destroy and
 * SDL_SetRelativeMouseMode now come from the SDK compat layer. */
int   SDL_mutexP(void *m) { (void)m; return 0; }
int   SDL_mutexV(void *m) { (void)m; return 0; }
int   SDL_GetRelativeMouseMode(void) { return 1; }
