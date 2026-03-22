/*
 * posix_shim.c -- Duke3D-specific stubs for openfpgaOS
 *
 * POSIX I/O, libc symbols, and printf are provided by the SDK runtime
 * (sdk/of_posix.c). This file only contains game-specific glue.
 */

#include <stdint.h>
#include "of_file.h"

/* Register Duke3D's data files with the OS file slot registry. */
void pocket_register_file_slots(void) {
    of_file_slot_register(3, "duke3d.grp");
}

/* Duke3D's filelength() -- get file size via lseek */
extern long lseek(int, long, int);
long filelength(int fd) {
    long cur = lseek(fd, 0, 1);  /* SEEK_CUR */
    long end = lseek(fd, 0, 2);  /* SEEK_END */
    lseek(fd, cur, 0);           /* SEEK_SET */
    return end;
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

/* SDL stubs — referenced by multivoc.c / menues.c */
void *SDL_CreateMutex(void) { return (void *)0; }
void  SDL_DestroyMutex(void *m) { (void)m; }
int   SDL_mutexP(void *m) { (void)m; return 0; }
int   SDL_mutexV(void *m) { (void)m; return 0; }
int   SDL_GetRelativeMouseMode(void) { return 1; }
int   SDL_SetRelativeMouseMode(int m) { (void)m; return 0; }
