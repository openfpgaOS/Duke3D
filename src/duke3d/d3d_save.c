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
 *   [4..7]    layout_version (OpenFPGA Duke save payload layout)
 *   [8..15]   reserved (zero-padded by truncation on "wb")
 *   [16..19]  magic = SAVE_MAGIC ("DKSV")
 *   [20..]    BUILD dfread/dfwrite payload
 */

#include "d3d_save.h"
#include "d3d_audio.h"
#include "of_file.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void copybufbyte(void *S, void *D, int32_t c);
extern int32_t compress(uint8_t *lzwinbuf, int32_t uncompleng, uint8_t *lzwoutbuf);
extern int32_t uncompress(uint8_t *lzwinbuf, int32_t compleng, uint8_t *lzwoutbuf);

#define LZWSIZE     16384
#define LZWMAX      (LZWSIZE + (LZWSIZE >> 4))
#define SAVE_MAGIC  0x444B5356u    /* "DKSV" */
#define SAVE_SLOT_BASE 10u
#define SAVE_LAYOUT_LEGACY 0u      /* Layout field was previously reserved. */
#define SAVE_LAYOUT 2u             /* Save payload layout, not a build ID */
#define SAVE_LAYOUT_TRANSITIONAL 0x4F465002u
#define LAYOUT_OFF  4
#define MAGIC_OFF   16

static OfSaveFile save_pool[D3D_MAXSAVES];
static int        save_pool_used[D3D_MAXSAVES];
static uint8_t    save_lzw_in[LZWSIZE];
static uint8_t    save_lzw_out[LZWMAX];

static const char *save_slot_names[D3D_MAXSAVES] = {
    "duke3d_0.sav",
    "duke3d_1.sav",
    "duke3d_2.sav",
    "duke3d_3.sav",
    "duke3d_4.sav",
    "duke3d_5.sav",
    "duke3d_6.sav",
    "duke3d_7.sav",
    "duke3d_8.sav",
    "duke3d_9.sav",
};

static void save_register_slots(void) {
    static int registered = 0;
    if (registered)
        return;

    for (int i = 0; i < D3D_MAXSAVES; i++)
        of_file_slot_register(SAVE_SLOT_BASE + (uint32_t)i, save_slot_names[i]);

    registered = 1;
}

static void slot_path(int slot, char *out, size_t out_len) {
    snprintf(out, out_len, "%s", save_slot_names[slot]);
}

static uint32_t load_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_save_header(uint8_t *header, uint32_t data_size) {
    memset(header, 0, D3D_SAVE_HEADER);
    store_le32(&header[0], data_size);
    store_le32(&header[LAYOUT_OFF], SAVE_LAYOUT);
    store_le32(&header[MAGIC_OFF], SAVE_MAGIC);
}

static int read_save_header(FILE *fp, uint32_t *data_size, uint32_t *layout,
                            uint32_t *magic) {
    uint8_t header[D3D_SAVE_HEADER];

    if (fseek(fp, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), fp) != sizeof(header))
        return 0;

    if (data_size) *data_size = load_le32(&header[0]);
    if (layout) *layout = load_le32(&header[LAYOUT_OFF]);
    if (magic) *magic = load_le32(&header[MAGIC_OFF]);
    return 1;
}

static int save_header_valid(uint32_t data_size, uint32_t layout,
                             uint32_t magic) {
    return data_size > D3D_SAVE_HEADER &&
           data_size <= D3D_SAVE_SIZE &&
           (layout == SAVE_LAYOUT_LEGACY ||
            layout == SAVE_LAYOUT ||
            layout == SAVE_LAYOUT_TRANSITIONAL) &&
           magic == SAVE_MAGIC;
}

static void save_mark_error(OfSaveFile *sf) {
    if (sf)
        sf->error = 1;
}

OfSaveFile *save_fopen(int slot, const char *mode) {
    if (slot < 0 || slot >= D3D_MAXSAVES) return NULL;
    if (!mode || (mode[0] != 'r' && mode[0] != 'w')) return NULL;

    save_register_slots();

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
     * the new payload; "rb" opens read-only.  Mirrors the savea/saveb
     * demo's offset==0 path. */
    sf->fp = fopen(path, sf->writing ? "wb" : "rb");
    if (!sf->fp) return NULL;

    if (sf->writing) {
        /* Write a valid header before the payload.  The exact APF file size
         * is still published by the OS close path; the header's size field is
         * only an app-level validity guard, so the slot capacity is a safe
         * value and avoids depending on a later random-access header patch. */
        uint8_t header[D3D_SAVE_HEADER];
        make_save_header(header, D3D_SAVE_SIZE);
        if (fwrite(header, 1, D3D_SAVE_HEADER, sf->fp) != D3D_SAVE_HEADER) {
            fclose(sf->fp);
            sf->fp = NULL;
            return NULL;
        }
    } else {
        /* Read the actual data_size from the header; clamp to the
         * slot ceiling so a corrupt header can't trick callers into
         * reading past the buffer. */
        uint32_t data_size = 0;
        uint32_t layout = 0;
        uint32_t magic = 0;
        if (!read_save_header(sf->fp, &data_size, &layout, &magic) ||
            !save_header_valid(data_size, layout, magic)) {
            fclose(sf->fp);
            sf->fp = NULL;
            return NULL;
        }
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

    int writing = sf->writing;
    int slot    = sf->game_slot;
    int status  = sf->error ? -1 : 0;

    if (fclose(sf->fp) != 0)
        status = -1;
    sf->fp = NULL;

    if (writing && status == 0 && !save_slot_valid(slot))
        status = -1;

    int idx = (int)(sf - save_pool);
    if (idx >= 0 && idx < D3D_MAXSAVES)
        save_pool_used[idx] = 0;

    return status;
}

int save_slot_valid(int slot) {
    if (slot < 0 || slot >= D3D_MAXSAVES) return 0;

    save_register_slots();

    char path[32];
    slot_path(slot, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t data_size = 0;
    uint32_t layout = 0;
    uint32_t magic = 0;
    int ok = read_save_header(f, &data_size, &layout, &magic) &&
             save_header_valid(data_size, layout, magic);
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
    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }

    uint8_t *ptr = (uint8_t *)buffer;
    short    leng = 0;
    int32_t  k = 0, kgoal;

    d3d_audio_pump_loading();
    if (save_fread(&leng, 2, 1, sf) != 1) goto fail;
    if (leng <= 0 || leng > LZWMAX) goto fail;
    if (save_fread(save_lzw_out, (uint32_t)leng, 1, sf) != 1) goto fail;
    d3d_audio_pump_loading();

    kgoal = uncompress(save_lzw_out, (int32_t)leng, save_lzw_in);
    d3d_audio_pump_loading();
    if (kgoal <= 0) goto fail;

    copybufbyte(save_lzw_in, ptr, (int32_t)dasizeof);
    k += (int32_t)dasizeof;

    for (unsigned int i = 1; i < count; i++) {
        if ((i & 4095u) == 0)
            d3d_audio_pump_loading();
        if (k >= kgoal) {
            if (save_fread(&leng, 2, 1, sf) != 1) goto fail;
            if (leng <= 0 || leng > LZWMAX) goto fail;
            if (save_fread(save_lzw_out, (uint32_t)leng, 1, sf) != 1) goto fail;
            d3d_audio_pump_loading();
            k = 0;
            kgoal = uncompress(save_lzw_out, (int32_t)leng, save_lzw_in);
            d3d_audio_pump_loading();
            if (kgoal <= 0) goto fail;
        }
        for (unsigned int j = 0; j < dasizeof; j++)
            ptr[j + dasizeof] = (uint8_t)((ptr[j] + save_lzw_in[j + k]) & 0xFF);
        k   += dasizeof;
        ptr += dasizeof;
    }
    return;

fail:
    save_mark_error(sf);
}

void save_dfwrite(void *buffer, unsigned int dasizeof, unsigned int count,
                  OfSaveFile *sf) {
    if (dasizeof > LZWSIZE) { count *= dasizeof; dasizeof = 1; }

    uint8_t *ptr = (uint8_t *)buffer;
    unsigned int k = dasizeof;

    copybufbyte(ptr, save_lzw_in, (int32_t)dasizeof);

    if (k > LZWSIZE - dasizeof) {
        d3d_audio_pump_loading();
        short leng = (short)compress(save_lzw_in, k, save_lzw_out);
        d3d_audio_pump_loading();
        if (leng <= 0 || leng > LZWMAX) goto fail;
        k = 0;
        if (save_fwrite(&leng, 2, 1, sf) != 1) goto fail;
        if (save_fwrite(save_lzw_out, (uint32_t)leng, 1, sf) != 1) goto fail;
        d3d_audio_pump_loading();
    }

    for (unsigned int i = 1; i < count; i++) {
        if ((i & 4095u) == 0)
            d3d_audio_pump_loading();
        for (unsigned int j = 0; j < dasizeof; j++)
            save_lzw_in[j + k] = (uint8_t)((ptr[j + dasizeof] - ptr[j]) & 0xFF);
        k += dasizeof;
        if (k > LZWSIZE - dasizeof) {
            d3d_audio_pump_loading();
            short leng = (short)compress(save_lzw_in, k, save_lzw_out);
            d3d_audio_pump_loading();
            if (leng <= 0 || leng > LZWMAX) goto fail;
            k = 0;
            if (save_fwrite(&leng, 2, 1, sf) != 1) goto fail;
            if (save_fwrite(save_lzw_out, (uint32_t)leng, 1, sf) != 1) goto fail;
            d3d_audio_pump_loading();
        }
        ptr += dasizeof;
    }
    if (k > 0) {
        d3d_audio_pump_loading();
        short leng = (short)compress(save_lzw_in, k, save_lzw_out);
        d3d_audio_pump_loading();
        if (leng <= 0 || leng > LZWMAX) goto fail;
        if (save_fwrite(&leng, 2, 1, sf) != 1) goto fail;
        if (save_fwrite(save_lzw_out, (uint32_t)leng, 1, sf) != 1) goto fail;
        d3d_audio_pump_loading();
    }
    return;

fail:
    save_mark_error(sf);
}
