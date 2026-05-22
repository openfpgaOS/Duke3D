//
//  tiles.c
//  Duke3D
//
//  Created by fabien sanglard on 12-12-22.
//  Copyright (c) 2012 fabien sanglard. All rights reserved.
//

#include <stdlib.h>
#include <string.h>

#include "tiles.h"
#include "engine.h"
#include "draw.h"
#include "filesystem.h"
#ifdef OPENFPGA
#include "of_cache.h"
#include "of_file.h"
#include "of_timer.h"
#include "../../d3d_audio.h"
#include "../../d3d_gpu.h"
#endif

char  artfilename[20];

tile_t tiles[MAXTILES];

int32_t numTiles;

int32_t artversion;

uint8_t  *pic = NULL;

uint8_t  gotpic[(MAXTILES+7)>>3];

#ifdef OPENFPGA
static int tile_bulk_preload_depth = 0;
static int tile_bulk_preload_dirty = 0;

static void tile_prepare_for_gpu_write(void)
{
    if (tile_bulk_preload_depth > 0)
        return;

    /* A hot tile load can reuse BUILD cache memory that queued GPU spans
     * still reference.  Drain before allocache()/writes can recycle or
     * overwrite those bytes; the post-write sync below then makes the new
     * tile visible to the texture cache. */
    d3d_gpu_drain();
}

static void tile_sync_for_gpu(uint8_t *ptr, uint32_t size)
{
    if (!ptr || size == 0)
        return;

    /* Hot loads may be rendered in the same frame.  Publish the changed
     * tile bytes with the range writeback+invalidate service, then flush
     * the GPU texture cache before the next textured span can sample stale
     * lines. */
    d3d_audio_pump_loading();
    of_cache_flush_range(ptr, size);
    d3d_audio_pump_loading();
    __asm__ volatile("fence" ::: "memory");
    d3d_gpu_tex_invalidate();
}

static void tile_note_loaded_for_gpu(uint8_t *ptr, uint32_t size)
{
    if (!ptr || size == 0)
        return;

    if (tile_bulk_preload_depth > 0) {
        tile_bulk_preload_dirty = 1;
        return;
    }

    tile_sync_for_gpu(ptr, size);
}

static void tile_begin_bulk_preload(void)
{
    tile_bulk_preload_depth++;
}

static void tile_end_bulk_preload(void)
{
    if (tile_bulk_preload_depth <= 0)
        return;

    tile_bulk_preload_depth--;
    if (tile_bulk_preload_depth != 0 || !tile_bulk_preload_dirty)
        return;

    /* Level preload does not render with the freshly loaded textures until
     * after docacheit() returns, so one full D-cache flush is enough. Avoid
     * the per-tile uncached byte mirror, which is correct for hot loads but
     * dominates the all-ART preload path.
     */
    d3d_audio_pump_loading();
    of_cache_flush();
    d3d_audio_pump_loading();
    __asm__ volatile("fence" ::: "memory");
    d3d_gpu_drain();
    d3d_audio_pump_loading();
    d3d_gpu_tex_invalidate();
    tile_bulk_preload_dirty = 0;
}

static void tile_art_filename(char *dst, int32_t filenum)
{
    strcpy(dst, artfilename);
    dst[7] = (filenum % 10) + 48;
    dst[6] = ((filenum / 10) % 10) + 48;
    dst[5] = ((filenum / 100) % 10) + 48;
}

static int tile_marked_for_load(int32_t tilenume)
{
    if ((gotpic[tilenume >> 3] & (1 << (tilenume & 7))) == 0)
        return 0;
    if (tiles[tilenume].data != NULL)
        return 0;
    if (tiles[tilenume].dim.width <= 0 || tiles[tilenume].dim.height <= 0)
        return 0;
    return 1;
}
#endif

void setviewtotile(short tilenume, int32_t tileWidth, int32_t tileHeight)
{
    int32_t i, j;
    
    /* Redirect rendering into a BUILD tile. */
    tiles[tilenume].dim.width = tileWidth;
    tiles[tilenume].dim.height = tileHeight;
    bakxsiz[setviewcnt] = tileWidth;
    bakysiz[setviewcnt] = tileHeight;
    bakbytesperline[setviewcnt] = bytesperline;
    bytesperline = tileHeight;
    setBytesPerLine(tileHeight);
    bakvidoption[setviewcnt] = vidoption;
    vidoption = 2;
    bakframeplace[setviewcnt] = frameplace;
#ifdef OPENFPGA
    bakviewtiledata[setviewcnt] = tiles[tilenume].data;
    if (bakviewtiledata[setviewcnt] != NULL)
    {
        uint32_t tileBytes = (uint32_t)tileWidth * (uint32_t)tileHeight;
        of_cache_inval_range(bakviewtiledata[setviewcnt], tileBytes);
        frameplace = (uint8_t *)of_uncached(bakviewtiledata[setviewcnt]);
        d3d_gpu_set_fb(bakviewtiledata[setviewcnt], tileHeight);
    }
    else
    {
        frameplace = NULL;
        d3d_gpu_set_fb(NULL, tileHeight);
    }
#else
    frameplace = tiles[tilenume].data;
#endif
#ifdef OPENFPGA
    if (d3d_gpu_force_cpu_spans && frameplace != NULL)
        memset(frameplace, 0, (size_t)tileWidth * (size_t)tileHeight);
    else
        d3d_gpu_clear_rect_fb(frameplace, (uint16_t)tileHeight,
                              (uint16_t)tileWidth, 0);
#endif
    bakwindowx1[setviewcnt] = windowx1;
    bakwindowy1[setviewcnt] = windowy1;
    bakwindowx2[setviewcnt] = windowx2;
    bakwindowy2[setviewcnt] = windowy2;
    copybufbyte(&startumost[windowx1],&bakumost[windowx1],(windowx2-windowx1+1)*sizeof(bakumost[0]));
    copybufbyte(&startdmost[windowx1],&bakdmost[windowx1],(windowx2-windowx1+1)*sizeof(bakdmost[0]));
    setview(0,0,tileHeight-1,tileWidth-1);
    setaspect(65536,65536);
    j = 0;
    for(i=0; i<=tileWidth; i++) {
        ylookup[i] = j;
        j += tileHeight;
    }
    setviewcnt++;
}




void squarerotatetile(short tilenume)
{
    int32_t i, j, k;
    uint8_t  *ptr1, *ptr2;
    
    dimensions_t tileDim;
    
    tileDim.width = tiles[tilenume].dim.width;
    tileDim.height = tiles[tilenume].dim.height;
    
    /* supports square tiles only for rotation part */
    if (tileDim.width == tileDim.height)
    {
        k = (tileDim.width<<1);
        for(i=tileDim.width-1; i>=0; i--)
        {
            ptr1 = tiles[tilenume].data+i*(tileDim.width+1);
            ptr2 = ptr1;
            if ((i&1) != 0) {
                ptr1--;
                ptr2 -= tileDim.width;
                swapchar(ptr1,ptr2);
            }
            for(j=(i>>1)-1; j>=0; j--)
            {
                ptr1 -= 2;
                ptr2 -= k;
                swapchar2(ptr1,ptr2,tileDim.width);
            }
        }
    }
}



/* Lock a tile in the cache and mark it as used in the bitvector tracker. */
void setgotpic(int32_t tilenume)
{
    if (tiles[tilenume].lock < 200)
        tiles[tilenume].lock = 199;
    
    gotpic[tilenume>>3] |= pow2char[tilenume&7];
}





void loadtile(short tilenume)
{
    uint8_t  *ptr;
    int32_t i, tileFilesize;
#ifdef OPENFPGA
    uint32_t load_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
#endif
    
    
    
    
    if ((uint32_t)tilenume >= (uint32_t)MAXTILES)
        return;
    
    tileFilesize = tiles[tilenume].dim.width * tiles[tilenume].dim.height;
    
    if (tileFilesize <= 0)
        return;
    
    i = tilefilenum[tilenume];
    if (i != artfilnum){
        if (artfil != -1)
            kclose(artfil);
        artfilnum = i;
        artfilplc = 0L;
        
        artfilename[7] = (i%10)+48;
        artfilename[6] = ((i/10)%10)+48;
        artfilename[5] = ((i/100)%10)+48;
        artfil = TCkopen4load(artfilename,0);
        
        if (artfil == -1){
            printf("Error, unable to load artfile:'%s'.\n",artfilename);
            getchar();
            exit(0);
        }
        
        faketimerhandler();
    }
    
#ifdef OPENFPGA
    tile_prepare_for_gpu_write();
#endif

    if (tiles[tilenume].data == NULL){
        tiles[tilenume].lock = 199;
        allocache(&tiles[tilenume].data,tileFilesize,(uint8_t  *) &tiles[tilenume].lock);
    }
    
    if (artfilplc != tilefileoffs[tilenume])
    {
        klseek(artfil,tilefileoffs[tilenume]-artfilplc,SEEK_CUR);
        faketimerhandler();
    }
    ptr = tiles[tilenume].data;

#ifdef OPENFPGA
    d3d_audio_pump_loading();
#endif
    kread(artfil,ptr,tileFilesize);
#ifdef OPENFPGA
    d3d_audio_pump_loading();
#endif
    faketimerhandler();
    artfilplc = tilefileoffs[tilenume]+tileFilesize;

#ifdef OPENFPGA
    tile_note_loaded_for_gpu(ptr, (uint32_t)tileFilesize);
    if (load_t0)
        d3d_gpu_perf_note_tile_load((uint32_t)(uint16_t)tilenume,
                                    (uint32_t)tileFilesize,
                                    of_time_us() - load_t0);
#endif
}

#ifdef OPENFPGA
#define TILE_BULK_READ_CHUNK (64 * 1024)
#define TILE_ASYNC_READ_ALIGN 64u
#define TILE_ASYNC_WAIT_TIMEOUT_MS 5000u

/* Experimental openfpgaOS async data-slot path for level ART preload.
 * Keep it opt-in for now: current firmware can expose a small async read
 * window and any missed completion leaves level loading waiting in this
 * blocking preload path. */
int d3d_tile_async_preload = 0;
static uint8_t *tile_async_stage[2];
static uint32_t tile_async_chunk_size;
static int tile_async_disabled;
static volatile int tile_async_done;
static volatile int tile_async_result;

typedef struct tile_async_reader_s {
    int slot_id;
    int ready_buf;
    int pending_buf;
    uint32_t file_base;
    uint32_t file_size;
    uint32_t chunk_size;
    uint32_t start[2];
    uint32_t len[2];
    uint8_t *buf[2];
} tile_async_reader_t;

static void tile_async_callback(int token, int result)
{
    (void)token;
    tile_async_result = result;
    tile_async_done = 1;
}

static void tile_async_pump_wait(void)
{
    of_file_async_poll();
    d3d_audio_pump_loading();
    faketimerhandler();
}

static int tile_async_wait_idle(void)
{
    uint32_t start_ms = of_time_ms();

    while (of_file_async_busy()) {
        tile_async_pump_wait();
        if ((uint32_t)(of_time_ms() - start_ms) >
            TILE_ASYNC_WAIT_TIMEOUT_MS) {
            tile_async_disabled = 1;
            return 0;
        }
    }

    return 1;
}

static int tile_async_stage_init(void)
{
    uint32_t max_read;
    uint32_t stage_size;
    uint32_t size;

    if (!d3d_tile_async_preload || tile_async_disabled)
        return 0;

    if (tile_async_stage[0] && tile_async_stage[1])
        return tile_async_chunk_size > 0;

    max_read = of_file_async_max_read();
    stage_size = of_file_dma_stage_size();
    if (max_read == 0 || stage_size < 8192u)
        return 0;

    size = TILE_BULK_READ_CHUNK;
    if (size > max_read)
        size = max_read;
    if (size > stage_size / 2u)
        size = stage_size / 2u;
    size &= ~(TILE_ASYNC_READ_ALIGN - 1u);
    if (size < 4096u)
        return 0;

    tile_async_stage[0] = of_file_dma_stage_alloc(size, TILE_ASYNC_READ_ALIGN);
    tile_async_stage[1] = of_file_dma_stage_alloc(size, TILE_ASYNC_READ_ALIGN);
    if (!tile_async_stage[0] || !tile_async_stage[1]) {
        tile_async_stage[0] = NULL;
        tile_async_stage[1] = NULL;
        tile_async_chunk_size = 0;
        return 0;
    }

    tile_async_chunk_size = size;
    return 1;
}

static int tile_async_contains(const tile_async_reader_t *reader,
                               int buf, uint32_t off)
{
    if (buf < 0)
        return 0;
    return off >= reader->start[buf] &&
           off < reader->start[buf] + reader->len[buf];
}

static int tile_async_wait_pending(tile_async_reader_t *reader)
{
    uint32_t start_ms;

    if (reader->pending_buf < 0)
        return 1;

    start_ms = of_time_ms();
    while (!tile_async_done) {
        if (of_file_async_poll() > 0 && !tile_async_done) {
            tile_async_result = 0;
            tile_async_done = 1;
            break;
        }
        if (tile_async_done)
            break;

        /* Some firmware paths complete through poll/busy without invoking
         * the optional callback.  Treat idle as successful completion so
         * level loading cannot spin forever waiting for an IRQ callback. */
        if (!of_file_async_busy()) {
            tile_async_result = 0;
            tile_async_done = 1;
            break;
        }

        d3d_audio_pump_loading();
        faketimerhandler();
        if ((uint32_t)(of_time_ms() - start_ms) >
            TILE_ASYNC_WAIT_TIMEOUT_MS) {
            tile_async_disabled = 1;
            reader->pending_buf = -1;
            return 0;
        }
    }

    if (tile_async_result != 0) {
        tile_async_disabled = 1;
        reader->pending_buf = -1;
        return 0;
    }

    /* The async staging buffers are allocated from CRAM0, which is uncached
     * on Pocket.  async_complete() has already returned the CRAM0 mux to the
     * CPU side, so a CPU fence is enough before copying from the stage buffer.
     */
    __asm__ volatile("fence" ::: "memory");

    reader->ready_buf = reader->pending_buf;
    reader->pending_buf = -1;
    return 1;
}

static int tile_async_start_read(tile_async_reader_t *reader,
                                 int buf, uint32_t start)
{
    uint32_t len;
    int token;

    if (start >= reader->file_size)
        return 0;

    if (!tile_async_wait_idle())
        return 0;

    len = reader->file_size - start;
    if (len > reader->chunk_size)
        len = reader->chunk_size;

    tile_async_done = 0;
    tile_async_result = -1;
    token = of_file_read_async(reader->slot_id,
                               reader->file_base + start,
                               reader->buf[buf], len,
                               tile_async_callback);
    if (token < 0) {
        tile_async_done = 1;
        return 0;
    }

    reader->start[buf] = start;
    reader->len[buf] = len;
    reader->pending_buf = buf;
    return 1;
}

static void tile_async_prefetch_next(tile_async_reader_t *reader)
{
    uint32_t next;
    int buf;

    if (reader->ready_buf < 0 || reader->pending_buf >= 0)
        return;

    next = reader->start[reader->ready_buf] + reader->len[reader->ready_buf];
    if (next >= reader->file_size)
        return;

    buf = reader->ready_buf ^ 1;
    (void)tile_async_start_read(reader, buf, next);
}

static int tile_async_ensure(tile_async_reader_t *reader, uint32_t off)
{
    uint32_t start;
    int buf;

    if (tile_async_contains(reader, reader->ready_buf, off))
        return 1;

    if (tile_async_contains(reader, reader->pending_buf, off)) {
        if (!tile_async_wait_pending(reader))
            return 0;
        tile_async_prefetch_next(reader);
        return tile_async_contains(reader, reader->ready_buf, off);
    }

    if (reader->pending_buf >= 0 && !tile_async_wait_pending(reader))
        return 0;

    if (tile_async_contains(reader, reader->ready_buf, off))
        return 1;

    start = (off / reader->chunk_size) * reader->chunk_size;
    buf = (reader->ready_buf >= 0) ? (reader->ready_buf ^ 1) : 0;
    if (!tile_async_start_read(reader, buf, start))
        return 0;
    if (!tile_async_wait_pending(reader))
        return 0;

    tile_async_prefetch_next(reader);
    return tile_async_contains(reader, reader->ready_buf, off);
}

static void tile_async_reader_end(tile_async_reader_t *reader)
{
    (void)tile_async_wait_pending(reader);
}

static int tile_async_reader_begin(tile_async_reader_t *reader,
                                   int slot_id,
                                   uint32_t file_base,
                                   uint32_t file_size)
{
    if (slot_id < 0 || file_size == 0)
        return 0;
    if (!tile_async_stage_init())
        return 0;

    memset(reader, 0, sizeof(*reader));
    reader->slot_id = slot_id;
    reader->ready_buf = -1;
    reader->pending_buf = -1;
    reader->file_base = file_base;
    reader->file_size = file_size;
    reader->chunk_size = tile_async_chunk_size;
    reader->buf[0] = tile_async_stage[0];
    reader->buf[1] = tile_async_stage[1];
    return 1;
}

static int TILE_PreloadMarkedSlow(void)
{
    int32_t i;
    int loaded = 0;

    tile_begin_bulk_preload();
    for (i = 0; i < MAXTILES; i++) {
        if (!tile_marked_for_load(i))
            continue;

        loadtile((short)i);
        loaded++;
        if ((loaded & 7) == 0)
            faketimerhandler();
    }
    tile_end_bulk_preload();

    return loaded;
}

int TILE_PreloadMarked(void)
{
    uint8_t *chunk;
    int32_t filenum, i;
    int loaded = 0;

    chunk = (uint8_t *)malloc(TILE_BULK_READ_CHUNK);
    if (chunk == NULL)
        return TILE_PreloadMarkedSlow();

    if (artfil != -1) {
        kclose(artfil);
        artfil = -1;
        artfilnum = -1;
        artfilplc = 0L;
    }

    tile_begin_bulk_preload();

    for (filenum = 0; filenum < numtilefiles; filenum++) {
        int has_needed_tile = 0;
        int fil;
        char filename[20];
        int32_t chunk_start = -1;
        int32_t chunk_len = 0;
        int use_async = 0;
        tile_async_reader_t async_reader;

        d3d_audio_pump_loading();

        for (i = 0; i < MAXTILES; i++) {
            if (tilefilenum[i] == filenum && tile_marked_for_load(i)) {
                has_needed_tile = 1;
                break;
            }
        }

        if (!has_needed_tile)
            continue;

        tile_art_filename(filename, filenum);
        memset(&async_reader, 0, sizeof(async_reader));
        async_reader.ready_buf = -1;
        async_reader.pending_buf = -1;
        {
            int32_t grpID, fileIndex, grpOffset, grpSize;
            if (kgrp_find_file(filename, &grpID, &fileIndex,
                               &grpOffset, &grpSize) == 0) {
                (void)fileIndex;
                use_async = tile_async_reader_begin(
                    &async_reader,
                    kgrp_slot_id(grpID),
                    (uint32_t)grpOffset,
                    (uint32_t)grpSize);
            }
        }

        fil = -1;
        if (!use_async) {
            fil = TCkopen4load(filename, 0);
            if (fil == -1) {
                printf("Error, unable to load artfile:'%s'.\n", filename);
                getchar();
                exit(0);
            }
        }

        for (i = 0; i < MAXTILES; i++) {
            int32_t remaining, tile_off;
            uint8_t *dst;
            int32_t tileFilesize;

            if (tilefilenum[i] != filenum || !tile_marked_for_load(i))
                continue;

            tileFilesize = tiles[i].dim.width * tiles[i].dim.height;
            tiles[i].lock = 199;
            allocache(&tiles[i].data, tileFilesize,
                      (uint8_t *)&tiles[i].lock);

            dst = tiles[i].data;
            tile_off = tilefileoffs[i];
            remaining = tileFilesize;

            while (remaining > 0) {
                const uint8_t *read_chunk = chunk;
                int32_t chunk_end = chunk_start + chunk_len;
                int32_t avail, copy_len;

                if (chunk_len <= 0 || tile_off < chunk_start ||
                    tile_off >= chunk_end) {
                    if (use_async) {
                        if (!tile_async_ensure(&async_reader,
                                               (uint32_t)tile_off)) {
                            use_async = 0;
                            tile_async_reader_end(&async_reader);
                            fil = TCkopen4load(filename, 0);
                            if (fil == -1) {
                                printf("Error, unable to load artfile:'%s'.\n",
                                       filename);
                                getchar();
                                exit(0);
                            }
                            chunk_start = -1;
                            chunk_len = 0;
                            continue;
                        }
                        chunk_start =
                            (int32_t)async_reader.start[async_reader.ready_buf];
                        chunk_len =
                            (int32_t)async_reader.len[async_reader.ready_buf];
                        read_chunk = async_reader.buf[async_reader.ready_buf];
                    } else {
                        klseek(fil, tile_off, SEEK_SET);
                        chunk_start = tile_off;
                        d3d_audio_pump_loading();
                        chunk_len = kread(fil, chunk, TILE_BULK_READ_CHUNK);
                        d3d_audio_pump_loading();
                        faketimerhandler();
                        if (chunk_len <= 0) {
                            printf("Error reading artfile:'%s'.\n", filename);
                            getchar();
                            exit(0);
                        }
                    }
                    chunk_end = chunk_start + chunk_len;
                }
                if (use_async)
                    read_chunk = async_reader.buf[async_reader.ready_buf];

                avail = chunk_end - tile_off;
                copy_len = (remaining < avail) ? remaining : avail;
                memcpy(dst, read_chunk + (tile_off - chunk_start), copy_len);
                dst += copy_len;
                tile_off += copy_len;
                remaining -= copy_len;
            }

            tile_note_loaded_for_gpu(tiles[i].data, (uint32_t)tileFilesize);
            loaded++;
            if ((loaded & 3) == 0)
                d3d_audio_pump_loading();
            if ((loaded & 7) == 0)
                faketimerhandler();
        }

        if (use_async)
            tile_async_reader_end(&async_reader);
        else
            kclose(fil);
    }

    tile_end_bulk_preload();
    free(chunk);

    return loaded;
}
#endif



uint8_t* allocatepermanenttile(short tilenume, int32_t width, int32_t height)
{
    int32_t j;
    uint32_t tileDataSize;
    
    //Check dimensions are correct.
    if ((width <= 0) || (height <= 0) || ((uint32_t)tilenume >= (uint32_t)MAXTILES))
        return(0);
    
    tileDataSize = width * height;
    
    tiles[tilenume].lock = 255;
    allocache(&tiles[tilenume].data,tileDataSize,(uint8_t  *) &tiles[tilenume].lock);
    
    tiles[tilenume].dim.width = width;
    tiles[tilenume].dim.height = height;
    tiles[tilenume].animFlags = 0;
    
    j = 15;
    while ((j > 1) && (pow2long[j] > width))
        j--;
    picsiz[tilenume] = ((uint8_t )j);
    
    j = 15;
    while ((j > 1) && (pow2long[j] > height))
        j--;
    picsiz[tilenume] += ((uint8_t )(j<<4));
    
    return(tiles[tilenume].data);
}



int loadpics(char  *filename, char * gamedir)

{
    int32_t offscount, localtilestart, localtileend, dasiz;
    short fil, i, j, k;
    
    
    strcpy(artfilename,filename);
    
    for(i=0; i<MAXTILES; i++)
    {
        tiles[i].dim.width = 0;
        tiles[i].dim.height = 0;
        tiles[i].animFlags = 0L;
    }
    
    artsize = 0L;
    
    numtilefiles = 0;
    do
    {
        k = numtilefiles;
        
        artfilename[7] = (k%10)+48;
        artfilename[6] = ((k/10)%10)+48;
        artfilename[5] = ((k/100)%10)+48;
        
        
        
        if ((fil = TCkopen4load(artfilename,0)) != -1)
        {
            kread32(fil,&artversion);
            if (artversion != 1) return(-1);

            kread32(fil,&numTiles);
            kread32(fil,&localtilestart);
            kread32(fil,&localtileend);

            /* ART headers store little-endian scalar tables. */
            for (i = localtilestart; i <= localtileend; i++)
                kread16(fil,&tiles[i].dim.width);

            for (i = localtilestart; i <= localtileend; i++)
                kread16(fil,&tiles[i].dim.height);

            for (i = localtilestart; i <= localtileend; i++)
                kread32(fil,&tiles[i].animFlags);

            offscount = 4+4+4+4+((localtileend-localtilestart+1)<<3);
            for(i=localtilestart; i<=localtileend; i++)
            {
                tilefilenum[i] = k;
                tilefileoffs[i] = offscount;
                dasiz = tiles[i].dim.width*tiles[i].dim.height;
                offscount += dasiz;
                artsize += ((dasiz+15)&0xfffffff0);
            }
            kclose(fil);

            numtilefiles++;

#ifdef OPENFPGA
            /* Update progress bar: art loading spans 65%→90% */
            {
                extern void of_progress(int);
                of_progress(65 + (numtilefiles * 25) / 20);
            }
#endif

        }
    }
    while (k != numtilefiles);
    
#ifndef OPENFPGA
    printf("Art files loaded\n");
#endif
    
    clearbuf(gotpic,(MAXTILES+31)>>5,0L);
    
    /* Allocate the BUILD art cache. */

#ifdef OPENFPGA
    /* openfpgaOS has 64MB SDRAM; use a generous cache to avoid eviction. */
    cachesize = max(artsize, 1048576);
    if (cachesize > 32 * 1024 * 1024)
        cachesize = 32 * 1024 * 1024;
#else
    cachesize = max(artsize, 1048576);
#endif
    while ((pic = (uint8_t *)malloc(cachesize)) == NULL)
    {
        cachesize -= 65536L;
        if (cachesize < 65536) return(-1);
    }
    initcache(pic,cachesize);
    
    for(i=0; i<MAXTILES; i++)
    {
        j = 15;
        while ((j > 1) && (pow2long[j] > tiles[i].dim.width)) 
            j--;
        
        picsiz[i] = ((uint8_t )j);
        j = 15;
        
        while ((j > 1) && (pow2long[j] > tiles[i].dim.height)) 
            j--;
        
        picsiz[i] += ((uint8_t )(j<<4));
    }
    
    artfil = -1;
    artfilnum = -1;
    artfilplc = 0L;
    
    return(0);
}


void TILE_MakeAvailable(short picID){
    if (tiles[picID].data == NULL) 
        loadtile(picID);

}

void copytilepiece(int32_t tilenume1, int32_t sx1, int32_t sy1, int32_t xsiz, int32_t ysiz,
                   int32_t tilenume2, int32_t sx2, int32_t sy2)
{
    uint8_t  *ptr1, *ptr2, dat;
    int32_t xsiz1, ysiz1, xsiz2, ysiz2, i, j, x1, y1, x2, y2;
    
    xsiz1 = tiles[tilenume1].dim.width;
    ysiz1 = tiles[tilenume1].dim.height;
    
    xsiz2 = tiles[tilenume2].dim.width;
    ysiz2 = tiles[tilenume2].dim.height;
    
    
    if ((xsiz1 > 0) && (ysiz1 > 0) && (xsiz2 > 0) && (ysiz2 > 0))
    {
        TILE_MakeAvailable(tilenume1);
        TILE_MakeAvailable(tilenume2);

#ifdef OPENFPGA
        tile_prepare_for_gpu_write();
#endif
        
        x1 = sx1;
        for(i=0; i<xsiz; i++)
        {
            y1 = sy1;
            for(j=0; j<ysiz; j++)
            {
                x2 = sx2+i;
                y2 = sy2+j;
                if ((x2 >= 0) && (y2 >= 0) && (x2 < xsiz2) && (y2 < ysiz2))
                {
                    ptr1 = tiles[tilenume1].data + x1*ysiz1 + y1;
                    ptr2 = tiles[tilenume2].data + x2*ysiz2 + y2;
                    dat = *ptr1;
                    
                    
                    if (dat != 255)
                        *ptr2 = *ptr1;
                }
                
                y1++;
                if (y1 >= ysiz1) y1 = 0;
            }
            x1++;
            if (x1 >= xsiz1) x1 = 0;
        }

#ifdef OPENFPGA
        tile_sync_for_gpu(tiles[tilenume2].data, (uint32_t)(xsiz2 * ysiz2));
#endif
    }
}



/* Return the animation offset to add to a tile number. */
int animateoffs(int16_t tilenum)
{
    int32_t i, k, offs;
    
    offs = 0;
    
    i = (totalclocklock>>((tiles[tilenum].animFlags>>24)&15));
    
    if ((tiles[tilenum].animFlags&63) > 0){
        switch(tiles[tilenum].animFlags&192)
        {
            case 64:
                k = (i%((tiles[tilenum].animFlags&63)<<1));
                if (k < (tiles[tilenum].animFlags&63))
                    offs = k;
                else
                    offs = (((tiles[tilenum].animFlags&63)<<1)-k);
                break;
            case 128:
                offs = (i%((tiles[tilenum].animFlags&63)+1));
                break;
            case 192:
                offs = -(i%((tiles[tilenum].animFlags&63)+1));
        }
    }
    
    return(offs);
}
