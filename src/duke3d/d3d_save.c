/*
 * d3d_save.c -- Save file I/O for openfpgaOS.
 *
 * Each game save lives in its own nonvolatile APF data slot.  Duke's
 * core metadata names those slots duke3d_0.sav .. duke3d_9.sav, so we
 * open the manifest filenames directly instead of relying on save_N
 * aliases.  The OS auto-flushes to SD on fclose().
 *
 * File layout written by us (BUILD's dfread/dfwrite expects an
 * uncompressed body; the wrapper adds a small fixed header so we can
 * cheaply validate a slot without decoding the LZW stream):
 *
 *   [0..3]    data_size  (uint32, total bytes written including header)
 *   [4..15]   reserved (zero-padded by truncation on "wb")
 *   [16..19]  magic = SAVE_MAGIC ("DKSV")
 *   [20..]    BUILD dfread/dfwrite payload
 */

#include "d3d_save.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void copybufbyte(void *S, void *D, int32_t c);
extern int32_t compress(uint8_t *lzwinbuf, int32_t uncompleng, uint8_t *lzwoutbuf);
extern int32_t uncompress(uint8_t *lzwinbuf, int32_t compleng, uint8_t *lzwoutbuf);
extern void *malloc(unsigned int size);
extern void  free(void *ptr);

#define LZWSIZE     16384
#define SAVE_MAGIC  0x444B5356u    /* "DKSV" */
#define MAGIC_OFF   16

static OfSaveFile save_pool[D3D_MAXSAVES];
static int        save_pool_used[D3D_MAXSAVES];

static void slot_path(int slot, char *out, size_t out_len) {
    snprintf(out, out_len, "duke3d_%d.sav", slot);
}

OfSaveFile *save_fopen(int slot, const char *mode) {
    if (slot < 0 || slot >= D3D_MAXSAVES) return NULL;
    if (!mode || (mode[0] != 'r' && mode[0] != 'w')) return NULL;

    int idx = -1;
    for (int i = 0; i < D3D_MAXSAVES; i++) {
        if (!save_pool_used[i]) { idx = i; break; }
    }
    if (idx < 0) return NULL;

    OfSaveFile *sf = &save_pool[idx];
    sf->game_slot = slot;
    sf->offset    = D3D_SAVE_HEADER;
    sf->size      = D3D_SAVE_SIZE;
    sf->writing   = (mode[0] == 'w');
    sf->error     = 0;

    char path[32];
    slot_path(slot, path, sizeof(path));

    /* "wb" truncates so previous saves don't leave stale bytes past
     * the new data_size; "rb" opens read-only.  Mirrors the savea/
     * saveb demo's offset==0 path. */
    sf->fp = fopen(path, sf->writing ? "wb" : "rb");
    if (!sf->fp) return NULL;

    if (sf->writing) {
        /* Reserve the header region by writing 20 zero bytes up front.
         * The kernel VFS does NOT honor fseek-past-EOF on a "wb" handle
         * (savea/saveb avoids this by using "r+b" for nonzero offsets),
         * so we must extend the file naturally before payload writes.
         * save_fclose() patches data_size and magic over these zeros via
         * an "r+b" reopen. */
        static const uint8_t zero_header[D3D_SAVE_HEADER] = {0};
        if (fwrite(zero_header, 1, D3D_SAVE_HEADER, sf->fp) != D3D_SAVE_HEADER) {
            fclose(sf->fp);
            sf->fp = NULL;
            return NULL;
        }
    } else {
        /* Read the actual data_size from the header; clamp to the
         * slot ceiling so a corrupt header can't trick callers into
         * reading past the buffer. */
        uint32_t data_size = 0;
        if (fread(&data_size, 4, 1, sf->fp) == 1 &&
            data_size > D3D_SAVE_HEADER && data_size <= D3D_SAVE_SIZE)
            sf->size = data_size;

        /* Position at the payload start. Backward/forward seek on a "rb"
         * handle is fine — the kernel only restricts seeks on write
         * handles. */
        if (fseek(sf->fp, D3D_SAVE_HEADER, SEEK_SET) != 0) {
            fclose(sf->fp);
            sf->fp = NULL;
            return NULL;
        }
    }

    save_pool_used[idx] = 1;
    return sf;
}

unsigned int save_fread(void *buf, unsigned int size, unsigned int count,
                        OfSaveFile *sf) {
    if (!sf || !sf->fp || size == 0 || count == 0) return 0;
    if (sf->offset >= sf->size) return 0;

    uint32_t total = size * count;
    uint32_t avail = sf->size - sf->offset;
    if (total > avail)
        total = avail;

    size_t got = fread(buf, 1, total, sf->fp);
    if (got != total && ferror(sf->fp))
        sf->error = 1;
    sf->offset += (uint32_t)got;
    /* Per stdio convention: return number of complete items read. */
    return (unsigned int)(got / size);
}

unsigned int save_fwrite(const void *buf, unsigned int size, unsigned int count,
                         OfSaveFile *sf) {
    if (!sf || !sf->fp || size == 0 || count == 0) return 0;
    if (sf->offset >= sf->size) return 0;

    uint32_t total = size * count;
    uint32_t avail = sf->size - sf->offset;
    if (total > avail) {
        total = avail;
        sf->error = 1;
    }

    size_t put = fwrite(buf, 1, total, sf->fp);
    if (put != total)
        sf->error = 1;
    sf->offset += (uint32_t)put;
    return (unsigned int)(put / size);
}

int save_fseek(OfSaveFile *sf, long offset, int whence) {
    if (!sf || !sf->fp) return -1;

    long new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (long)sf->offset + offset; break;
    case SEEK_END: new_off = (long)sf->size + offset; break;
    default: return -1;
    }
    if (new_off < 0) new_off = 0;
    if (new_off > (long)sf->size) new_off = (long)sf->size;
    if (fseek(sf->fp, new_off, SEEK_SET) != 0) {
        sf->error = 1;
        return -1;
    }
    sf->offset = (uint32_t)new_off;
    return 0;
}

int save_fclose(OfSaveFile *sf) {
    if (!sf || !sf->fp) return -1;

    /* Snapshot the final payload size BEFORE closing the wb handle —
     * sf->offset advances past D3D_SAVE_HEADER+payload during writes. */
    uint32_t data_size = sf->offset;
    int      writing   = sf->writing;
    int      slot      = sf->game_slot;
    int      status    = sf->error ? -1 : 0;

    if (fclose(sf->fp) != 0)
        status = -1;
    sf->fp = NULL;

    if (writing) {
        /* Header back-fill via a fresh "r+b" reopen — matches the
         * savea/saveb demo's pattern (`fopen(path, offset==0 ? "wb" :
         * "r+b")`).  Some kernel VFS impls don't preserve backward
         * seeks across a single "wb" handle, so updating the header
         * inline from the same handle silently drops on close.
         * Reopening with "r+b" gives us a writable handle with random
         * access on the now-persisted bytes. */
        char path[32];
        slot_path(slot, path, sizeof(path));
        FILE *fp = fopen(path, "r+b");
        if (fp) {
            uint32_t magic = SAVE_MAGIC;
            if (fseek(fp, 0, SEEK_SET) != 0 ||
                fwrite(&data_size, 4, 1, fp) != 1 ||
                fseek(fp, MAGIC_OFF, SEEK_SET) != 0 ||
                fwrite(&magic, 4, 1, fp) != 1)
                status = -1;
            if (fclose(fp) != 0)
                status = -1;
        } else {
            status = -1;
        }

        if (status == 0 && !save_slot_valid(slot))
            status = -1;
    }

    int idx = (int)(sf - save_pool);
    if (idx >= 0 && idx < D3D_MAXSAVES)
        save_pool_used[idx] = 0;

    return status;
}

int save_slot_valid(int slot) {
    if (slot < 0 || slot >= D3D_MAXSAVES) return 0;

    char path[32];
    slot_path(slot, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic = 0;
    int ok = (fseek(f, MAGIC_OFF, SEEK_SET) == 0 &&
              fread(&magic, 4, 1, f) == 1 &&
              magic == SAVE_MAGIC);
    fclose(f);
    return ok;
}

/* ======================================================================
 * LZW-compressed I/O — BUILD's dfread/dfwrite delta+LZW chunk format.
 * Each chunk on disk: int16 leng, then `leng` bytes of compressed data.
 * Per element, the user buffer holds the decoded value; the on-disk
 * stream stores deltas (current - previous, byte-wise) so common
 * structures (sectors, sprites) compress aggressively.
 * ====================================================================== */

void save_dfread(void *buffer, unsigned int dasizeof, unsigned int count,
                 OfSaveFile *sf) {
    int32_t maxleng = LZWSIZE + (LZWSIZE >> 4);

    uint8_t *lbuf4 = (uint8_t *)malloc(LZWSIZE);
    uint8_t *lbuf5 = (uint8_t *)malloc(maxleng);
    if (!lbuf4 || !lbuf5) goto out;

    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }

    uint8_t *ptr = (uint8_t *)buffer;
    short    leng = 0;
    int32_t  k = 0, kgoal;

    if (save_fread(&leng, 2, 1, sf) != 1) goto out;
    if (leng <= 0 || leng > maxleng) goto out;
    if (save_fread(lbuf5, (uint32_t)leng, 1, sf) != 1) goto out;

    kgoal = uncompress(lbuf5, (int32_t)leng, lbuf4);
    if (kgoal <= 0) goto out;

    copybufbyte(lbuf4, ptr, (int32_t)dasizeof);
    k += (int32_t)dasizeof;

    for (unsigned int i = 1; i < count; i++) {
        if (k >= kgoal) {
            if (save_fread(&leng, 2, 1, sf) != 1) goto out;
            if (leng <= 0 || leng > maxleng) goto out;
            if (save_fread(lbuf5, (uint32_t)leng, 1, sf) != 1) goto out;
            k = 0;
            kgoal = uncompress(lbuf5, (int32_t)leng, lbuf4);
            if (kgoal <= 0) goto out;
        }
        for (unsigned int j = 0; j < dasizeof; j++)
            ptr[j + dasizeof] = (uint8_t)((ptr[j] + lbuf4[j + k]) & 0xFF);
        k   += dasizeof;
        ptr += dasizeof;
    }

out:
    free(lbuf4);
    free(lbuf5);
}

void save_dfwrite(void *buffer, unsigned int dasizeof, unsigned int count,
                  OfSaveFile *sf) {
    uint8_t *lbuf4 = (uint8_t *)malloc(LZWSIZE);
    uint8_t *lbuf5 = (uint8_t *)malloc(LZWSIZE + (LZWSIZE >> 4));
    if (!lbuf4 || !lbuf5) { free(lbuf4); free(lbuf5); return; }

    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }

    uint8_t *ptr = (uint8_t *)buffer;
    unsigned int k = dasizeof;

    copybufbyte(ptr, lbuf4, (int32_t)dasizeof);

    if (k > LZWSIZE - dasizeof) {
        short leng = (short)compress(lbuf4, k, lbuf5);
        k = 0;
        save_fwrite(&leng, 2, 1, sf);
        save_fwrite(lbuf5, (uint32_t)leng, 1, sf);
    }

    for (unsigned int i = 1; i < count; i++) {
        for (unsigned int j = 0; j < dasizeof; j++)
            lbuf4[j + k] = (uint8_t)((ptr[j + dasizeof] - ptr[j]) & 0xFF);
        k += dasizeof;
        if (k > LZWSIZE - dasizeof) {
            short leng = (short)compress(lbuf4, k, lbuf5);
            k = 0;
            save_fwrite(&leng, 2, 1, sf);
            save_fwrite(lbuf5, (uint32_t)leng, 1, sf);
        }
        ptr += dasizeof;
    }
    if (k > 0) {
        short leng = (short)compress(lbuf4, k, lbuf5);
        save_fwrite(&leng, 2, 1, sf);
        save_fwrite(lbuf5, (uint32_t)leng, 1, sf);
    }

    free(lbuf4);
    free(lbuf5);
}
