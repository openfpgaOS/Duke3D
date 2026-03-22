/*
 * d3d_save.c -- Save file I/O for openfpgaOS
 *
 * Each game save has its own nonvolatile save slot (0-9).
 * Uses standard fopen("save_N")/fread/fwrite/fclose — the OS kernel
 * handles slot mapping and auto-flushes to SD on fclose().
 * LZW compress/uncompress use the BUILD engine software implementation.
 */

#include "d3d_save.h"
#include <stdint.h>
#include <stdio.h>

/* Forward declarations for BUILD engine functions */
extern void copybufbyte(void *S, void *D, int32_t c);
extern int32_t compress(uint8_t *lzwinbuf, int32_t uncompleng, uint8_t *lzwoutbuf);
extern int32_t uncompress(uint8_t *lzwinbuf, int32_t compleng, uint8_t *lzwoutbuf);
extern void *malloc(unsigned int size);
extern void  free(void *ptr);

#define LZWSIZE 16384
#define SAVE_MAGIC 0x444B5356  /* "DKSV" — marks a valid game save */

/* Static pool of save file handles */
static OfSaveFile save_pool[D3D_MAXSAVES];
static int      save_pool_used[D3D_MAXSAVES];

/* Save slot name buffer */
static char save_path[16];

static const char *save_slot_path(int slot) {
    /* Build "save_N" string */
    save_path[0] = 's'; save_path[1] = 'a'; save_path[2] = 'v';
    save_path[3] = 'e'; save_path[4] = '_';
    if (slot >= 10) {
        save_path[5] = '0' + (slot / 10);
        save_path[6] = '0' + (slot % 10);
        save_path[7] = '\0';
    } else {
        save_path[5] = '0' + slot;
        save_path[6] = '\0';
    }
    return save_path;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

OfSaveFile *save_fopen(int slot, const char *mode) {
    if (slot < 0 || slot >= D3D_MAXSAVES)
        return (OfSaveFile *)0;

    int idx = -1;
    for (int i = 0; i < D3D_MAXSAVES; i++) {
        if (!save_pool_used[i]) { idx = i; break; }
    }
    if (idx < 0) return (OfSaveFile *)0;

    OfSaveFile *sf = &save_pool[idx];
    sf->game_slot = slot;
    sf->offset    = D3D_SAVE_HEADER;
    sf->size      = D3D_SAVE_SIZE;
    sf->writing   = (mode[0] == 'w') ? 1 : 0;

    /* Open the underlying OS save slot via POSIX */
    sf->fp = fopen(save_slot_path(slot), sf->writing ? "r+b" : "rb");
    if (!sf->fp) {
        /* If r+b fails (new save), try wb for write */
        if (sf->writing)
            sf->fp = fopen(save_slot_path(slot), "wb");
        if (!sf->fp)
            return (OfSaveFile *)0;
    }

    if (!sf->writing) {
        /* Read actual data size stored at offset 0 */
        uint32_t data_size = 0;
        fseek(sf->fp, 0, 0);
        fread(&data_size, 4, 1, sf->fp);
        if (data_size > D3D_SAVE_HEADER && data_size <= D3D_SAVE_SIZE)
            sf->size = data_size;
    }

    /* Position at data start */
    fseek(sf->fp, D3D_SAVE_HEADER, 0);

    save_pool_used[idx] = 1;
    return sf;
}

unsigned int save_fread(void *buf, unsigned int size, unsigned int count,
                        OfSaveFile *sf) {
    uint32_t total = size * count;
    if (sf->offset >= sf->size) return 0;
    uint32_t avail = sf->size - sf->offset;
    if (total > avail) total = avail;
    if (total == 0) return 0;

    fread(buf, 1, total, sf->fp);
    sf->offset += total;
    return count;
}

unsigned int save_fwrite(const void *buf, unsigned int size, unsigned int count,
                         OfSaveFile *sf) {
    uint32_t total = size * count;
    if (sf->offset >= sf->size) return 0;
    uint32_t avail = sf->size - sf->offset;
    if (total > avail) total = avail;
    if (total == 0) return 0;

    fwrite(buf, 1, total, sf->fp);
    sf->offset += total;
    return count;
}

int save_fseek(OfSaveFile *sf, long offset, int whence) {
    long new_off;
    switch (whence) {
    case 0: new_off = offset; break;
    case 1: new_off = (long)sf->offset + offset; break;
    case 2: new_off = (long)sf->size + offset; break;
    default: return -1;
    }
    if (new_off < 0) new_off = 0;
    if (new_off > (long)sf->size) new_off = (long)sf->size;
    sf->offset = (uint32_t)new_off;
    fseek(sf->fp, sf->offset, 0);
    return 0;
}

void save_fclose(OfSaveFile *sf) {
    if (!sf) return;

    if (sf->writing) {
        /* Store actual data size at offset 0 */
        uint32_t data_size = sf->offset;
        fseek(sf->fp, 0, 0);
        fwrite(&data_size, 4, 1, sf->fp);

        /* Write magic to mark slot valid */
        uint32_t magic = SAVE_MAGIC;
        fseek(sf->fp, 16, 0);
        fwrite(&magic, 4, 1, sf->fp);
    }

    /* fclose auto-flushes to SD with actual written size */
    fclose(sf->fp);
    sf->fp = (void *)0;

    int idx = (int)(sf - save_pool);
    if (idx >= 0 && idx < D3D_MAXSAVES)
        save_pool_used[idx] = 0;
}

int save_slot_valid(int slot) {
    if (slot < 0 || slot >= D3D_MAXSAVES)
        return 0;

    uint32_t magic = 0;
    FILE *f = fopen(save_slot_path(slot), "rb");
    if (!f) return 0;
    fseek(f, 16, 0);
    fread(&magic, 4, 1, f);
    fclose(f);

    return magic == SAVE_MAGIC;
}

/* ======================================================================
 * LZW-compressed I/O (BUILD engine dfread/dfwrite format)
 * ====================================================================== */

void save_dfread(void *buffer, unsigned int dasizeof, unsigned int count,
                 OfSaveFile *sf) {
    unsigned int i, j;
    int32_t k, kgoal;
    short leng;
    uint8_t *ptr;
    uint8_t *lbuf4, *lbuf5;
    int32_t maxleng = LZWSIZE + (LZWSIZE >> 4);

    lbuf4 = (uint8_t *)malloc(LZWSIZE);
    lbuf5 = (uint8_t *)malloc(maxleng);
    if (!lbuf4 || !lbuf5) goto out;

    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }
    ptr = (uint8_t *)buffer;

    save_fread(&leng, 2, 1, sf);
    if (leng <= 0 || leng > maxleng) goto out;
    save_fread(lbuf5, (int32_t)leng, 1, sf);

    k = 0;
    kgoal = uncompress(lbuf5, (int32_t)leng, lbuf4);
    if (kgoal <= 0) goto out;

    copybufbyte(lbuf4, ptr, (int32_t)dasizeof);
    k += (int32_t)dasizeof;

    for (i = 1; i < count; i++) {
        if (k >= kgoal) {
            save_fread(&leng, 2, 1, sf);
            if (leng <= 0 || leng > maxleng) goto out;
            save_fread(lbuf5, (int32_t)leng, 1, sf);
            k = 0;
            kgoal = uncompress(lbuf5, (int32_t)leng, lbuf4);
            if (kgoal <= 0) goto out;
        }
        for (j = 0; j < dasizeof; j++)
            ptr[j + dasizeof] = (uint8_t)((ptr[j] + lbuf4[j + k]) & 255);
        k += dasizeof;
        ptr += dasizeof;
    }

out:
    free(lbuf4);
    free(lbuf5);
}

void save_dfwrite(void *buffer, unsigned int dasizeof, unsigned int count,
                  OfSaveFile *sf) {
    unsigned int i, j, k;
    short leng;
    uint8_t *ptr;
    uint8_t *lbuf4, *lbuf5;

    lbuf4 = (uint8_t *)malloc(LZWSIZE);
    lbuf5 = (uint8_t *)malloc(LZWSIZE + (LZWSIZE >> 4));
    if (!lbuf4 || !lbuf5) { free(lbuf4); free(lbuf5); return; }

    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }
    ptr = (uint8_t *)buffer;

    copybufbyte(ptr, lbuf4, (int32_t)dasizeof);
    k = dasizeof;

    if (k > LZWSIZE - dasizeof) {
        leng = (short)compress(lbuf4, k, lbuf5);
        k = 0;
        save_fwrite(&leng, 2, 1, sf);
        save_fwrite(lbuf5, (int32_t)leng, 1, sf);
    }

    for (i = 1; i < count; i++) {
        for (j = 0; j < dasizeof; j++)
            lbuf4[j + k] = (uint8_t)((ptr[j + dasizeof] - ptr[j]) & 255);
        k += dasizeof;
        if (k > LZWSIZE - dasizeof) {
            leng = (short)compress(lbuf4, k, lbuf5);
            k = 0;
            save_fwrite(&leng, 2, 1, sf);
            save_fwrite(lbuf5, (int32_t)leng, 1, sf);
        }
        ptr += dasizeof;
    }
    if (k > 0) {
        leng = (short)compress(lbuf4, k, lbuf5);
        save_fwrite(&leng, 2, 1, sf);
        save_fwrite(lbuf5, (int32_t)leng, 1, sf);
    }

    free(lbuf4);
    free(lbuf5);
}
