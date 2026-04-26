/*
 * d3d_gpu.c — Duke3D ↔ openfpgaOS GPU rasteriser glue (single TU).
 *
 * of_gpu.h declares static mutable ring-buffer state (_gpu_wrptr,
 * _gpu_fence_next, etc.), so it MUST be included from exactly one
 * translation unit per program.  All GPU calls Duke3D makes route
 * through this file.
 */

#include "d3d_gpu.h"

#include "of.h"
#include "of_caps.h"
#include "of_cache.h"
#include "of_gpu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

int d3d_gpu_present  = 0;
int d3d_gpu_use_spans = 0;   /* Master switch.  Set to 0 BEFORE the
                              * first frame to disable the GPU
                              * entirely: d3d_gpu_init() bails so
                              * d3d_gpu_present stays 0, every helper
                              * (set_fb, drain, flush, pre_cpu_fb_access,
                              * clear_rect_fb, all the span hooks)
                              * short-circuits, and BUILD's CPU loops
                              * run unmodified.  Used for the GPU-on
                              * vs GPU-off A/B perf test. */

/* Cached framebuffer base + stride so the per-span helpers don't have
 * to chase the BUILD globals every call. */
static uint8_t *fb_base;
static int      fb_stride;   /* pixels per scanline */

/* Multi-palookup state.  The fabric now holds up to 16 palookups in
 * SDRAM (one per slot), reachable via gpu_tex_cache port B; CPU
 * switches the active slot with of_gpu_set_colormap_id(slot).  We
 * upload lazily on first encounter of a palookup row that doesn't
 * fall in any already-loaded slot, so most maps that use 4–8 distinct
 * pals never touch slot 8+.  Slot 0 is reserved for palookup[0] (the
 * pal0 default that the GPU resets to). */
#define D3D_GPU_NUM_SHADES   32
#define D3D_GPU_PAL_BYTES    (D3D_GPU_NUM_SHADES * 256)
#define D3D_GPU_PAL_SLOTS    OF_GPU_PALOOKUP_SLOTS  /* fabric advertises 16 */

static const uint8_t *gpu_pal_base_of_slot[D3D_GPU_PAL_SLOTS];
static int            gpu_pal_next_slot;     /* number of slots in use */
static int            gpu_current_slot;      /* GPU's active slot mirror, -1 = unset */

/* Deferred GPU-tex-cache invalidate is now unconditional once per
 * frame (see d3d_gpu_set_fb).  The dirty flag stayed in the API as
 * a hint hook for callers that want to forcibly mark mid-frame, but
 * d3d_gpu_set_fb invalidates regardless — covering every BUILD path
 * that mutates tile bytes (loadtile, copytilepiece, allocache shuffles,
 * animated-tile data updates, runtime-built HUD/weapon sprites)
 * without having to chase each one individually.  Cache walk is
 * ~10 µs at 100 MHz; the GPU tex cache only helps within a frame, so
 * across-frame invalidate costs us nothing in steady state. */
static volatile int gpu_tex_dirty;

void d3d_gpu_init(void)
{
    /* A/B test escape hatch — leave d3d_gpu_present at 0 so every
     * downstream helper short-circuits and BUILD's CPU loops run. */
    if (!d3d_gpu_use_spans) {
        printf("[d3d_gpu] DISABLED via d3d_gpu_use_spans=0 — SW renderer\n");
        return;
    }

    /* Skip cleanly on PC builds and on FPGA targets that don't ship
     * a GPU window.  of_get_caps()->gpu_base == 0 is the documented
     * "no GPU" indicator. */
    const struct of_capabilities *caps = of_get_caps();
    if (!caps || caps->gpu_base == 0) {
        printf("[d3d_gpu] no GPU advertised by caps — SW renderer only\n");
        return;
    }

    of_gpu_init();
    d3d_gpu_present = 1;
    printf("[d3d_gpu] GPU init ok (base=0x%08x)\n", (unsigned)caps->gpu_base);
}

void d3d_gpu_set_fb(uint8_t *fb_pixels, int stride_pixels)
{
    /* Cache fb_base/fb_stride unconditionally — d3d_gpu_clear_rect_fb's
     * CPU memset fallback uses fb_stride for per-row advance even when
     * the GPU is disabled (use_spans=0 / d3d_gpu_present=0). */
    fb_base   = fb_pixels;
    fb_stride = stride_pixels;
    if (!d3d_gpu_present) return;

    /* Unconditional GPU-tex-cache invalidate at every frame boundary.
     * We're between frames here: the previous frame's d3d_gpu_flush()
     * ran of_gpu_finish() so the GPU is idle, and we haven't submitted
     * any new draws for the upcoming frame yet — perfect window for
     * GPU_TEX_FLUSH to walk-clear valid_mem without colliding with
     * an in-flight FB write.  Doing this every frame (instead of only
     * when the dirty flag is set) catches every BUILD code path that
     * mutates tile bytes — loadtile, copytilepiece, allocache shuffles,
     * animated-tile data updates, runtime HUD/weapon sprite builds —
     * without us having to find and hook each one. */
    GPU_TEX_FLUSH = 1;
    gpu_tex_dirty = 0;

    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb_pixels,
                           (uint16_t)stride_pixels);
}

/* Upload the active palookup as the GPU colormap.  Duke3D's palookup
 * is 32 shade rows × 256 entries = 8 KB; the GPU's colormap RAM holds
 * up to 64 rows × 256 entries.  We upload all NUM_SHADES rows starting
 * at row 0 so SPAN_COLORMAP's `light` field directly indexes shade. */
void d3d_gpu_upload_palookup(const uint8_t *palookup_table, int num_shades)
{
    if (!d3d_gpu_present || !palookup_table) return;
    if (num_shades <= 0)  num_shades = 32;
    if (num_shades > 64)  num_shades = 64;
    of_gpu_palookup_upload(0, palookup_table,
                           (uint32_t)num_shades * 256u);
}

void d3d_gpu_upload_transluc(const uint8_t *table, uint32_t size)
{
    if (!d3d_gpu_present || !table) return;
    of_gpu_translucency_upload(table, size);
}

void d3d_gpu_clear_rect_fb(uint8_t *dest, uint16_t w, uint16_t h, uint8_t color)
{
    if (!dest) return;
    if (w == 0 || h == 0) return;
    if (!d3d_gpu_present || !d3d_gpu_use_spans) {
        /* GPU off — CPU memset path so A/B tests against the GPU
         * implementation work, and the no-GPU bitstream still draws.
         * fb_stride supplies the per-row advance (== bytesperline). */
        for (int y = 0; y < (int)h; y++)
            memset(dest + y * fb_stride, color, w);
        return;
    }
    of_gpu_clear_rect((uint32_t)(uintptr_t)dest, w, h, color);
}

void d3d_gpu_pre_cpu_fb_access(void)
{
    if (!d3d_gpu_present) return;
    /* Drain GPU writes to SDRAM so subsequent CPU reads see current
     * pixels (without this, a CPU read of a region the GPU just
     * wrote could return stale SDRAM). */
    of_gpu_finish();
    /* Invalidate L1 — without this, a CPU read of an FB byte the
     * fabric just wrote could hit a STALE clean line in L1 from the
     * previous frame.  of_cache_flush is "write-back + invalidate"
     * and is the simplest correct primitive here.  The post-mirror
     * write-back is folded into of_video_flip()'s of_cache_clean_range
     * call (covers the FB area), so we don't need a separate flush
     * after the mirror writes. */
    of_cache_flush();
}

/* Drain pending GPU work so the next page-flip sees finished pixels.
 *
 * No CPU cache flush here — of_video_flip() does of_cache_clean_range()
 * on the active FB region, which covers every CPU FB write that could
 * still be in L1 (the only path that exists is completemirror, and its
 * writes go to the same FB region of_video_flip cleans).  The principle
 * (project_gpu_owns_framebuffer.md) plus the redundant flush in flip()
 * make our own of_cache_flush dead code.
 *
 * Status of CPU FB writers:
 *   - 3D renderer (vline/hline/sprite/translucent/rotated): all GPU.
 *   - Per-frame clears (letterbox bars, clearview, clearallviews,
 *     clear2dscreen, fillscreen16, setgamemode HW-buffer wipe): GPU.
 *   - 2D primitives (drawpixel, drawline16, plotpixel): GPU.
 *   - printext256 font glyphs: GPU.
 *   - completemirror (mirror-wall reverse-blit): CPU; calls
 *     d3d_gpu_pre_cpu_fb_access() for the read-side coherency, then
 *     relies on of_video_flip's cache_clean for the write-side. */
void d3d_gpu_flush(void)
{
    if (!d3d_gpu_present) return;
    of_gpu_finish();
}

void d3d_gpu_tex_invalidate(void)
{
    if (!d3d_gpu_present) return;
    GPU_TEX_FLUSH = 1;
}

void d3d_gpu_mark_tex_dirty(void)
{
    if (!d3d_gpu_present) return;
    gpu_tex_dirty = 1;
}

void d3d_gpu_drain(void)
{
    if (!d3d_gpu_present) return;
    of_gpu_finish();
}

/* ----------------------------------------------------------------------
 * Phase 2 (gated): one-column textured wall span.
 *
 * Scalar reference is draw.c::vlineasm1:
 *
 *   while (numPixels--) {
 *       *dest = palookupoffse[texture[vplce >> v_shift]];
 *       vplce += vince;
 *       dest  += bytesperline;
 *   }
 *
 *   - texture[]            = 1-D column data (already u-indexed)
 *   - vplce / vince        = 32-bit fixed point; texel index =
 *                            (vplce >> v_shift) & MASK
 *   - palookupoffse        = pointer into palookup[pal][shade*256]
 *
 * GPU equivalent uses a SPAN_COLUMN with tex_width=1 so address-gen is
 * `tex_addr + (t >> 16)`.  We renormalise vplce / vince from "texel-
 * units shifted by v_shift" to the GPU's 16.16 by:
 *
 *   t     = vplce << (16 - v_shift)        if v_shift <= 16
 *           vplce >> (v_shift - 16)        otherwise
 *   tstep = same transform on vince
 *
 * Caller passes the shade index (0..NUMSHADES-1) directly so we don't
 * have to back-derive it from the palookupoffse pointer here.
 * -------------------------------------------------------------------- */
/* Internal: convert (vplce, vince) from BUILD's "fractional shifted by
 * v_shift" representation into GPU 16.16.  Pulled out so vline + mvline
 * + vline4 share one math path. */
static inline void to_16_16(uint32_t vplce, uint32_t vince, uint8_t v_shift,
                            int32_t *t_out, int32_t *tstep_out)
{
    if (v_shift <= 16) {
        unsigned s = (unsigned)(16u - (unsigned)v_shift);
        *t_out     = (int32_t)(vplce << s);
        *tstep_out = (int32_t)(vince << s);
    } else {
        unsigned s = (unsigned)((unsigned)v_shift - 16u);
        *t_out     = (int32_t)(vplce >> s);
        *tstep_out = (int32_t)(vince >> s);
    }
}

/* BUILD's palookup table — declared in build.h as
 *   EXTERN uint8_t *palookup[MAXPALOOKUPS];
 * We may now consume any of palookup[0..255]; up to 16 distinct
 * palookups can live in GPU slots simultaneously. */
extern uint8_t *palookup[];

/* Lazy slot-0 upload + pointer-staleness check.  loadpalette() can
 * reallocate palookup[0] across game-init paths; if the cached base
 * differs from the current palookup[0], invalidate the entire slot
 * cache so all uploads happen fresh. */
static inline void ensure_pal0_uploaded(void)
{
    if (!palookup[0]) return;
    if (gpu_pal_base_of_slot[0] == palookup[0]) return;

    /* Stale or first time: reset cache + upload pal0 to slot 0. */
    for (int i = 0; i < D3D_GPU_PAL_SLOTS; i++)
        gpu_pal_base_of_slot[i] = NULL;
    of_gpu_palookup_upload(0, palookup[0], D3D_GPU_PAL_BYTES);
    gpu_pal_base_of_slot[0] = palookup[0];
    gpu_pal_next_slot = 1;
    /* GPU resets the active slot to 0; software mirror starts there. */
    gpu_current_slot = 0;
}

/* Backwards-compat: callers (and the comments above) historically
 * read this as "is `palookupoffse` in the GPU's loaded colormap, and
 * if so, what shade index?".  New behaviour: scan the loaded slots
 * (hot path: the slot used by the previous call), upload a fresh
 * slot lazily on first encounter of an unseen palookup, switch the
 * GPU's active slot via of_gpu_set_colormap_id when it changes, and
 * return the shade index.  Returns -1 only if the row is in NONE of
 * the BUILD palookups (unusual) or the slot table is full (can't fit
 * a 17th distinct palookup — caller falls back to CPU). */
int d3d_gpu_shade_for(const uint8_t *palookupoffse)
{
    if (!palookupoffse) return -1;
    ensure_pal0_uploaded();
    if (!gpu_pal_base_of_slot[0]) return -1;   /* palookup[0] not ready */

    /* Hot path: same slot as previous call? */
    if (gpu_current_slot >= 0) {
        const uint8_t *base = gpu_pal_base_of_slot[gpu_current_slot];
        if (base) {
            ptrdiff_t off = palookupoffse - base;
            if ((uintptr_t)off < D3D_GPU_PAL_BYTES)
                return (int)(off >> 8);
        }
    }

    /* Warm path: scan slots already uploaded — palookup pointers are
     * pairwise disjoint so the first match is unique. */
    for (int s = 0; s < gpu_pal_next_slot; s++) {
        const uint8_t *base = gpu_pal_base_of_slot[s];
        if (!base) continue;
        ptrdiff_t off = palookupoffse - base;
        if ((uintptr_t)off < D3D_GPU_PAL_BYTES) {
            if (gpu_current_slot != s) {
                of_gpu_set_colormap_id((uint8_t)s);
                gpu_current_slot = s;
            }
            return (int)(off >> 8);
        }
    }

    /* Cold path: locate the BUILD palookup[i] containing this row,
     * upload it to a fresh slot, point the GPU at it. */
    for (int pal_id = 0; pal_id < 256; pal_id++) {
        const uint8_t *p = palookup[pal_id];
        if (!p) continue;
        ptrdiff_t off = palookupoffse - p;
        if ((uintptr_t)off >= D3D_GPU_PAL_BYTES) continue;

        if (gpu_pal_next_slot >= D3D_GPU_PAL_SLOTS)
            return -1;   /* slot table full — caller falls back to CPU */

        int s = gpu_pal_next_slot++;
        of_gpu_palookup_upload((uint8_t)s, p, D3D_GPU_PAL_BYTES);
        gpu_pal_base_of_slot[s] = p;
        of_gpu_set_colormap_id((uint8_t)s);
        gpu_current_slot = s;
        return (int)(off >> 8);
    }
    return -1;   /* not in any palookup at all (unusual) */
}

/* Internal: build + emit a single COLUMN span. */
static inline void emit_column_span(uint8_t *dest, int num_pixels, int shade,
                                    int32_t t, int32_t tstep,
                                    const uint8_t *texture, uint8_t flags)
{
    of_gpu_span_t span = {
        .fb_addr   = (uint32_t)(uintptr_t)dest,
        .tex_addr  = (uint32_t)(uintptr_t)texture,
        .s         = 0,
        .t         = t,
        .sstep     = 0,
        .tstep     = tstep,
        .count     = (uint16_t)num_pixels,
        .light     = (uint8_t)(shade & 0x3F),
        .flags     = flags,
        .fb_stride = (int16_t)fb_stride,
        .tex_width = 1,
    };
    of_gpu_draw_span(&span);
}

void d3d_gpu_vline(uint8_t *dest, int num_pixels, int shade,
                   uint32_t vplce, uint32_t vince, uint8_t v_shift,
                   const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);

    /* fb_stride alone selects column vs row walk: emit_column_span
     * sets fb_stride = bytesperline so the rasteriser steps one row
     * per fragment automatically. */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture,
                     OF_GPU_SPAN_COLORMAP);
}

void d3d_gpu_mvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);

    /* SPAN_SKIP_ZERO drops texels matching the GPU's transparent index
     * (0xFF on this fabric — see gpudemo's sprite path).  BUILD's
     * TRANSPARENT_COLOR is also 0xFF, so this is a 1:1 map: any texel
     * the SW path skipped via `if (temp != TRANSPARENT_COLOR)` the GPU
     * skips at the fragment stage. */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture,
                     OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO);
}

void d3d_gpu_vline4(uint8_t *fb_at_y0, int num_pixels,
                    const int shade[4],
                    const uint32_t vplce[4],
                    const uint32_t vince[4],
                    uint8_t v_shift,
                    const uint8_t *const texture[4])
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();

    /* Four adjacent column spans, one per quad column.  The SW path
     * packs 4 texels into a uint32 store per scanline; here we just
     * submit 4 separate column draws and let the GPU schedule them in
     * parallel (the rasteriser is span-pipelined).  COLUMN flag is
     * vestigial — fb_stride = bytesperline drives the column walk. */
    for (int c = 0; c < 4; c++) {
        int32_t t, tstep;
        to_16_16(vplce[c], vince[c], v_shift, &t, &tstep);
        emit_column_span(fb_at_y0 + c, num_pixels, shade[c], t, tstep,
                         texture[c],
                         OF_GPU_SPAN_COLORMAP);
    }
}

void d3d_gpu_mvline4(uint8_t *fb_at_y0, int num_pixels,
                     const int shade[4],
                     const uint32_t vplce[4],
                     const uint32_t vince[4],
                     uint8_t v_shift,
                     const uint8_t *const texture[4])
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();

    /* Same as vline4 but with SKIP_ZERO so per-column transparency
     * drops 0xFF texels.  SW path's mixed-transparency byte-write
     * branch becomes a single-flag toggle on the GPU. */
    for (int c = 0; c < 4; c++) {
        int32_t t, tstep;
        to_16_16(vplce[c], vince[c], v_shift, &t, &tstep);
        emit_column_span(fb_at_y0 + c, num_pixels, shade[c], t, tstep,
                         texture[c],
                         OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO);
    }
}

/* Convert a BUILD fixed-point value `v` whose top `int_bits` are the
 * integer part (i.e. `frac_bits = 32 - int_bits`) into 16.16 fixed.
 * Equivalent to `v * 65536 / (1 << int_bits)` without overflow risk:
 *   - frac_bits == 16: identity
 *   - frac_bits  > 16: shift right by (frac_bits - 16)
 *   - frac_bits  < 16: shift left  by (16 - frac_bits) — int part
 *                      ends up in the high half, low bits are zero
 *                      (tex coord precision capped at 16 frac bits). */
static inline int32_t to_16_16_var(uint32_t v, int int_bits)
{
    int frac_bits = 32 - int_bits;
    if (frac_bits >= 16) return (int32_t)(v >> (frac_bits - 16));
    return (int32_t)(v << (16 - frac_bits));
}

/* ----------------------------------------------------------------------
 * Try-helpers — single-entry-single-exit, normal ABI.
 *
 * The motivation for these is a code-gen bug: when the gate body lives
 * inline in vlineasm4 (an OF_FASTTEXT function), GCC's lazy-save
 * optimiser saves s8/s9 only on the SW path but the merged epilogue
 * restores them unconditionally — so the GPU success path returns with
 * garbage in callee-saved registers and the caller crashes.  Moving the
 * gate body out into its own function gives it its own clean
 * prologue/epilogue and isolates the SW path's register usage from the
 * GPU dispatch.
 * -------------------------------------------------------------------- */

int d3d_gpu_try_vline1(uint8_t *dest, int num_pixels,
                       const uint8_t *palookupoffse,
                       int32_t vplce, int32_t vince,
                       uint8_t v_shift, const uint8_t *texture,
                       int32_t *vplce_out)
{
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    int shade = d3d_gpu_shade_for(palookupoffse);
    if (shade < 0) return 0;

    /* num_pixels here matches the SW path's `numPixels++; while(numPixels)`
     * — caller passes (numPixels + 1) explicitly. */
    d3d_gpu_vline(dest, num_pixels, shade,
                  (uint32_t)vplce, (uint32_t)vince, v_shift, texture);
    if (vplce_out) *vplce_out = vplce + vince * num_pixels;
    return 1;
}

int d3d_gpu_try_mvline1(uint8_t *dest, int num_pixels,
                        const uint8_t *palookupoffse,
                        int32_t vplce, int32_t vince,
                        uint8_t v_shift, const uint8_t *texture,
                        int32_t *vplce_out)
{
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    int shade = d3d_gpu_shade_for(palookupoffse);
    if (shade < 0) return 0;

    d3d_gpu_mvline(dest, num_pixels, shade,
                   (uint32_t)vplce, (uint32_t)vince, v_shift, texture);
    if (vplce_out) *vplce_out = vplce + vince * num_pixels;
    return 1;
}

/* vlineasm4 / mvlineasm4 read their per-column state from BUILD
 * globals (vplce[4], vince[4], bufplce[4], palookupoffse[4]).  We
 * import them via extern decls so the gate body matches the SW
 * version exactly — no parameter explosion at the call site. */
extern int32_t  vplce[4], vince[4];
extern intptr_t bufplce[4];
extern uint8_t *palookupoffse[4];

static int try_vline4_common(uint8_t *framebuffer, int num_pixels,
                             uint8_t v_shift, int masked)
{
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;

    int    shades[4];
    const uint8_t *tex[4];
    for (int c = 0; c < 4; c++) {
        shades[c] = d3d_gpu_shade_for(palookupoffse[c]);
        if (shades[c] < 0) return 0;
        tex[c] = (const uint8_t *)bufplce[c];
    }

    if (masked) {
        d3d_gpu_mvline4(framebuffer, num_pixels, shades,
                        (const uint32_t *)vplce,
                        (const uint32_t *)vince, v_shift, tex);
    } else {
        d3d_gpu_vline4(framebuffer, num_pixels, shades,
                       (const uint32_t *)vplce,
                       (const uint32_t *)vince, v_shift, tex);
    }
    for (int c = 0; c < 4; c++)
        vplce[c] += vince[c] * num_pixels;
    return 1;
}

int d3d_gpu_try_vline4(uint8_t *framebuffer, int num_pixels,
                       uint8_t v_shift)
{
    return try_vline4_common(framebuffer, num_pixels, v_shift, 0);
}

int d3d_gpu_try_mvline4(uint8_t *framebuffer, int num_pixels,
                        uint8_t v_shift)
{
    return try_vline4_common(framebuffer, num_pixels, v_shift, 1);
}

int d3d_gpu_try_hline(uint8_t *dest_right, int num_pixels, int shade_x256,
                      uint32_t i4, uint32_t i5,
                      uint32_t asm1, uint32_t asm2,
                      uint8_t width_bits, uint8_t shifter,
                      const uint8_t *texture)
{
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    /* hlineasm4 reads via globalpalwritten[shade|s], so the floor's
     * pal must match the one in the GPU's colormap RAM (palookup[0]). */
    extern uint8_t *globalpalwritten;
    if (d3d_gpu_shade_for(globalpalwritten) < 0) return 0;

    d3d_gpu_hline(dest_right, num_pixels, shade_x256, i4, i5,
                  asm1, asm2, width_bits, shifter, texture);
    return 1;
}

/* ----------------------------------------------------------------------
 * Stage 5: translucent paths (transluc[] BLEND unit).
 *
 * No try/return fallback per project_gpu_owns_framebuffer.md — once a
 * call site is converted, the CPU loop is dead code.  If the active
 * palookup isn't pal0 (d3d_gpu_shade_for returns -1), the helper drops
 * the draw rather than rendering with the wrong shading.  Missing
 * geometry surfaces the multi-colormap fabric gap honestly; silent
 * mis-shading would hide it.
 * -------------------------------------------------------------------- */

void d3d_gpu_tvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture, int reverse)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade < 0) return;

    ensure_pal0_uploaded();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* OF_GPU_SPAN_TRANSLUC_REV retired in lean Phase 2.1; rev path collapsed to fwd */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture, flags);
}

void d3d_gpu_tvline2(uint8_t *dest_a, int num_pixels,
                     int shade_a, int shade_b,
                     uint32_t vplce_a, uint32_t vince_a,
                     uint32_t vplce_b, uint32_t vince_b,
                     uint8_t v_shift,
                     const uint8_t *tex_a, const uint8_t *tex_b,
                     int reverse)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade_a < 0 || shade_b < 0) return;

    ensure_pal0_uploaded();

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* OF_GPU_SPAN_TRANSLUC_REV retired in lean Phase 2.1; rev path collapsed to fwd */

    int32_t ta, tstepa, tb, tstepb;
    to_16_16(vplce_a, vince_a, v_shift, &ta, &tstepa);
    to_16_16(vplce_b, vince_b, v_shift, &tb, &tstepb);

    /* Two adjacent column spans.  BUILD's tvlineasm2 interleaves the
     * left/right writes per row; the GPU sequences them through the
     * same fragment pipe in submission order, so the FB-read for the
     * RMW blend on the right column always sees the left column's
     * just-written byte (and vice-versa across rows). */
    emit_column_span(dest_a,     num_pixels, shade_a, ta, tstepa, tex_a, flags);
    emit_column_span(dest_a + 1, num_pixels, shade_b, tb, tstepb, tex_b, flags);
}

/* dorotatesprite-recorded rotation parameters for d3d_gpu_rhline /
 * d3d_gpu_rmhline.  Set by d3d_gpu_record_rotsprite_setup() before
 * each per-row inner loop runs. */
static int32_t  rs_xv2_full;
static int32_t  rs_yv2_full;
static int32_t  rs_tileHeight;

void d3d_gpu_record_rotsprite_setup(int32_t xv2, int32_t yv2, int32_t tileHeight)
{
    rs_xv2_full   = xv2;
    rs_yv2_full   = yv2;
    rs_tileHeight = tileHeight;
}

static inline void emit_rotsprite_hline(uint8_t *dest, int num_pixels, int shade,
                                        uint32_t bx_frac, uint32_t by_frac,
                                        const uint8_t *texture, uint8_t flags)
{
    /* Same column-major-via-swapped-S/T model as d3d_gpu_sprite_vline
     * but with NEGATIVE per-pixel steps (BUILD walks `texture -= …`)
     * and fb_stride = -1 (dest writes go dest[-1], dest[-2], …).
     * fb_addr is dest - 1 so the first GPU write lands at dest[-1]. */
    of_gpu_span_t span = {
        .fb_addr   = (uint32_t)(uintptr_t)(dest - 1),
        .tex_addr  = (uint32_t)(uintptr_t)texture,
        .s         = (int32_t)(by_frac >> 16),
        .t         = (int32_t)(bx_frac >> 16),
        .sstep     = -(int32_t)rs_yv2_full,
        .tstep     = -(int32_t)rs_xv2_full,
        .count     = (uint16_t)num_pixels,
        .light     = (uint8_t)(shade & 0x3F),
        .flags     = flags,
        .fb_stride = (int16_t)-1,
        .tex_width = (uint16_t)rs_tileHeight,
    };
    of_gpu_draw_span(&span);
}

void d3d_gpu_rhline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t bx_frac, uint32_t xv2_step,
                    uint32_t by_frac, uint32_t yv2_step,
                    uint16_t tile_height,
                    const uint8_t *texture)
{
    (void)xv2_step; (void)yv2_step; (void)tile_height;  /* sourced from rs_* */
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade < 0) return;
    ensure_pal0_uploaded();
    emit_rotsprite_hline(dest, num_pixels, shade, bx_frac, by_frac,
                         texture, OF_GPU_SPAN_COLORMAP);
}

void d3d_gpu_rmhline(uint8_t *dest, int num_pixels, int shade,
                     uint32_t bx_frac, uint32_t xv2_step,
                     uint32_t by_frac, uint32_t yv2_step,
                     uint16_t tile_height,
                     const uint8_t *texture)
{
    (void)xv2_step; (void)yv2_step; (void)tile_height;
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade < 0) return;
    ensure_pal0_uploaded();
    emit_rotsprite_hline(dest, num_pixels, shade, bx_frac, by_frac,
                         texture, OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO);
}

void d3d_gpu_sprite_vline(uint8_t *dest, int num_pixels, int shade,
                          uint32_t bx_frac, uint32_t xv_step,
                          uint32_t by_frac, uint32_t yv_step,
                          uint16_t tile_height,
                          const uint8_t *texture, int reverse)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade < 0) return;

    ensure_pal0_uploaded();

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* OF_GPU_SPAN_TRANSLUC_REV retired in lean Phase 2.1; rev path collapsed to fwd */

    /* Column-major sprite addressing via swapped S/T:
     *   GPU:  addr = base + t_int * tex_width + s_int
     *   want: addr = base + X_int * tile_height + Y_int
     * so X-axis state goes into t (t_int * tile_height = column step)
     * and Y-axis state into s (s_int = byte step within column).
     *
     * BUILD's bx<<16 / by<<16 inputs carry the FRAC of bx/by in the
     * upper 16 bits; integer parts are already baked into `texture`
     * by the caller.  Convert to GPU 16.16 (frac in low 16) by
     * shifting right 16. */
    of_gpu_span_t span = {
        .fb_addr   = (uint32_t)(uintptr_t)dest,
        .tex_addr  = (uint32_t)(uintptr_t)texture,
        .s         = (int32_t)(by_frac >> 16),     /* Y-axis init frac */
        .t         = (int32_t)(bx_frac >> 16),     /* X-axis init frac */
        .sstep     = (int32_t)yv_step,             /* full 16.16 yv */
        .tstep     = (int32_t)xv_step,             /* full 16.16 xv */
        .count     = (uint16_t)num_pixels,
        .light     = (uint8_t)(shade & 0x3F),
        .flags     = flags,
        .fb_stride = (int16_t)fb_stride,
        .tex_width = tile_height,
    };
    of_gpu_draw_span(&span);
}

/* Internal: shared hline emission for forward-walking floor/ceiling
 * spans (mhlineskipmodify, thlineskipmodify).  Same shld→GPU mapping
 * as d3d_gpu_hline below, but fb_stride=+1 and S/T steps are positive
 * (BUILD's mhline does i2 += asm1; i5 += asm2; dest++ per pixel). */
static inline void emit_fwd_hline(uint8_t *dest, int num_pixels, int shade_x256,
                                  uint32_t i2, uint32_t i5,
                                  uint32_t asm1, uint32_t asm2,
                                  uint8_t width_bits, uint8_t shifter,
                                  const uint8_t *texture, uint8_t flags)
{
    int32_t s_rshift = (int32_t)16 - (int32_t)width_bits;     /* i5 → sp_s */
    int32_t t_rshift = (int32_t)shifter - 16;                 /* i2 → sp_t */
    int32_t sp_s     = (s_rshift >= 0) ? (int32_t)(i5 >> s_rshift)
                                       : (int32_t)(i5 << -s_rshift);
    int32_t sp_t     = (t_rshift >= 0) ? (int32_t)(i2 >> t_rshift)
                                       : (int32_t)(i2 << -t_rshift);
    int32_t sp_sstep = (s_rshift >= 0) ? (int32_t)(asm2 >> s_rshift)
                                       : (int32_t)(asm2 << -s_rshift);
    int32_t sp_tstep = (t_rshift >= 0) ? (int32_t)(asm1 >> t_rshift)
                                       : (int32_t)(asm1 << -t_rshift);
    uint16_t tex_w   = (uint16_t)(1u << width_bits);
    uint16_t tex_h   = (uint16_t)(1u << (32u - shifter));
    of_gpu_span_t span = {
        .fb_addr    = (uint32_t)(uintptr_t)dest,
        .tex_addr   = (uint32_t)(uintptr_t)texture,
        .s          = sp_s,
        .t          = sp_t,
        .sstep      = sp_sstep,
        .tstep      = sp_tstep,
        .count      = (uint16_t)num_pixels,
        .light      = (uint8_t)((shade_x256 >> 8) & 0x3F),
        .flags      = flags,
        .fb_stride  = (int16_t)+1,
        .tex_width  = tex_w,
        .tex_w_mask = (uint16_t)(tex_w - 1),
        .tex_h_mask = (uint16_t)(tex_h - 1),
    };
    of_gpu_draw_span(&span);
}

void d3d_gpu_mhline(uint8_t *dest, int num_pixels, int shade_x256,
                    uint32_t i2, uint32_t i5,
                    uint32_t asm1, uint32_t asm2,
                    uint8_t width_bits, uint8_t shifter,
                    const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();
    /* Caller (mhlineskipmodify) must have verified mmach_asm3 is in
     * pal0's range — see hook in draw.c. */
    emit_fwd_hline(dest, num_pixels, shade_x256, i2, i5, asm1, asm2,
                   width_bits, shifter, texture,
                   OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO);
}

void d3d_gpu_thline(uint8_t *dest, int num_pixels, int shade_x256,
                    uint32_t i2, uint32_t i5,
                    uint32_t asm1, uint32_t asm2,
                    uint8_t width_bits, uint8_t shifter,
                    const uint8_t *texture, int reverse)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();
    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* OF_GPU_SPAN_TRANSLUC_REV retired in lean Phase 2.1; rev path collapsed to fwd */
    emit_fwd_hline(dest, num_pixels, shade_x256, i2, i5, asm1, asm2,
                   width_bits, shifter, texture, flags);
}

void d3d_gpu_hline(uint8_t *dest_right, int num_pixels, int shade_x256,
                   uint32_t i4, uint32_t i5,
                   uint32_t asm1, uint32_t asm2,
                   uint8_t width_bits, uint8_t shifter,
                   const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();
    /* Caller (hlineasm4) must have already checked that the floor's
     * pal matches the one in the GPU's colormap RAM via the public
     * predicate — see hlineasm4's gate in draw.c. */

    /* GPU multiply-mode + POT wrap masks reproduce BUILD's hlineasm4
     * shld addressing exactly:
     *   BUILD: src = ((i5 >> shifter) << width_bits) | (i4 >> (32 - width_bits))
     *   GPU:   addr = base + (sp_t_int & h_mask) * tex_w + (sp_s_int & w_mask)
     *
     * Translate (i4, i5) in 0.32 to (sp_s, sp_t) in 16.16 by shifting
     * so the integer fields line up:
     *   sp_s_int = i4 >> 27 (= top width_bits of i4) requires
     *   sp_s     = i4 >> (32 - width_bits - 16) = i4 >> (16 - width_bits)
     *   sp_t     = i5 >> (shifter - 16)
     * (negative shift means the width is wider than 16 bits — left-
     * shift instead).  Per-pixel deltas scale the same way:
     *   sp_sstep = -(asm2 >> (16 - width_bits))
     *   sp_tstep = -(asm1 >> (shifter - 16))
     *
     * tex_width = 1 << width_bits.  POT wrap masks tile the texture
     * mod (tex_w, tex_h) — BUILD/Duke3D textures are always POT, so
     * the mask reproduces shld's natural 32-bit wrap byte-for-byte. */
    int32_t s_rshift = (int32_t)16 - (int32_t)width_bits;     /* i4 → sp_s   */
    int32_t t_rshift = (int32_t)shifter - 16;                 /* i5 → sp_t   */
    int32_t sp_s     = (s_rshift >= 0) ? (int32_t)(i4 >> s_rshift)
                                       : (int32_t)(i4 << -s_rshift);
    int32_t sp_t     = (t_rshift >= 0) ? (int32_t)(i5 >> t_rshift)
                                       : (int32_t)(i5 << -t_rshift);
    int32_t sp_sstep = (s_rshift >= 0) ? -(int32_t)(asm2 >> s_rshift)
                                       : -(int32_t)(asm2 << -s_rshift);
    int32_t sp_tstep = (t_rshift >= 0) ? -(int32_t)(asm1 >> t_rshift)
                                       : -(int32_t)(asm1 << -t_rshift);
    uint16_t tex_w   = (uint16_t)(1u << width_bits);
    uint16_t tex_h   = (uint16_t)(1u << (32u - shifter));
    of_gpu_span_t span = {
        .fb_addr    = (uint32_t)(uintptr_t)dest_right,
        .tex_addr   = (uint32_t)(uintptr_t)texture,
        .s          = sp_s,
        .t          = sp_t,
        .sstep      = sp_sstep,
        .tstep      = sp_tstep,
        .count      = (uint16_t)num_pixels,
        .light      = (uint8_t)((shade_x256 >> 8) & 0x3F),
        .flags      = OF_GPU_SPAN_COLORMAP,
        .fb_stride  = (int16_t)-1,        /* walk left, matching SW dest-- */
        .tex_width  = tex_w,
        .tex_w_mask = (uint16_t)(tex_w - 1),
        .tex_h_mask = (uint16_t)(tex_h - 1),
    };
    of_gpu_draw_span(&span);
}
