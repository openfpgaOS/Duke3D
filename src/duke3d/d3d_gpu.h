/*
 * d3d_gpu.h — Duke3D ↔ openfpgaOS GPU rasteriser glue.
 *
 * openfpgaOS GPU replacement paths for BUILD's hot renderer loops.
 *
 * Lifecycle:
 *   d3d_gpu_init()         — once after of_video_init()
 *   d3d_gpu_set_fb(...)    — after each retarget_frameplace
 *   d3d_gpu_upload_palookup() — once per global palette change
 *   d3d_gpu_flush()        — immediately before of_video_flip()
 */

#ifndef D3D_GPU_H
#define D3D_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master enable for any GPU work at all.  Turned on by d3d_gpu_init() if
 * the runtime caps descriptor advertises a GPU; left at 0 otherwise so
 * the same ELF runs on GPU-less targets without divergence. */
extern int d3d_gpu_present;

/* The single runtime hardware-renderer switch.  When 1, converted BUILD
 * draw functions submit spans to the GPU; when 0 they use the original
 * software loops.  All sub-path choices below are fixed to the current
 * fastest stable command mix. */
extern int d3d_gpu_use_spans;

/* Scoped CPU fallback gate for BUILD paths that must preserve exact
 * software framebuffer semantics.  This is internal flow control, not a
 * user-facing renderer setting. */
extern int d3d_gpu_force_cpu_spans;

/* A/B verification toggle for the CMD_DRAW_COLUMN_LIST (0x4C) column
 * path: set to 1 to force column-eligible draws (wall/sprite vlines,
 * s=0/sstep=0) back onto the 0x48 affine span-group path.  Both paths
 * produce byte-identical pixels by hardware contract; flippable at
 * runtime.  No effect on bitstreams without OF_HW_GPU_COLUMN_LIST,
 * where the affine path is always used. */
extern int d3d_gpu_force_affine_columns;

/* Fixed command policy for the current openfpgaOS GPU:
 * BUILD computes spans on the CPU and Duke submits SDK affine span groups
 * with explicit per-lane colormap ids.  The SDK lowers those groups to the
 * unified GPU_CMD_DRAW_PARAM_SPAN_LIST command.  Rotatesprite stays on CPU
 * for byte-sensitive 2D/menu/save paths. */
#define D3D_GPU_FORCE_ROTATESPRITE_CPU      1
#define D3D_GPU_USE_CACHED_FRAMEPLACE       0

/* Perf tracing is compiled cold for release builds. */
#define d3d_gpu_perf_enable      0
#define d3d_gpu_perf_deep_enable 0
#define d3d_gpu_perf_time_paths  0

enum {
    D3D_GPU_PERF_PHASE_DISPLAYROOMS = 0,
    D3D_GPU_PERF_PHASE_DRAWROOMS,
    D3D_GPU_PERF_PHASE_DRAWMASKS,
    D3D_GPU_PERF_PHASE_MOVELOOP,
    D3D_GPU_PERF_PHASE_LOOP_MISC,
    D3D_GPU_PERF_PHASE_DISPLAYREST,
    D3D_GPU_PERF_PHASE_POSTREST,
    D3D_GPU_PERF_PHASE_MOVE_PACKETS,
    D3D_GPU_PERF_PHASE_DOMOVE_TOTAL,
    D3D_GPU_PERF_PHASE_DOMOVE_SETUP,
    D3D_GPU_PERF_PHASE_DOMOVE_INPUT,
    D3D_GPU_PERF_PHASE_DOMOVE_FTA,
    D3D_GPU_PERF_PHASE_DOMOVE_WEAPONS,
    D3D_GPU_PERF_PHASE_DOMOVE_TRANSPORTS,
    D3D_GPU_PERF_PHASE_DOMOVE_PLAYERS,
    D3D_GPU_PERF_PHASE_DOMOVE_FALLERS,
    D3D_GPU_PERF_PHASE_DOMOVE_EXPLOSIONS,
    D3D_GPU_PERF_PHASE_DOMOVE_ACTORS,
    D3D_GPU_PERF_PHASE_DOMOVE_EFFECTORS,
    D3D_GPU_PERF_PHASE_DOMOVE_STANDABLES,
    D3D_GPU_PERF_PHASE_DOMOVE_ANIM,
    D3D_GPU_PERF_PHASE_DOMOVE_FX,
    D3D_GPU_PERF_PHASE_DOMOVE_FAKE,
    D3D_GPU_PERF_PHASE_DOMOVE_CYCLERS,
    D3D_GPU_PERF_PHASE_COUNT
};

enum {
    D3D_GPU_PERF_ZONE_CEILSCAN = 0,
    D3D_GPU_PERF_ZONE_FLORSCAN,
    D3D_GPU_PERF_ZONE_WALLSCAN,
    D3D_GPU_PERF_ZONE_MASKWALLSCAN,
    D3D_GPU_PERF_ZONE_TRANSMASKWALLSCAN,
    D3D_GPU_PERF_ZONE_DRAWMASKWALL,
    D3D_GPU_PERF_ZONE_DRAWSPRITE,
    D3D_GPU_PERF_ZONE_DOROTATESPRITE,
    D3D_GPU_PERF_ZONE_COUNT
};

enum {
    D3D_GPU_STALL_TEX_WAIT = 0,
    D3D_GPU_STALL_CMAP_WAIT,
    D3D_GPU_STALL_CMAP_ISSUE,
    D3D_GPU_STALL_FBSS_BUSY,
    D3D_GPU_STALL_FB_WRITE,
    D3D_GPU_STALL_INFLIGHT,
    D3D_GPU_STALL_PERSP_WAIT,
    D3D_GPU_STALL_COUNT
};

typedef struct d3d_gpu_perf_capture_s {
    uint32_t valid;
    uint32_t seq;
    uint32_t elapsed_ms;
    uint32_t frames;
    uint32_t render_avg_us, render_max_us;
    uint32_t page_avg_us, page_max_us;
    uint32_t wait_avg_us, wait_max_us;
    uint32_t acquire_avg_us, acquire_max_us;
    uint32_t finish_avg_us, finish_max_us;
    uint32_t sync_avg_us, sync_max_us;
    uint32_t batches, max_batch;
    uint32_t submit_avg_us, submit_max_us;
    uint32_t spans, span_pixels;
    uint32_t vline_spans, vline_pixels;
    uint32_t mvline_spans, mvline_pixels;
    uint32_t vline4_spans, vline4_pixels;
    uint32_t mvline4_spans, mvline4_pixels;
    uint32_t hline_calls, hline_pixels;
    uint32_t sprite_spans, sprite_pixels;
    uint32_t tile_loads, tile_bytes;
    uint32_t tile_load_avg_us, tile_load_max_us;
    uint32_t tile_load_max_id, tile_load_max_bytes;
    uint32_t move_steps, move_steps_max;
    uint32_t move_backlog_max, move_remaining_max;
    uint32_t tex_req_avg, tex_miss_avg;
    uint32_t tex_req_max, tex_miss_max;
    uint32_t tex_miss_permille;
    uint32_t dma_waits, dma_spin_iters;
    uint32_t ring_waits, ring_spin_iters;
    uint32_t ring_min_free, ring_last_free;
    uint32_t gpu_status;
    uint32_t stall_avg[D3D_GPU_STALL_COUNT];
    uint32_t stall_max[D3D_GPU_STALL_COUNT];
    uint32_t phase_avg_us[D3D_GPU_PERF_PHASE_COUNT];
    uint32_t phase_max_us[D3D_GPU_PERF_PHASE_COUNT];
    uint32_t zone_avg_us[D3D_GPU_PERF_ZONE_COUNT];
    uint32_t zone_max_us[D3D_GPU_PERF_ZONE_COUNT];
} d3d_gpu_perf_capture_t;

extern volatile d3d_gpu_perf_capture_t d3d_gpu_perf_latest;
extern volatile d3d_gpu_perf_capture_t d3d_gpu_perf_worst;

void d3d_gpu_init(void);
void d3d_gpu_set_fb(uint8_t *fb_pixels, int stride_pixels);
void d3d_gpu_prepare_framebuffer_for_present(void);
void d3d_gpu_upload_palookup(const uint8_t *palookup_table, int num_shades);
void d3d_gpu_flush(void);
void d3d_gpu_perf_capture_pending(void);
void d3d_gpu_perf_discard_interval(void);
void d3d_gpu_perf_dump(void);

void d3d_gpu_perf_note_cpu_fallback(void);
void d3d_gpu_perf_note_tile_load(uint32_t tile_id, uint32_t bytes,
                                 uint32_t us);
void d3d_gpu_perf_note_moveloop(uint32_t steps, uint32_t backlog,
                                uint32_t remaining);
void d3d_gpu_perf_note_phase(int phase, uint32_t us);
void d3d_gpu_perf_note_zone(int zone, uint32_t us);
void d3d_gpu_perf_report_frame(uint32_t frame_period_us,
                               uint32_t render_us,
                               uint32_t page_us,
                               uint32_t wait_flip_us,
                               uint32_t drain_batch_us,
                               uint32_t flip_emit_us,
                               uint32_t acquire_us,
                               uint32_t audio_us);

/* Drain pending span buffer to the GPU ring without emitting a fence.
 * Used by the GPU-triggered flip path in display_of.c::_nextpage —
 * the subsequent d3d_gpu_flip_to() emits its own fence + drain
 * primitive via CMD_FLIP, so we don't want d3d_gpu_flush()'s
 * of_gpu_finish wait here. */
void d3d_gpu_drain_batch(void);

/* GPU-triggered flip — emit CMD_FLIP into the ring and kick.  Returns
 * the fence token of_gpu_flip_to gave us so the caller can pass it to
 * of_video_acquire_next() for the wait.  Lives here (not display_of.c)
 * because of_gpu.h's static ring state (_gpu_wrptr, _gpu_fence_next)
 * MUST live in exactly one TU; d3d_gpu.c owns it. */
uint32_t d3d_gpu_flip_to(int idx);

/* GPU-routed horizontal mirror reverse-blit.  Used by completemirror
 * to flip a screen rect through the X axis: for each row, pixels are
 * read from `src` (size `count` bytes), reversed, and written to
 * `dst`.  Both addresses are FB-resident — the helper emits a
 * CMD_FENCE before the blit spans so the GPU's m_wr_inflight from
 * earlier rendering drains to SDRAM before the tex_cache reads
 * back.  Per-row affine spans with sstep=-0x10000 do the reverse
 * sample.  Replaces the old CPU memcpy + reverse loop, which was
 * the last documented CPU FB read+write path; cached frameplace mode
 * now cleans CPU fallback writes before later GPU work/present. */
void d3d_gpu_blit_mirror(uint8_t *dst, const uint8_t *src,
                         int count, int rows, int row_stride);

/* GPU-routed FB rect clear.  Pointer-friendly wrapper around the SDK
 * of_gpu_clear_rect; safe no-op when the GPU isn't present (caller
 * gets the same nothing-rendered behaviour as the previous CPU memset
 * sites without having to gate on d3d_gpu_present themselves).
 * Letterbox bars, 2D `clear2dscreen`, fillscreen16, drawline16,
 * setgamemode HW-buffer wipe — all of the per-frame
 * memset(frameplace, …) categories route through here so the FB is
 * 100% GPU-owned (project_gpu_owns_framebuffer.md). */
void d3d_gpu_clear_rect_fb(uint8_t *dest, uint16_t w, uint16_t h, uint8_t color);

/* Upload BUILD's transluc[65536] LUT to the fabric BLEND unit.  Call
 * once per level (after loadpalette has filled `transluc`).  Wraps the
 * SDK helper so callers don't need to drag of_gpu.h's static state into
 * a second TU. */
void d3d_gpu_upload_transluc(const uint8_t *table, uint32_t size);
int d3d_gpu_translucent_spans_ready(void);

/* Invalidate the GPU texture cache immediately.  Call only when the GPU is
 * already idle; hot tile-loading paths drain before using this. */
void d3d_gpu_tex_invalidate(void);

/* Legacy compatibility hook; current hot tile paths invalidate explicitly
 * after draining and syncing the changed bytes. */
void d3d_gpu_mark_tex_dirty(void);

/* GPU-only drain (no CPU cache flush).  display_of.c uses it to time
 * the GPU finish step separately from the CPU cache flush. */
void d3d_gpu_drain(void);

/* Call BEFORE any CPU code that needs to read or write the
 * framebuffer this frame (e.g. completemirror's reverse-blit).
 * Drains the GPU so SDRAM holds all pending pixels, invalidates L1
 * so CPU reads hit fresh SDRAM, and arms d3d_gpu_flush() to do a
 * write-back at the end of the frame so any CPU writes the caller
 * makes get pushed to SDRAM before of_video_flip().  Common case
 * (no CPU FB access at all this frame) skips the flush — that's the
 * win that retiring of_cache_flush from the hot path delivers. */
void d3d_gpu_pre_cpu_fb_access(void);

/* Cheaper write-only variant for CPU code that only draws through the
 * uncached framebuffer alias.  It drains GPU writes for ordering, but
 * skips the full D-cache flush needed only before CPU framebuffer reads. */
void d3d_gpu_prepare_cpu_fb_write(void);

/* Returns shade index 0..63 if `palookupoffse` is inside a GPU-loaded
 * palookup slot, or can be lazily uploaded into one.  Returns -1 if no
 * slot is available; caller must fall back to the SW path so the draw
 * uses the correct per-pal shading.  The slot variant also reports the
 * selected slot, which the span encoder writes into the lane metadata. */
int  d3d_gpu_shade_for(const uint8_t *palookupoffse);
int  d3d_gpu_shade_slot_for(const uint8_t *palookupoffse, int *slot_out);

/* Try-helpers for the BUILD inner loops.  Each lives outside the
 * OF_FASTTEXT section so it gets its own ABI-clean prologue/epilogue;
 * the call site in draw.c becomes a single boolean dispatch with no
 * register-save divergence between GPU and SW paths.
 *
 * Each returns 1 if the GPU absorbed the call (caller MUST return
 * immediately), 0 if it didn't (caller falls through to its scalar
 * SW loop, which then runs unaffected).
 *
 * The vline*_try variants take all of vlineasm1's args plus the
 * mach3_al shift; they do the shade range check, the GPU dispatch,
 * and the SW-equivalent vplce update internally. */
int  d3d_gpu_try_vline1(uint8_t *dest, int num_pixels,
                        const uint8_t *palookupoffse,
                        int32_t vplce, int32_t vince,
                        uint8_t v_shift, const uint8_t *texture,
                        int32_t *vplce_out);
int  d3d_gpu_try_mvline1(uint8_t *dest, int num_pixels,
                         const uint8_t *palookupoffse,
                         int32_t vplce, int32_t vince,
                         uint8_t v_shift, const uint8_t *texture,
                         int32_t *vplce_out);
int  d3d_gpu_try_vline4(uint8_t *framebuffer, int num_pixels,
                        uint8_t v_shift);
int  d3d_gpu_try_mvline4(uint8_t *framebuffer, int num_pixels,
                         uint8_t v_shift);
int  d3d_gpu_try_hline(uint8_t *dest_right, int num_pixels, int shade_x256,
                       uint32_t i4, uint32_t i5,
                       uint32_t asm1, uint32_t asm2,
                       uint8_t width_bits, uint8_t shifter,
                       const uint8_t *texture);

/* Span helpers used by the gated draw replacements in draw.c.  Kept in
 * d3d_gpu.c so of_gpu.h's static mutable ring state stays in one TU. */
void d3d_gpu_vline(uint8_t *dest, int num_pixels, int shade,
                   uint32_t vplce, uint32_t vince, uint8_t v_shift,
                   const uint8_t *texture);

/* Transparent-aware vline: same as d3d_gpu_vline but emits the
 * SPAN_SKIP_ZERO flag so texel value 0xFF (BUILD's TRANSPARENT_COLOR)
 * is dropped at the fragment stage. */
void d3d_gpu_mvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture);

/* Compact hardware 4-column batch (vlineasm4 replacement). */
void d3d_gpu_vline4(uint8_t *fb_at_y0, int num_pixels,
                    const int shade[4],
                    const uint32_t vplce[4],
                    const uint32_t vince[4],
                    uint8_t v_shift,
                    const uint8_t *const texture[4]);

/* Masked 4-column batch (mvlineasm4 replacement).  Same as vline4 but
 * each span carries SPAN_SKIP_ZERO so 0xFF texels drop. */
void d3d_gpu_mvline4(uint8_t *fb_at_y0, int num_pixels,
                     const int shade[4],
                     const uint32_t vplce[4],
                     const uint32_t vince[4],
                     uint8_t v_shift,
                     const uint8_t *const texture[4]);

/* ----------------------------------------------------------------------
 * Translucent paths (fabric transluc[] BLEND unit).
 *
 * Architectural principle (project_gpu_owns_framebuffer.md): once the
 * GPU is doing the work, the CPU MUST NOT write the framebuffer on the
 * converted paths.  The helpers below are therefore unconditional —
 * NO try/return fallback; the caller's CPU loop is dead code.  If a
 * helper hits a fabric gap (for example no palookup slot is available),
 * it skips the draw rather than silently writing the wrong shading;
 * missing translucent geometry is an honest signal that the gap is real.
 *
 * The translucency LUT upload path disables these helpers if the fabric
 * BLEND unit does not respond, so reverse translucency and missing hardware
 * still fall back through the original CPU call sites. -------- */

/* Translucent vertical wall column — replaces draw.c::tvlineasm1 for
 * Duke TRANS_NORMAL. `texture` is the 1-D column data (BUILD's `source`
 * arg in tvlineasm1). TRANS_REVERSE falls back to CPU because current
 * fabric no longer exposes a per-span reverse bit. */
void d3d_gpu_tvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture, int reverse);

/* Translucent paired columns — replaces draw.c::tvlineasm2.  Issues two
 * column spans at dest_a and dest_a+1 (BUILD's tran2edi / tran2edi+1).
 * The two columns share v_shift but otherwise have independent vplce /
 * vince / texture / shade / colormap slot. */
void d3d_gpu_tvline2(uint8_t *dest_a, int num_pixels,
                     int shade_a, int shade_b,
                     int slot_a, int slot_b,
                     uint32_t vplce_a, uint32_t vince_a,
                     uint32_t vplce_b, uint32_t vince_b,
                     uint8_t v_shift,
                     const uint8_t *tex_a, const uint8_t *tex_b,
                     int reverse);

/* Translucent rotated/scaled sprite vertical column — replaces
 * draw.c::DrawSpriteVerticalLine.  Models BUILD's two-axis fractional
 * sprite walk as a GPU affine column span over a column-major sprite
 * (tex_height = sprite column stride).  Caller passes the FULL 16.16
 * xv / yv per-pixel steps and the initial fractional bx / by — the
 * integer parts are already baked into `texture` by BUILD's caller.
 *
 * Note: BUILD's SW path uses a one-time `adder` bump on first X-frac
 * wrap which only matches an affine pattern when xv-frac wraps at
 * most once per column.  The GPU path is the exact affine; for
 * typical sprite columns the two are byte-identical, for very long
 * columns the GPU path is correct and the SW path was the
 * approximation. */
void d3d_gpu_sprite_vline(uint8_t *dest, int num_pixels, int shade,
                          uint32_t bx_frac, uint32_t xv_step,
                          uint32_t by_frac, uint32_t yv_step,
                          uint16_t tile_height,
                          const uint8_t *texture, int reverse);

/* Rotated affine sprite hlines (rhlineasm4 / rmhlineasm4 replacements).
 * Horizontal-walk analog of d3d_gpu_sprite_vline: column-major sprite
 * via swapped S/T (tex_width = tile_height), negative steps for the
 * BUILD `texture -= …` loop, and fb_stride = -1 for the dest[-1],
 * dest[-2] write pattern.  Caller passes the full 16.16 xv2 / yv2 and
 * the initial fractional bx<<16 / by<<16 just as for sprite_vline.
 *   rhline  = SPAN_COLORMAP                         (opaque)
 *   rmhline = SPAN_COLORMAP | SPAN_SKIP_ZERO        (color-key) */
int d3d_gpu_rhline(uint8_t *dest, int num_pixels, int shade,
                   uint32_t bx_frac, uint32_t xv2_step,
                   uint32_t by_frac, uint32_t yv2_step,
                   uint16_t tile_height,
                   const uint8_t *texture);

int d3d_gpu_rmhline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t bx_frac, uint32_t xv2_step,
                    uint32_t by_frac, uint32_t yv2_step,
                    uint16_t tile_height,
                    const uint8_t *texture);

/* Recorder called from engine.c::dorotatesprite before each
 * setuprhlineasm4 / setuprmhlineasm4 so the rh/rmh hooks have access
 * to the full 16.16 xv2 / yv2 and tileHeight without trying to
 * reverse-engineer them from BUILD's frac/integer-split statics. */
void d3d_gpu_record_rotsprite_setup(int32_t xv2, int32_t yv2, int32_t tileHeight);

/* Masked horizontal span (mhlineskipmodify replacement).  Same shld
 * addressing as hlineasm4 but walks dest++, with positive S/T steps
 * and SPAN_SKIP_ZERO for color-key transparency on 0xFF texels.
 *   shade_x256  = palookup row offset already × 256 (BUILD's `shade`)
 *   i2 / i5     = initial T / S in 0.32
 *   asm1 / asm2 = per-pixel +T / +S
 *   width_bits  = log2(tex_width); shifter as in hlineasm4 */
void d3d_gpu_mhline(uint8_t *dest, int num_pixels, int shade_x256,
                    uint32_t i2, uint32_t i5,
                    uint32_t asm1, uint32_t asm2,
                    uint8_t width_bits, uint8_t shifter,
                    const uint8_t *texture);

/* Translucent horizontal span (thlineskipmodify replacement) for Duke
 * TRANS_NORMAL. Same as mhline plus SPAN_TRANSLUC; TRANS_REVERSE falls
 * back to CPU at the draw.c call site. */
void d3d_gpu_thline(uint8_t *dest, int num_pixels, int shade_x256,
                    uint32_t i2, uint32_t i5,
                    uint32_t asm1, uint32_t asm2,
                    uint8_t width_bits, uint8_t shifter,
                    const uint8_t *texture, int reverse);

/* Affine horizontal span (hlineasm4 replacement).  Reverse-direction:
 * the SW loop walks dest--, so we feed fb_stride = -1 and let the GPU
 * step right-to-left from `dest_right`.
 *   width_bits  = log2(tex_width)            (bitsSetup)
 *   shifter     = bit position of T integer  ((256 - machxbits_al)&31)
 *   shade_x256  = palookup row offset already multiplied by 256
 *                 (matches BUILD's hlineasm4 first-arg encoding)
 *   i4 / i5     = initial S / T fixed-point at dest_right
 *   asm1 / asm2 = per-pixel T / S decrement (i5 -= asm1, i4 -= asm2)
 *   texture     = full 2-D tile base */
void d3d_gpu_hline(uint8_t *dest_right, int num_pixels, int shade_x256,
                   uint32_t i4, uint32_t i5,
                   uint32_t asm1, uint32_t asm2,
                   uint8_t width_bits, uint8_t shifter,
                   const uint8_t *texture);

#ifdef __cplusplus
}
#endif

#endif /* D3D_GPU_H */
