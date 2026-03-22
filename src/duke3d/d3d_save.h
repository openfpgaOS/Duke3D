/*
 * d3d_save.h -- Save file I/O for openfpgaOS
 *
 * Each game save has its own nonvolatile data slot (ids 10-19).
 * Uses of_save_read/write with the data slot id directly.
 */

#ifndef D3D_SAVE_H
#define D3D_SAVE_H

#include <stdint.h>

#define D3D_MAXSAVES      10
#define D3D_SAVE_SLOT_BASE 10             /* first data slot id in instance JSON */
#define D3D_SAVE_SIZE     (256 * 1024)    /* 256KB per save slot */
#define D3D_SAVE_HEADER   20              /* 4-byte magic at offset 16, data starts at 20 */

typedef struct {
    int      game_slot;     /* which game save (0..D3D_MAXSAVES-1) = OS save slot index */
    uint32_t offset;        /* current byte position within save slot */
    uint32_t size;          /* total size of save slot */
    int      writing;       /* 1 = write mode, 0 = read mode */
} OfSaveFile;

OfSaveFile *save_fopen(int slot, const char *mode);
unsigned int save_fread(void *buf, unsigned int size, unsigned int count, OfSaveFile *sf);
unsigned int save_fwrite(const void *buf, unsigned int size, unsigned int count, OfSaveFile *sf);
int save_fseek(OfSaveFile *sf, long offset, int whence);
void save_fclose(OfSaveFile *sf);
void save_dfread(void *buffer, unsigned int dasizeof, unsigned int count, OfSaveFile *sf);
void save_dfwrite(void *buffer, unsigned int dasizeof, unsigned int count, OfSaveFile *sf);
int save_slot_valid(int slot);

#endif /* D3D_SAVE_H */
