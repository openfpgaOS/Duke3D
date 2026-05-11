/*
 * d3d_save.h -- Save file I/O for openfpgaOS
 *
 * Each game save has its own nonvolatile save slot (0-9).
 * Uses the APF manifest filenames duke3d_N.sav; the OS auto-flushes
 * to SD card with actual written size on fclose().
 */

#ifndef D3D_SAVE_H
#define D3D_SAVE_H

#include <stdint.h>

#define D3D_MAXSAVES      10
#define D3D_SAVE_SIZE     (256 * 1024)    /* 256KB per save slot */
#define D3D_SAVE_HEADER   20              /* size, layout version, reserved, magic; payload at 20 */

typedef struct {
    void    *fp;            /* FILE* to the underlying save slot */
    int      game_slot;     /* which game save (0..D3D_MAXSAVES-1) */
    uint32_t offset;        /* current byte position within save slot */
    uint32_t size;          /* total size of save slot */
    int      writing;       /* 1 = write mode, 0 = read mode */
    int      error;         /* sticky I/O error for write/close reporting */
} OfSaveFile;

OfSaveFile *save_fopen(int slot, const char *mode);
unsigned int save_fread(void *buf, unsigned int size, unsigned int count, OfSaveFile *sf);
unsigned int save_fwrite(const void *buf, unsigned int size, unsigned int count, OfSaveFile *sf);
int save_fseek(OfSaveFile *sf, long offset, int whence);
int save_fclose(OfSaveFile *sf);
void save_dfread(void *buffer, unsigned int dasizeof, unsigned int count, OfSaveFile *sf);
void save_dfwrite(void *buffer, unsigned int dasizeof, unsigned int count, OfSaveFile *sf);
int save_slot_valid(int slot);

#endif /* D3D_SAVE_H */
