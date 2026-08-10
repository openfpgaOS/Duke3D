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
#include "of_timer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(OF_GPU_STALL_COUNT) && defined(OF_GPU_TEX_DBG_COUNTER_MASK)
#define D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS 1
#else
#define D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS 0
#endif

#if D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS
typedef char d3d_gpu_stall_count_must_match[
    ((int)D3D_GPU_STALL_COUNT == (int)OF_GPU_STALL_COUNT) ? 1 : -1];
#endif

int d3d_gpu_present  = 0;
#define D3D_GPU_SKIP_SUBMIT 0

int d3d_gpu_use_spans = 1;   /* Master switch.  Set to 0 before init to
                              * keep BUILD on its original CPU renderer. */
int d3d_gpu_force_cpu_spans = 0;  /* Scoped gate: when non-zero, draw.c
                                   * try paths fall through to BUILD's
                                   * original CPU loops. */
static int d3d_gpu_translucent_spans_enabled = 1;

static int gpu_use_column_list;   /* 0x4C caps bit + probe, set at init */

/* A/B verification toggle: force column-eligible spans (s=0/sstep=0)
 * back onto the 0x48 affine span-group path.  Checked at queue time so
 * it can be flipped live; both paths render byte-identical pixels. */
int d3d_gpu_force_affine_columns = 0;

/* --- Non-fatal emission guard --------------------------------------
 * of_gpu.h's ring waits are bounded but FATAL: on timeout they trap and
 * take the machine down.  That is right for a wedged pipeline, but wrong
 * when the GPU is merely paused — the platform menu freezes scanout, so
 * the ring stops draining while BUILD keeps drawing.  Duke fills a 16 KB
 * ring well inside one frame, so the old behaviour was a hard trap on a
 * condition that resolves the instant the menu closes.
 *
 * Every flush now probes for its WHOLE batch up front with
 * of_gpu_can_emit(), then falls back to a short bounded reserve before
 * giving up.  Ring space only grows until we emit (the GPU is the sole
 * consumer), so a batch that probes clean cannot block partway through —
 * that is the property the up-front probe buys, and why the probe covers
 * the full chunked byte count rather than one command at a time.
 *
 * On failure the staged lanes are DROPPED, not emitted.  The frame is
 * already lost (scanout is frozen); a torn frame that recovers beats a
 * trap that does not.  The latch keeps the rest of the frame cheap —
 * once stalled we stop re-probing per batch — and clears at the frame
 * boundary so a recovered GPU resumes on the next page. */
static int gpu_emit_stalled;
static uint32_t gpu_emit_stall_events;
static uint32_t gpu_emit_dropped_lanes;

/* Short: this is the "GPU is paused" path, not the "GPU is slow" path.
 * _gpu_ring_ensure's own fatal spin is 50M iterations; anything that
 * needs more than a nudge here is a stall, and waiting longer only
 * burns the frame we are trying to salvage. */
#define D3D_GPU_EMIT_SPIN_LIMIT 20000u

static int d3d_gpu_reserve_or_drop(uint32_t bytes)
{
    /* The healthy path must cost nothing.  _gpu_ring_ensure() decides on
     * the CACHED rdptr and only touches the register once that says the
     * ring is short; of_gpu_can_emit() always reads GPU_RING_RDPTR.
     * Probing with it unconditionally would add an uncached MMIO read to
     * every flush — and Duke flushes on every batch fill and every state
     * change, so that is hundreds of reads a frame on a path that used
     * to do none.
     *
     * Agreeing with the cached value is also what makes deferring safe:
     * if it says there is room, the emit path's fast path reads the same
     * value and never reaches the fatal spin.  A stale cache can only
     * UNDER-report free space (wrptr advances, rdptr looks frozen), so
     * the error direction is a needless register read, never a missed
     * trap. */
    if (_gpu_ring_free_known() >= bytes)
        return 1;

    /* Cached view says short — now the register read is worth paying. */
    if (of_gpu_can_emit(bytes))
        return 1;
    if (of_gpu_try_reserve_bytes(bytes, D3D_GPU_EMIT_SPIN_LIMIT))
        return 1;

    if (!gpu_emit_stalled) {
        gpu_emit_stalled = 1;
        gpu_emit_stall_events++;
        /* Reported unconditionally: this is a FAULT (we are dropping the
         * frame's draws), not a perf metric, and the perf harness is
         * compiled out of release builds — gating it there would make the
         * only symptom invisible on exactly the builds that ship.
         * Throttled because the latch re-arms every frame, so a menu held
         * open would otherwise emit one line per frame forever. */
        if (gpu_emit_stall_events <= 8u ||
            (gpu_emit_stall_events & 0xFFu) == 0u)
            printf("[d3d_gpu] ring stalled, dropping draws "
                   "(event %u, %u lanes dropped so far)\n",
                   gpu_emit_stall_events, gpu_emit_dropped_lanes);
    }
    return 0;
}

/* Worst-case wire bytes for a lane batch, matching the SDK's chunking:
 * both emitters split at their MAX_NATIVE_LANES, and each chunk carries
 * its own 1-word command header on top of the payload words. */
static inline uint32_t d3d_gpu_batch_bytes(uint32_t lanes, uint32_t native,
                                           uint32_t lane_words,
                                           uint32_t payload_fixed)
{
    uint32_t chunks = (lanes + native - 1u) / native;
    return (chunks * (1u + payload_fixed) + lanes * lane_words) * 4u;
}

static const uint8_t *pending_transluc_table;
static uint32_t pending_transluc_size;

#define D3D_GPU_REQUIRED_FEATURES \
    (OF_HW_GPU_SPAN | OF_HW_GPU_FRAGPIPE | OF_HW_GPU_PARAM_SPAN_LIST)
#define D3D_GPU_TRANSLUC_WAIT_TIMEOUT_US 50000u

/* Cached framebuffer base for the GPU-disabled CPU-fallback path.
 * Stride is read from BUILD's bytesperline at submit time: setviewtotile
 * (low-detail mode, tilted/rotated screen blits) flips bytesperline
 * mid-frame between screen stride and tile stride, and a stale cache
 * here would land vline writes on every other tile row. */
static uint8_t *fb_base;
extern int32_t  bytesperline;   /* BUILD's live row stride in bytes */
extern uint8_t *frameplace;     /* BUILD's active CPU framebuffer alias */
static int gpu_fb_drained_for_cpu;
static int cpu_fb_read_coherent;
static int gpu_fb_read_barrier_needed;
static int cpu_fb_dirty_for_gpu;

#define D3D_GPU_DISPLAY_W     320
#define D3D_GPU_DISPLAY_H     200

static inline void mark_gpu_fb_dirty(void)
{
    gpu_fb_drained_for_cpu = 0;
    cpu_fb_read_coherent = 0;
}

static uint8_t *d3d_gpu_cached_sdram_alias(uint8_t *ptr)
{
    const struct of_capabilities *caps = of_get_caps();
    if (!ptr || !caps || caps->sdram_base == 0 ||
        caps->sdram_uncached_base == 0 || caps->sdram_size == 0)
        return ptr;

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t uncached_base = (uintptr_t)caps->sdram_uncached_base;
    uintptr_t sdram_size = (uintptr_t)caps->sdram_size;
    if (addr >= uncached_base && addr - uncached_base < sdram_size)
        return (uint8_t *)(uintptr_t)(addr - uncached_base +
                                      (uintptr_t)caps->sdram_base);

    return ptr;
}

static int d3d_gpu_is_uncached_sdram_alias(const void *ptr)
{
    const struct of_capabilities *caps = of_get_caps();
    if (!ptr || !caps || caps->sdram_uncached_base == 0 || caps->sdram_size == 0)
        return 0;

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t uncached_base = (uintptr_t)caps->sdram_uncached_base;
    return addr >= uncached_base &&
           addr - uncached_base < (uintptr_t)caps->sdram_size;
}

static void d3d_gpu_clean_cpu_fb_range(void)
{
    if (!fb_base || d3d_gpu_is_uncached_sdram_alias(frameplace))
        return;

    if (bytesperline == D3D_GPU_DISPLAY_W)
        of_cache_flush_range(fb_base, D3D_GPU_DISPLAY_W * D3D_GPU_DISPLAY_H);
    else
        of_cache_flush();
    __asm__ volatile("fence" ::: "memory");
}

static inline void d3d_gpu_clean_cpu_fb_before_gpu(void)
{
    if (!cpu_fb_dirty_for_gpu)
        return;
    d3d_gpu_clean_cpu_fb_range();
    cpu_fb_dirty_for_gpu = 0;
    cpu_fb_read_coherent = 0;
}

/* Spinwatch: every entry/exit point that could spin updates this
 * tag.  Because the SDK app and kernel are separate binaries, we
 * print directly here (throttled to every 256th call so steady-
 * state load is small).  When a freeze happens, the LAST tag line
 * before silence pinpoints which path the CPU got stuck in.
 *
 * Tag IDs:
 *   0  = idle / exiting
 *   1  = d3d_gpu_drain entered
 *   2  = d3d_gpu_flush entered
 *   3  = d3d_gpu_pre_cpu_fb_access entered
 *   4  = of_gpu_finish entered (about to spin on fence)
 *   5  = d3d_gpu_flush_batch entered
 *   6  = d3d_gpu_drain_batch entered
 *   7  = d3d_gpu_flip_to entered
 *   8  = d3d_gpu_clear_rect_fb entered
 */
volatile uint32_t d3d_spin_tag = 0;
volatile uint32_t d3d_spin_seq = 0;
static inline void d3d_spin_log(uint32_t t) {
    d3d_spin_tag = t;
    d3d_spin_seq++;
}
#define SPIN_TAG(t) d3d_spin_log(t)

/* Multi-palookup state.  The fabric now holds up to 16 palookups in
 * SDRAM (one per slot), reachable via gpu_tex_cache port B.  We upload
 * lazily on first encounter of a palookup row that doesn't fall in any
 * already-loaded slot, then put that slot in each span's colormap_id
 * nibble so palette changes do not split batches.  Slot 0 is reserved
 * for palookup[0] (the pal0 default that the GPU resets to). */
#define D3D_GPU_MAX_SHADES   64
#define D3D_GPU_PAL_SLOTS    OF_GPU_PALOOKUP_SLOTS  /* fabric advertises 16 */
#define D3D_GPU_SHADE_CACHE_SIZE 32

static const uint8_t *gpu_pal_base_of_slot[D3D_GPU_PAL_SLOTS];
static int            gpu_pal_next_slot;     /* number of slots in use */
static int            gpu_current_slot;      /* Last resolved span slot, -1 = unset */
extern short          numpalookups;

typedef struct d3d_gpu_shade_cache_entry_s {
    const uint8_t *row;
    uint8_t        shade;
    uint8_t        slot;
} d3d_gpu_shade_cache_entry_t;

static d3d_gpu_shade_cache_entry_t gpu_shade_cache[D3D_GPU_SHADE_CACHE_SIZE];

static inline int d3d_gpu_pal_shades(void)
{
    int shades = (int)numpalookups;
    if (shades <= 0)
        shades = D3D_GPU_MAX_SHADES;
    if (shades > D3D_GPU_MAX_SHADES)
        shades = D3D_GPU_MAX_SHADES;
    return shades;
}

static inline uint32_t d3d_gpu_pal_bytes(void)
{
    return (uint32_t)d3d_gpu_pal_shades() * 256u;
}

static inline void d3d_gpu_shade_cache_clear(void)
{
    memset(gpu_shade_cache, 0, sizeof(gpu_shade_cache));
}

static inline uint32_t d3d_gpu_shade_cache_index(const uint8_t *row)
{
    return (uint32_t)(((uintptr_t)row >> 8) & (D3D_GPU_SHADE_CACHE_SIZE - 1u));
}

static inline void d3d_gpu_shade_cache_store(const uint8_t *row,
                                             int shade, int slot)
{
    if (!row || shade < 0 || slot < 0)
        return;
    d3d_gpu_shade_cache_entry_t *e =
        &gpu_shade_cache[d3d_gpu_shade_cache_index(row)];
    e->row = row;
    e->shade = (uint8_t)shade;
    e->slot = (uint8_t)slot;
}

static inline int d3d_gpu_shade_cache_lookup(const uint8_t *row, int *slot_out)
{
    if (!row)
        return -1;

    const d3d_gpu_shade_cache_entry_t *e =
        &gpu_shade_cache[d3d_gpu_shade_cache_index(row)];
    if (e->row != row)
        return -1;

    int slot = (int)e->slot;
    if (slot < 0 || slot >= gpu_pal_next_slot)
        return -1;

    const uint8_t *base = gpu_pal_base_of_slot[slot];
    if (!base)
        return -1;

    ptrdiff_t off = row - base;
    if ((uintptr_t)off >= d3d_gpu_pal_bytes())
        return -1;

    gpu_current_slot = slot;
    if (slot_out)
        *slot_out = slot;
    return (int)e->shade;
}

enum {
    PERF_PATH_VLINE = 0,
    PERF_PATH_MVLINE,
    PERF_PATH_VLINE4,
    PERF_PATH_MVLINE4,
    PERF_PATH_HLINE,
    PERF_PATH_MHLINE,
    PERF_PATH_THLINE,
    PERF_PATH_TVLINE,
    PERF_PATH_TVLINE2,
    PERF_PATH_RHLINE,
    PERF_PATH_RMHLINE,
    PERF_PATH_SPRITE,
    PERF_PATH_MIRROR,
    PERF_PATH_COUNT
};

typedef struct d3d_gpu_perf_s {
    uint32_t frames;

    uint64_t frame_period_us;
    uint64_t render_us;
    uint64_t page_us;
    uint64_t wait_flip_us;
    uint64_t drain_batch_us;
    uint64_t flip_emit_us;
    uint64_t acquire_us;
    uint64_t audio_us;
    uint32_t max_frame_period_us;
    uint32_t max_render_us;
    uint32_t max_page_us;
    uint32_t max_wait_flip_us;
    uint32_t max_drain_batch_us;
    uint32_t max_flip_emit_us;
    uint32_t max_acquire_us;
    uint32_t max_audio_us;

    uint32_t spans;
    uint32_t span_pixels;
    uint32_t masked_spans;
    uint32_t translucent_spans;
    uint32_t skipped_spans;
    uint32_t direct_spans;
    uint32_t direct_pixels;

    uint32_t batches;
    uint32_t flushes;
    uint32_t max_batch;
    uint64_t batch_submit_us;
    uint32_t max_batch_submit_us;

    uint32_t clear_rects;
    uint32_t clear_pixels;
    uint64_t clear_emit_us;
    uint32_t max_clear_emit_us;

    uint32_t setfb_calls;
    uint32_t tex_flushes;
    uint64_t setfb_us;
    uint32_t max_setfb_us;

    uint32_t pal_uploads;
    uint32_t pal_switches;
    uint32_t pal_misses;
    uint64_t pal_upload_us;
    uint64_t pal_switch_us;
    uint32_t max_pal_upload_us;
    uint32_t max_pal_switch_us;

    uint32_t cpu_fallbacks;
    uint32_t cpu_syncs;
    uint32_t finish_calls;
    uint64_t cpu_sync_us;
    uint64_t finish_us;
    uint32_t max_cpu_sync_us;
    uint32_t max_finish_us;

    uint64_t direct_submit_us;
    uint32_t max_direct_submit_us;

    uint64_t tex_reqs;
    uint64_t tex_misses;
    uint32_t max_tex_reqs;
    uint32_t max_tex_misses;
    uint32_t dma_waits;
    uint32_t dma_spin_iters;
    uint32_t ring_waits;
    uint32_t ring_spin_iters;
    uint32_t ring_min_free;
    uint32_t last_ring_free;
    uint32_t last_gpu_status;
    uint32_t last_ring_rdptr;
    uint32_t last_ring_wrptr;
    uint64_t stall_cycles[D3D_GPU_STALL_COUNT];
    uint32_t max_stall_cycles[D3D_GPU_STALL_COUNT];

    uint32_t phase_calls[D3D_GPU_PERF_PHASE_COUNT];
    uint64_t phase_us[D3D_GPU_PERF_PHASE_COUNT];
    uint32_t phase_max_us[D3D_GPU_PERF_PHASE_COUNT];

    uint32_t zone_calls[D3D_GPU_PERF_ZONE_COUNT];
    uint64_t zone_us[D3D_GPU_PERF_ZONE_COUNT];
    uint32_t zone_max_us[D3D_GPU_PERF_ZONE_COUNT];

    uint32_t path_calls[PERF_PATH_COUNT];
    uint32_t path_spans[PERF_PATH_COUNT];
    uint32_t path_pixels[PERF_PATH_COUNT];
    uint64_t path_us[PERF_PATH_COUNT];
    uint32_t path_max_us[PERF_PATH_COUNT];

    uint32_t tile_loads;
    uint32_t tile_load_bytes;
    uint64_t tile_load_us;
    uint32_t max_tile_load_us;
    uint32_t max_tile_load_id;
    uint32_t max_tile_load_bytes;
    uint32_t move_steps;
    uint32_t max_move_steps;
    uint32_t max_move_backlog;
    uint32_t max_move_remaining;
} d3d_gpu_perf_t;

static d3d_gpu_perf_t gpu_perf;
static uint32_t gpu_perf_last_report_ms;
#if D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS
static uint32_t gpu_perf_hw_prev_tex_req;
static uint32_t gpu_perf_hw_prev_tex_miss;
static uint32_t gpu_perf_hw_prev_stall[D3D_GPU_STALL_COUNT];
#endif
static int gpu_perf_hw_valid;
volatile d3d_gpu_perf_capture_t d3d_gpu_perf_latest;
volatile d3d_gpu_perf_capture_t d3d_gpu_perf_worst;

static inline void perf_add_time(uint64_t *sum, uint32_t *maxv, uint32_t us)
{
    *sum += us;
    if (us > *maxv)
        *maxv = us;
}

static inline uint32_t perf_avg(uint64_t sum, uint32_t count)
{
    return count ? (uint32_t)(sum / count) : 0;
}

#if D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS
static inline uint32_t perf_gpu_tex_counter_delta(uint32_t now, uint32_t prev)
{
    return (now - prev) & OF_GPU_TEX_DBG_COUNTER_MASK;
}
#endif

static inline void perf_reset_interval(void)
{
    memset(&gpu_perf, 0, sizeof(gpu_perf));
    gpu_perf.ring_min_free = OF_GPU_RING_SIZE;
}

static inline void perf_sample_gpu_hw(void)
{
    if (!d3d_gpu_present)
        return;

    of_gpu_debug_snapshot_t snap;
    of_gpu_debug_snapshot(&snap, 1);

#if D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS
    if (!gpu_perf_hw_valid) {
        gpu_perf_hw_prev_tex_req = snap.tex_req_count;
        gpu_perf_hw_prev_tex_miss = snap.tex_miss_count;
        for (int i = 0; i < D3D_GPU_STALL_COUNT; i++)
            gpu_perf_hw_prev_stall[i] = snap.stall_count[i];
        gpu_perf_hw_valid = 1;
    } else {
        uint32_t req_delta =
            perf_gpu_tex_counter_delta(snap.tex_req_count,
                                       gpu_perf_hw_prev_tex_req);
        uint32_t miss_delta =
            perf_gpu_tex_counter_delta(snap.tex_miss_count,
                                       gpu_perf_hw_prev_tex_miss);
        gpu_perf_hw_prev_tex_req = snap.tex_req_count;
        gpu_perf_hw_prev_tex_miss = snap.tex_miss_count;

        gpu_perf.tex_reqs += req_delta;
        gpu_perf.tex_misses += miss_delta;
        if (req_delta > gpu_perf.max_tex_reqs)
            gpu_perf.max_tex_reqs = req_delta;
        if (miss_delta > gpu_perf.max_tex_misses)
            gpu_perf.max_tex_misses = miss_delta;

        for (int i = 0; i < D3D_GPU_STALL_COUNT; i++) {
            uint32_t stall_delta =
                snap.stall_count[i] - gpu_perf_hw_prev_stall[i];
            gpu_perf_hw_prev_stall[i] = snap.stall_count[i];
            gpu_perf.stall_cycles[i] += stall_delta;
            if (stall_delta > gpu_perf.max_stall_cycles[i])
                gpu_perf.max_stall_cycles[i] = stall_delta;
        }
    }
#else
    gpu_perf_hw_valid = 0;
#endif

    gpu_perf.dma_waits += snap.dma_waits;
    gpu_perf.dma_spin_iters += snap.dma_spin_iters;
    gpu_perf.ring_waits += snap.ring_waits;
    gpu_perf.ring_spin_iters += snap.ring_spin_iters;
    if (snap.min_ring_free < gpu_perf.ring_min_free)
        gpu_perf.ring_min_free = snap.min_ring_free;
    gpu_perf.last_ring_free = snap.ring_free;
    gpu_perf.last_gpu_status = snap.status;
    gpu_perf.last_ring_rdptr = snap.rdptr;
    gpu_perf.last_ring_wrptr = snap.wrptr;
}

static void perf_capture_interval(uint32_t elapsed_ms)
{
    uint32_t frames = gpu_perf.frames ? gpu_perf.frames : 1;
    d3d_gpu_perf_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    cap.valid = 1;
    cap.seq = d3d_gpu_perf_latest.seq + 1;
    cap.elapsed_ms = elapsed_ms;
    cap.frames = frames;
    cap.render_avg_us = perf_avg(gpu_perf.render_us, frames);
    cap.render_max_us = gpu_perf.max_render_us;
    cap.page_avg_us = perf_avg(gpu_perf.page_us, frames);
    cap.page_max_us = gpu_perf.max_page_us;
    cap.wait_avg_us = perf_avg(gpu_perf.wait_flip_us, frames);
    cap.wait_max_us = gpu_perf.max_wait_flip_us;
    cap.acquire_avg_us = perf_avg(gpu_perf.acquire_us, frames);
    cap.acquire_max_us = gpu_perf.max_acquire_us;
    cap.finish_avg_us = perf_avg(gpu_perf.finish_us, gpu_perf.finish_calls);
    cap.finish_max_us = gpu_perf.max_finish_us;
    cap.sync_avg_us = perf_avg(gpu_perf.cpu_sync_us, gpu_perf.cpu_syncs);
    cap.sync_max_us = gpu_perf.max_cpu_sync_us;
    cap.batches = gpu_perf.batches;
    cap.max_batch = gpu_perf.max_batch;
    cap.submit_avg_us = perf_avg(gpu_perf.batch_submit_us, gpu_perf.batches);
    cap.submit_max_us = gpu_perf.max_batch_submit_us;
    cap.spans = gpu_perf.spans;
    cap.span_pixels = gpu_perf.span_pixels;
    cap.vline_spans = gpu_perf.path_spans[PERF_PATH_VLINE];
    cap.vline_pixels = gpu_perf.path_pixels[PERF_PATH_VLINE];
    cap.mvline_spans = gpu_perf.path_spans[PERF_PATH_MVLINE];
    cap.mvline_pixels = gpu_perf.path_pixels[PERF_PATH_MVLINE];
    cap.vline4_spans = gpu_perf.path_spans[PERF_PATH_VLINE4];
    cap.vline4_pixels = gpu_perf.path_pixels[PERF_PATH_VLINE4];
    cap.mvline4_spans = gpu_perf.path_spans[PERF_PATH_MVLINE4];
    cap.mvline4_pixels = gpu_perf.path_pixels[PERF_PATH_MVLINE4];
    cap.hline_calls = gpu_perf.path_calls[PERF_PATH_HLINE];
    cap.hline_pixels = gpu_perf.path_pixels[PERF_PATH_HLINE];
    cap.sprite_spans = gpu_perf.path_spans[PERF_PATH_SPRITE];
    cap.sprite_pixels = gpu_perf.path_pixels[PERF_PATH_SPRITE];
    cap.tile_loads = gpu_perf.tile_loads;
    cap.tile_bytes = gpu_perf.tile_load_bytes;
    cap.tile_load_avg_us =
        perf_avg(gpu_perf.tile_load_us, gpu_perf.tile_loads);
    cap.tile_load_max_us = gpu_perf.max_tile_load_us;
    cap.tile_load_max_id = gpu_perf.max_tile_load_id;
    cap.tile_load_max_bytes = gpu_perf.max_tile_load_bytes;
    cap.move_steps = gpu_perf.move_steps;
    cap.move_steps_max = gpu_perf.max_move_steps;
    cap.move_backlog_max = gpu_perf.max_move_backlog;
    cap.move_remaining_max = gpu_perf.max_move_remaining;
    cap.tex_req_avg = perf_avg(gpu_perf.tex_reqs, frames);
    cap.tex_miss_avg = perf_avg(gpu_perf.tex_misses, frames);
    cap.tex_req_max = gpu_perf.max_tex_reqs;
    cap.tex_miss_max = gpu_perf.max_tex_misses;
    cap.tex_miss_permille = gpu_perf.tex_reqs ?
        (uint32_t)((gpu_perf.tex_misses * 1000u) / gpu_perf.tex_reqs) : 0;
    cap.dma_waits = gpu_perf.dma_waits;
    cap.dma_spin_iters = gpu_perf.dma_spin_iters;
    cap.ring_waits = gpu_perf.ring_waits;
    cap.ring_spin_iters = gpu_perf.ring_spin_iters;
    cap.ring_min_free = gpu_perf.ring_min_free;
    cap.ring_last_free = gpu_perf.last_ring_free;
    cap.gpu_status = gpu_perf.last_gpu_status;
    for (int i = 0; i < D3D_GPU_STALL_COUNT; i++) {
        cap.stall_avg[i] = perf_avg(gpu_perf.stall_cycles[i], frames);
        cap.stall_max[i] = gpu_perf.max_stall_cycles[i];
    }

    for (int i = 0; i < D3D_GPU_PERF_PHASE_COUNT; i++) {
        cap.phase_avg_us[i] =
            perf_avg(gpu_perf.phase_us[i], gpu_perf.phase_calls[i]);
        cap.phase_max_us[i] = gpu_perf.phase_max_us[i];
    }
    for (int i = 0; i < D3D_GPU_PERF_ZONE_COUNT; i++) {
        cap.zone_avg_us[i] =
            perf_avg(gpu_perf.zone_us[i], gpu_perf.zone_calls[i]);
        cap.zone_max_us[i] = gpu_perf.zone_max_us[i];
    }

    d3d_gpu_perf_latest = cap;
    if (!d3d_gpu_perf_worst.valid ||
        cap.render_max_us > d3d_gpu_perf_worst.render_max_us)
        d3d_gpu_perf_worst = cap;
}

void d3d_gpu_perf_discard_interval(void)
{
    if (!d3d_gpu_perf_enable)
        return;

    if (d3d_gpu_present) {
        of_gpu_debug_snapshot_t snap;
        of_gpu_debug_snapshot(&snap, 1);
#if D3D_GPU_HAVE_HW_TEX_STALL_COUNTERS
        gpu_perf_hw_prev_tex_req = snap.tex_req_count;
        gpu_perf_hw_prev_tex_miss = snap.tex_miss_count;
        for (int i = 0; i < D3D_GPU_STALL_COUNT; i++)
            gpu_perf_hw_prev_stall[i] = snap.stall_count[i];
        gpu_perf_hw_valid = 1;
#else
        (void)snap;
        gpu_perf_hw_valid = 0;
#endif
    } else {
        gpu_perf_hw_valid = 0;
    }

    perf_reset_interval();
    gpu_perf_last_report_ms = of_time_ms();
}

void d3d_gpu_perf_capture_pending(void)
{
    if (!d3d_gpu_perf_enable)
        return;

    if (gpu_perf.frames) {
        uint32_t now_ms = of_time_ms();
        uint32_t elapsed_ms = gpu_perf_last_report_ms ?
            (now_ms - gpu_perf_last_report_ms) : 0;
        perf_capture_interval(elapsed_ms);
    }

    d3d_gpu_perf_discard_interval();
}

void d3d_gpu_perf_dump(void)
{
    d3d_gpu_perf_capture_pending();

    d3d_gpu_perf_capture_t cap = d3d_gpu_perf_worst.valid ?
        d3d_gpu_perf_worst : d3d_gpu_perf_latest;
    if (!cap.valid)
        return;

    printf("[perf-worst] seq=%u n=%u ms=%u render=%u/%u page=%u/%u "
           "wait=%u/%u acq=%u/%u fin=%u/%u sync=%u/%u batch=%u/%u "
           "submit=%u/%u span=%u pix=%u "
           "v=%u/%u mv=%u/%u v4=%u/%u "
           "mv4=%u/%u h=%u/%u spr=%u/%u tex=%u/%u max=%u/%u "
           "miss=%u.%u%% tile=%u/%u/%u/%u/%u/%u mstep=%u/%u/%u/%u "
           "dma=%u/%u ring=%u/%u "
           "free=%u/%u st=%08x\n",
           cap.seq,
           cap.frames,
           cap.elapsed_ms,
           cap.render_avg_us,
           cap.render_max_us,
           cap.page_avg_us,
           cap.page_max_us,
           cap.wait_avg_us,
           cap.wait_max_us,
           cap.acquire_avg_us,
           cap.acquire_max_us,
           cap.finish_avg_us,
           cap.finish_max_us,
           cap.sync_avg_us,
           cap.sync_max_us,
           cap.batches,
           cap.max_batch,
           cap.submit_avg_us,
           cap.submit_max_us,
           cap.spans,
           cap.span_pixels,
           cap.vline_spans,
           cap.vline_pixels,
           cap.mvline_spans,
           cap.mvline_pixels,
           cap.vline4_spans,
           cap.vline4_pixels,
           cap.mvline4_spans,
           cap.mvline4_pixels,
           cap.hline_calls,
           cap.hline_pixels,
           cap.sprite_spans,
           cap.sprite_pixels,
           cap.tex_req_avg,
           cap.tex_miss_avg,
           cap.tex_req_max,
           cap.tex_miss_max,
           cap.tex_miss_permille / 10u,
           cap.tex_miss_permille % 10u,
           cap.tile_loads,
           cap.tile_bytes,
           cap.tile_load_avg_us,
           cap.tile_load_max_us,
           cap.tile_load_max_id,
           cap.tile_load_max_bytes,
           cap.move_steps,
           cap.move_steps_max,
           cap.move_backlog_max,
           cap.move_remaining_max,
           cap.dma_waits,
           cap.dma_spin_iters,
           cap.ring_waits,
           cap.ring_spin_iters,
           cap.ring_min_free,
           cap.ring_last_free,
           cap.gpu_status);

    printf("[perf-stall] cyc tex=%u/%u cmap=%u/%u cmapi=%u/%u "
           "fbss=%u/%u fbwr=%u/%u infl=%u/%u persp=%u/%u\n",
           cap.stall_avg[D3D_GPU_STALL_TEX_WAIT],
           cap.stall_max[D3D_GPU_STALL_TEX_WAIT],
           cap.stall_avg[D3D_GPU_STALL_CMAP_WAIT],
           cap.stall_max[D3D_GPU_STALL_CMAP_WAIT],
           cap.stall_avg[D3D_GPU_STALL_CMAP_ISSUE],
           cap.stall_max[D3D_GPU_STALL_CMAP_ISSUE],
           cap.stall_avg[D3D_GPU_STALL_FBSS_BUSY],
           cap.stall_max[D3D_GPU_STALL_FBSS_BUSY],
           cap.stall_avg[D3D_GPU_STALL_FB_WRITE],
           cap.stall_max[D3D_GPU_STALL_FB_WRITE],
           cap.stall_avg[D3D_GPU_STALL_INFLIGHT],
           cap.stall_max[D3D_GPU_STALL_INFLIGHT],
           cap.stall_avg[D3D_GPU_STALL_PERSP_WAIT],
           cap.stall_max[D3D_GPU_STALL_PERSP_WAIT]);

    printf("[perf-wzone] move=%u/%u misc=%u/%u disp=%u/%u "
           "rooms=%u/%u masks=%u/%u rest=%u/%u post=%u/%u "
           "ceil=%u/%u flor=%u/%u wall=%u/%u mscan=%u/%u "
           "tscan=%u/%u dmwall=%u/%u spr=%u/%u rot=%u/%u\n",
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_MOVELOOP],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_MOVELOOP],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_LOOP_MISC],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_LOOP_MISC],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DISPLAYROOMS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DISPLAYROOMS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DRAWROOMS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DRAWROOMS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DRAWMASKS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DRAWMASKS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DISPLAYREST],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DISPLAYREST],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_POSTREST],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_POSTREST],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_CEILSCAN],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_CEILSCAN],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_FLORSCAN],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_FLORSCAN],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_WALLSCAN],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_WALLSCAN],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_MASKWALLSCAN],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_MASKWALLSCAN],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_TRANSMASKWALLSCAN],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_TRANSMASKWALLSCAN],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_DRAWMASKWALL],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_DRAWMASKWALL],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_DRAWSPRITE],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_DRAWSPRITE],
           cap.zone_avg_us[D3D_GPU_PERF_ZONE_DOROTATESPRITE],
           cap.zone_max_us[D3D_GPU_PERF_ZONE_DOROTATESPRITE]);

    printf("[perf-game] pkt=%u/%u dom=%u/%u setup=%u/%u input=%u/%u "
           "fta=%u/%u weap=%u/%u trans=%u/%u ply=%u/%u fall=%u/%u "
           "expl=%u/%u act=%u/%u eff=%u/%u stand=%u/%u anim=%u/%u "
           "fx=%u/%u fake=%u/%u cyc=%u/%u\n",
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_MOVE_PACKETS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_MOVE_PACKETS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_TOTAL],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_TOTAL],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_SETUP],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_SETUP],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_INPUT],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_INPUT],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_FTA],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_FTA],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_WEAPONS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_WEAPONS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_TRANSPORTS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_TRANSPORTS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_PLAYERS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_PLAYERS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_FALLERS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_FALLERS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_EXPLOSIONS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_EXPLOSIONS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_ACTORS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_ACTORS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_EFFECTORS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_EFFECTORS],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_STANDABLES],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_STANDABLES],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_ANIM],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_ANIM],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_FX],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_FX],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_FAKE],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_FAKE],
           cap.phase_avg_us[D3D_GPU_PERF_PHASE_DOMOVE_CYCLERS],
           cap.phase_max_us[D3D_GPU_PERF_PHASE_DOMOVE_CYCLERS]);

    /* Only when it has actually happened — a silent counter would let a
     * frame-dropping stall pass for a normal slow frame.  Lifetime
     * totals, not per-interval: these are rare events. */
    if (gpu_emit_stall_events)
        printf("[perf-emit] ring stalls=%u dropped_lanes=%u\n",
               gpu_emit_stall_events, gpu_emit_dropped_lanes);
}

static inline uint32_t perf_dt_us(uint32_t t0)
{
    return of_time_us() - t0;
}

static inline void perf_note_path_time(int path, uint32_t t0);
static inline void d3d_gpu_emit_span_encoded(int path,
                                             uint32_t fb_addr,
                                             uint32_t tex_addr,
                                             int32_t s, int32_t t,
                                             int32_t sstep, int32_t tstep,
                                             uint16_t count, uint8_t light,
                                             uint8_t flags,
                                             uint8_t colormap_id,
                                             int16_t fb_stride,
                                             uint16_t tex_width,
                                             uint16_t tex_w_mask,
                                             uint16_t tex_h_mask);

void d3d_gpu_perf_note_cpu_fallback(void)
{
    if (!d3d_gpu_perf_enable) return;
    gpu_perf.cpu_fallbacks++;
}

void d3d_gpu_perf_note_tile_load(uint32_t tile_id, uint32_t bytes,
                                 uint32_t us)
{
    if (!d3d_gpu_perf_enable) return;
    gpu_perf.tile_loads++;
    gpu_perf.tile_load_bytes += bytes;
    perf_add_time(&gpu_perf.tile_load_us, &gpu_perf.max_tile_load_us, us);
    if (us == gpu_perf.max_tile_load_us) {
        gpu_perf.max_tile_load_id = tile_id;
        gpu_perf.max_tile_load_bytes = bytes;
    }
}

void d3d_gpu_perf_note_moveloop(uint32_t steps, uint32_t backlog,
                                uint32_t remaining)
{
    if (!d3d_gpu_perf_enable) return;
    gpu_perf.move_steps += steps;
    if (steps > gpu_perf.max_move_steps)
        gpu_perf.max_move_steps = steps;
    if (backlog > gpu_perf.max_move_backlog)
        gpu_perf.max_move_backlog = backlog;
    if (remaining > gpu_perf.max_move_remaining)
        gpu_perf.max_move_remaining = remaining;
}

void d3d_gpu_perf_note_phase(int phase, uint32_t us)
{
    if (!d3d_gpu_perf_enable) return;
    if (phase < 0 || phase >= D3D_GPU_PERF_PHASE_COUNT) return;
    gpu_perf.phase_calls[phase]++;
    perf_add_time(&gpu_perf.phase_us[phase],
                  &gpu_perf.phase_max_us[phase], us);
}

void d3d_gpu_perf_note_zone(int zone, uint32_t us)
{
    if (!d3d_gpu_perf_enable || !d3d_gpu_perf_deep_enable) return;
    if (zone < 0 || zone >= D3D_GPU_PERF_ZONE_COUNT) return;
    gpu_perf.zone_calls[zone]++;
    perf_add_time(&gpu_perf.zone_us[zone],
                  &gpu_perf.zone_max_us[zone], us);
}

void d3d_gpu_perf_report_frame(uint32_t frame_period_us,
                               uint32_t render_us,
                               uint32_t page_us,
                               uint32_t wait_flip_us,
                               uint32_t drain_batch_us,
                               uint32_t flip_emit_us,
                               uint32_t acquire_us,
                               uint32_t audio_us)
{
    if (!d3d_gpu_perf_enable) return;

    uint32_t now_ms = of_time_ms();
    if (gpu_perf_last_report_ms == 0)
        gpu_perf_last_report_ms = now_ms;

    gpu_perf.frames++;
    perf_sample_gpu_hw();
    perf_add_time(&gpu_perf.frame_period_us, &gpu_perf.max_frame_period_us,
                  frame_period_us);
    perf_add_time(&gpu_perf.render_us, &gpu_perf.max_render_us, render_us);
    perf_add_time(&gpu_perf.page_us, &gpu_perf.max_page_us, page_us);
    perf_add_time(&gpu_perf.wait_flip_us, &gpu_perf.max_wait_flip_us,
                  wait_flip_us);
    perf_add_time(&gpu_perf.drain_batch_us, &gpu_perf.max_drain_batch_us,
                  drain_batch_us);
    perf_add_time(&gpu_perf.flip_emit_us, &gpu_perf.max_flip_emit_us,
                  flip_emit_us);
    perf_add_time(&gpu_perf.acquire_us, &gpu_perf.max_acquire_us, acquire_us);
    perf_add_time(&gpu_perf.audio_us, &gpu_perf.max_audio_us, audio_us);

    uint32_t elapsed_ms = now_ms - gpu_perf_last_report_ms;
    if (elapsed_ms < 3000)
        return;

    perf_capture_interval(elapsed_ms);
    perf_reset_interval();
    gpu_perf_last_report_ms = now_ms;
}

/* Verify CMD_DRAW_COLUMN_LIST (0x4C) end-to-end before trusting the
 * caps bit: a bitstream that advertises OF_HW_GPU_COLUMN_LIST but
 * predates the decode path drains the command as a no-op, which this
 * catches (the probe pixel stays 0).  Same probe as the Doom port's
 * r_gpu.c.  Runs once at init, before BUILD sets a real framebuffer —
 * every draw path gates on fb_base, so the probe's SET_FB state can't
 * leak into a frame. */
static uint8_t gpu_probe_fb[64] __attribute__((aligned(64)));
static uint8_t gpu_probe_tex[64] __attribute__((aligned(64)));

static int d3d_gpu_probe_column_list(void)
{
    of_gpu_column_list_group_t group;

    memset(gpu_probe_fb, 0, sizeof(gpu_probe_fb));
    memset(gpu_probe_tex, 0, sizeof(gpu_probe_tex));
    gpu_probe_tex[0] = 0xa5;

    of_cache_flush_range(gpu_probe_fb, sizeof(gpu_probe_fb));
    of_cache_flush_range(gpu_probe_tex, sizeof(gpu_probe_tex));
    GPU_TEX_FLUSH = 1;

    of_gpu_set_framebuffer((uint32_t)(uintptr_t)gpu_probe_fb, 8);

    memset(&group, 0, sizeof(group));
    group.lane_count = 1;
    group.tex_width = 1;
    group.fb_step = 1;
    group.fb_addr[0] = (uint32_t)(uintptr_t)gpu_probe_fb;
    group.tex_addr[0] = (uint32_t)(uintptr_t)gpu_probe_tex;
    group.count[0] = 1;
    group.colormap_id[0] = 0;

    of_gpu_draw_column_list(&group);
    of_gpu_finish();
    of_cache_inval_range(gpu_probe_fb, sizeof(gpu_probe_fb));

    return gpu_probe_fb[0] == 0xa5;
}

void d3d_gpu_init(void)
{
    perf_reset_interval();
    gpu_perf_hw_valid = 0;
    gpu_perf_last_report_ms = of_time_ms();

    /* Leave d3d_gpu_present at 0 so every downstream helper
     * short-circuits and BUILD's CPU loops run. */
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

    if ((caps->hw_features & D3D_GPU_REQUIRED_FEATURES) !=
        D3D_GPU_REQUIRED_FEATURES) {
        printf("[d3d_gpu] incompatible GPU features=0x%08x need=0x%08x — SW renderer\n",
               (unsigned)caps->hw_features,
               (unsigned)D3D_GPU_REQUIRED_FEATURES);
        return;
    }
    of_gpu_init();
    d3d_gpu_present = 1;
    printf("[d3d_gpu] GPU init ok (base=0x%08x features=0x%08x)\n",
           (unsigned)caps->gpu_base, (unsigned)caps->hw_features);

    /* Wall/sprite columns ride CMD_DRAW_COLUMN_LIST (0x4C) when the
     * bitstream has it; fallback is the 0x48 affine span-group path,
     * which stays selectable via d3d_gpu_force_affine_columns. */
    gpu_use_column_list = 0;
    if (of_has_feature(OF_HW_GPU_COLUMN_LIST) && d3d_gpu_probe_column_list())
        gpu_use_column_list = 1;
    printf("[d3d_gpu] column list (0x4C): %s\n",
           gpu_use_column_list ? "enabled" : "unavailable, using 0x48");

    if (pending_transluc_table && pending_transluc_size == 65536)
        d3d_gpu_upload_transluc(pending_transluc_table, pending_transluc_size);
}

/* Native affine span submission via the SDK helper.
 *
 * Duke/BUILD already computes exact spans on the CPU.  The GPU side should
 * only consume compact affine span groups with explicit per-lane colormap
 * slots; the SDK lowers these groups to GPU_CMD_DRAW_PARAM_SPAN_LIST. */
static of_gpu_affine_span_group_t affine_batch;
static int affine_batch_count;

/* CMD_DRAW_COLUMN_LIST (0x4C) batch: 5-word lane records for vertical
 * 1-pixel-wide columns — drops the always-zero s/sstep words the 0x48
 * affine lane form carries, ~28% less command traffic on BUILD's
 * dominant draw type (wall + masked/sprite columns).  Pixels are
 * byte-identical to the affine group with s=0/sstep=0 by hardware
 * contract, so SKIP_ZERO color-key and TRANSLUC blending behave
 * identically.
 *
 * INVARIANT (same as the Doom port's r_gpu.c): at most one of
 * affine_batch / column_batch is non-empty at any time, so painter's
 * order between spans and columns is preserved. */
static of_gpu_column_list_group_t column_batch;
static int column_batch_count;

static inline int d3d_gpu_affine_batch_compatible(uint8_t flags,
                                                  int32_t fb_step,
                                                  uint16_t tex_width,
                                                  uint16_t tex_w_mask,
                                                  uint16_t tex_h_mask)
{
    return affine_batch_count > 0 &&
           affine_batch_count < (int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES &&
           affine_batch.flags == flags &&
           affine_batch.fb_step == fb_step &&
           affine_batch.tex_width == tex_width &&
           affine_batch.tex_w_mask == tex_w_mask &&
           affine_batch.tex_h_mask == tex_h_mask;
}

static void d3d_gpu_flush_affine_batch(void)
{
    if (affine_batch_count <= 0)
        return;

    SPIN_TAG(5);
    affine_batch.lane_count = (uint8_t)affine_batch_count;

    /* Probe before committing — see the gpu_emit_stalled notes above. */
    if (gpu_emit_stalled ||
        !d3d_gpu_reserve_or_drop(
            d3d_gpu_batch_bytes((uint32_t)affine_batch_count,
                                OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES,
                                OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS,
                                OF_GPU_PARAM_DIRECT_AFFINE_WORDS(0)))) {
        gpu_emit_dropped_lanes += (uint32_t)affine_batch_count;
        affine_batch_count = 0;
        SPIN_TAG(0);
        return;
    }

    if (d3d_gpu_perf_enable) {
        gpu_perf.flushes++;
        gpu_perf.batches++;
        if ((uint32_t)affine_batch_count > gpu_perf.max_batch)
            gpu_perf.max_batch = (uint32_t)affine_batch_count;
    }

    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_draw_affine_span_group(&affine_batch);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.batch_submit_us,
                      &gpu_perf.max_batch_submit_us, dt);
    }

    affine_batch_count = 0;
    /* Advisory: publishes only past the SDK's lazy-kick threshold, so
     * the GPU starts chewing on staged work mid-frame instead of
     * idling until the staging buffer fills or a sync point hits. */
    of_gpu_kick();
    SPIN_TAG(0);
}

static void d3d_gpu_flush_column_batch(void)
{
    if (column_batch_count <= 0)
        return;

    SPIN_TAG(5);
    column_batch.lane_count = (uint8_t)column_batch_count;

    if (gpu_emit_stalled ||
        !d3d_gpu_reserve_or_drop(
            d3d_gpu_batch_bytes((uint32_t)column_batch_count,
                                OF_GPU_COLUMN_LIST_MAX_NATIVE_LANES,
                                OF_GPU_COLUMN_LIST_LANE_WORDS,
                                OF_GPU_COLUMN_LIST_WORDS(0)))) {
        gpu_emit_dropped_lanes += (uint32_t)column_batch_count;
        column_batch_count = 0;
        SPIN_TAG(0);
        return;
    }

    if (d3d_gpu_perf_enable) {
        gpu_perf.flushes++;
        gpu_perf.batches++;
        if ((uint32_t)column_batch_count > gpu_perf.max_batch)
            gpu_perf.max_batch = (uint32_t)column_batch_count;
    }

    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_draw_column_list(&column_batch);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.batch_submit_us,
                      &gpu_perf.max_batch_submit_us, dt);
    }

    column_batch_count = 0;
    of_gpu_kick();
    SPIN_TAG(0);
}

/* Flush ALL staged draw batches.  The queue-side invariant keeps at
 * most one of the two non-empty, so the call order is immaterial. */
static void d3d_gpu_flush_batch(void)
{
    d3d_gpu_flush_affine_batch();
    d3d_gpu_flush_column_batch();
}

static inline void d3d_gpu_barrier_before_fb_read(void)
{
    if (!gpu_fb_read_barrier_needed)
        return;

    /* Translucent spans read the current framebuffer byte before writing the
     * blended result.  If an earlier span batch is still buffered, or older
     * fabric has pending FB writes in flight, the RMW read can sample stale
     * pixels.  Insert a GPU-side fence before framebuffer readback spans.
     */
    d3d_gpu_flush_batch();
    /* 2-word fence, same non-fatal rule as the draw batches: with the
     * ring wedged there is nothing staged worth fencing anyway, since
     * the flush above just dropped it. */
    if (d3d_gpu_reserve_or_drop(2u * 4u)) {
        of_gpu_fence();
        of_gpu_kick();
    }
    gpu_fb_read_barrier_needed = 0;
}

static inline void d3d_gpu_flush_tex_cache_now(void)
{
    if (!d3d_gpu_present)
        return;
    if (d3d_gpu_perf_enable)
        gpu_perf.tex_flushes++;
    GPU_TEX_FLUSH = 1;
}

static void d3d_gpu_finish_for_lookup_update(void)
{
    d3d_gpu_flush_batch();
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_finish();
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        gpu_perf.finish_calls++;
        perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
    }
    gpu_fb_drained_for_cpu = 1;
    cpu_fb_read_coherent = 0;
}

/* Public wrapper around d3d_gpu_flush_batch — called by display_of.c's
 * GPU-triggered flip path before emitting CMD_FLIP. */
void d3d_gpu_drain_batch(void) {
    if (!d3d_gpu_present) return;
    SPIN_TAG(6);
    d3d_gpu_flush_batch();
    SPIN_TAG(0);
}

/* GPU-triggered flip wrapper.  Lives in this TU (the single owner of
 * of_gpu.h's static ring state — _gpu_wrptr, _gpu_fence_next, etc.)
 * so display_of.c can trigger flips without including of_gpu.h itself,
 * which would create a duplicate set of static ring-tracking variables
 * and desync the actual ring writes from the SW-tracked wrptr.  An
 * earlier attempt at calling of_gpu_flip_to() directly from
 * display_of.c hit exactly this: tok=0 from a separate
 * _gpu_fence_next, and worse, of_gpu_kick() writing display_of.c's
 * stale _gpu_wrptr=0xC truncated the ring back to 12 bytes — the GPU
 * lost track of all the BUILD-emitted spans + bar-clear commands. */
uint32_t d3d_gpu_flip_to(int idx) {
    if (!d3d_gpu_present) return 0;
    SPIN_TAG(7);
    d3d_gpu_clean_cpu_fb_before_gpu();

    /* CMD_FLIP is 3 words, but a full ring traps on any emit at all.
     * Token 0 is the documented "nothing to wait for" value — the same
     * one the init path passes — so of_video_acquire_next() will not
     * block on a fence that is never going to be published. */
    uint32_t token = 0;
    if (d3d_gpu_reserve_or_drop(3u * 4u)) {
        token = of_gpu_flip_to(idx);
        of_gpu_kick();
    }

    /* Frame boundary: drop the latch so the next frame re-probes once
     * from scratch.  A GPU that resumed while we were skipping work
     * recovers here without any explicit "menu closed" notification. */
    gpu_emit_stalled = 0;

    SPIN_TAG(0);
    return token;
}

void d3d_gpu_prepare_framebuffer_for_present(void)
{
    d3d_gpu_clean_cpu_fb_before_gpu();
}

/* GPU-routed mirror reverse-blit.  See d3d_gpu.h for rationale.
 *   dst, src   — FB byte addresses (cached alias — GPU's m_rd / m_wr
 *                go directly to SDRAM regardless of CPU-cache alias).
 *   count      — pixels per row.
 *   rows       — number of rows to mirror.
 *   row_stride — bytes between rows (BUILD's bytesperline / ylookup[1]).
 *
 * Per row: dst[k] = src[count-1-k].  Implemented as one affine span
 * per row that walks dst BACKWARDS (fb_stride = -1) while sampling
 * src FORWARD (sstep = +0x10000).  Walking dst backwards instead of
 * sampling src reverse avoids the gpu_core.v `p0_s_int <= sp_s[31:16]
 * & sp_tex_w_mask` step — a negative s_int would alias to 0xFFFF
 * after masking and read 64 KB past the source.  fb_stride is signed
 * 16-bit at the GPU side and the per-pixel update sign-extends, so
 * negative strides work natively.
 *
 * CMD_FENCE first drains in-flight m_wr_* from earlier world spans
 * so tex_cache reads back fresh SDRAM (cr-gpu-fence-write-completion
 * gives us the GPU-side write→read ordering at the command-processor
 * level — no CPU spin involved). */
void d3d_gpu_blit_mirror(uint8_t *dst, const uint8_t *src,
                         int count, int rows, int row_stride) {
    if (!d3d_gpu_present) return;
    if (count <= 0 || rows <= 0) return;

    d3d_gpu_clean_cpu_fb_before_gpu();
    of_gpu_fence();

    /* fb_addr for row 0 starts at dst's LAST pixel (so the backwards
     * walk finishes at dst[0]); tex_addr starts at src's FIRST pixel
     * (forward sample with sstep=+1 reads src[0..count-1]). */
    uint32_t dst_last = (uint32_t)(uintptr_t)(dst + (count - 1));
    uint32_t src_base = (uint32_t)(uintptr_t)src;

    for (int r = 0; r < rows; r++) {
        uint32_t t0 = (d3d_gpu_perf_enable && d3d_gpu_perf_time_paths) ?
            of_time_us() : 0;
        d3d_gpu_emit_span_encoded(PERF_PATH_MIRROR,
                                  dst_last + (uint32_t)(r * row_stride),
                                  src_base + (uint32_t)(r * row_stride),
                                  0, 0, 0x10000, 0,
                                  (uint16_t)count,
                                  0, 0, 0,
                                  -1, 1, 0xFFFF, 0xFFFF);
        if (d3d_gpu_perf_enable) {
            gpu_perf.direct_spans++;
            gpu_perf.direct_pixels += (uint32_t)count;
            if (t0) {
                uint32_t dt = perf_dt_us(t0);
                perf_add_time(&gpu_perf.direct_submit_us,
                              &gpu_perf.max_direct_submit_us, dt);
            }
            perf_note_path_time(PERF_PATH_MIRROR, t0);
        }
    }
    d3d_gpu_flush_batch();
    mark_gpu_fb_dirty();
    of_gpu_kick();
}

static inline uint32_t perf_path_begin(void)
{
    return (d3d_gpu_perf_enable && d3d_gpu_perf_time_paths) ? of_time_us() : 0;
}

static inline void perf_note_path_time(int path, uint32_t t0)
{
    if (!d3d_gpu_perf_enable) return;
    if (path < 0 || path >= PERF_PATH_COUNT) return;
    gpu_perf.path_calls[path]++;
    if (!t0) return;
    perf_add_time(&gpu_perf.path_us[path],
                  &gpu_perf.path_max_us[path], perf_dt_us(t0));
}

static inline void perf_note_path_span_values(int path, uint32_t pixels)
{
    if (!d3d_gpu_perf_enable) return;
    if (path < 0 || path >= PERF_PATH_COUNT) return;
    gpu_perf.path_spans[path]++;
    gpu_perf.path_pixels[path] += pixels;
}

static inline uint8_t current_span_colormap_id(void)
{
    return (gpu_current_slot > 0) ? (uint8_t)(gpu_current_slot & 0xF) : 0;
}

static inline uint8_t span_colormap_id_for_slot(int slot)
{
    return (slot > 0) ? (uint8_t)(slot & 0xF) : 0;
}

static inline void perf_note_span_values(int path, uint16_t count,
                                         uint8_t flags)
{
    if (!d3d_gpu_perf_enable) return;
    gpu_perf.spans++;
    gpu_perf.span_pixels += count;
    if (flags & OF_GPU_SPAN_SKIP_ZERO)
        gpu_perf.masked_spans++;
    if (flags & OF_GPU_SPAN_TRANSLUC)
        gpu_perf.translucent_spans++;
    if (D3D_GPU_SKIP_SUBMIT)
        gpu_perf.skipped_spans++;
    perf_note_path_span_values(path, count);
}

static inline int d3d_gpu_column_batch_compatible(uint8_t flags,
                                                  int32_t fb_step,
                                                  uint16_t tex_width,
                                                  uint16_t tex_w_mask,
                                                  uint16_t tex_h_mask)
{
    return column_batch_count > 0 &&
           column_batch_count < (int)OF_GPU_COLUMN_LIST_MAX_LANES &&
           column_batch.flags == flags &&
           column_batch.fb_step == fb_step &&
           column_batch.tex_width == tex_width &&
           column_batch.tex_w_mask == tex_w_mask &&
           column_batch.tex_h_mask == tex_h_mask;
}

static inline void d3d_gpu_queue_column(uint32_t fb_addr, uint32_t tex_addr,
                                        int32_t t, int32_t tstep,
                                        uint16_t count, uint8_t light,
                                        uint8_t flags, uint8_t colormap_id,
                                        int32_t fb_step, uint16_t tex_width,
                                        uint16_t tex_w_mask,
                                        uint16_t tex_h_mask)
{
    /* Painter's order: staged affine spans must land before this column. */
    if (affine_batch_count != 0)
        d3d_gpu_flush_affine_batch();

    if (!d3d_gpu_column_batch_compatible(flags, fb_step, tex_width,
                                         tex_w_mask, tex_h_mask)) {
        d3d_gpu_flush_column_batch();
        memset(&column_batch, 0, sizeof(column_batch));
        column_batch.flags = flags;
        column_batch.tex_width = tex_width;
        column_batch.tex_w_mask = tex_w_mask;
        column_batch.tex_h_mask = tex_h_mask;
        column_batch.fb_step = fb_step;
    }

    int lane = column_batch_count++;
    column_batch.fb_addr[lane] = fb_addr;
    column_batch.tex_addr[lane] = tex_addr;
    column_batch.count[lane] = count;
    column_batch.t[lane] = t;
    column_batch.tstep[lane] = tstep;
    column_batch.light[lane] = (uint8_t)(light & 0x3F);
    column_batch.colormap_id[lane] = (uint8_t)(colormap_id & 0x0F);

    if (column_batch_count == (int)OF_GPU_COLUMN_LIST_MAX_LANES)
        d3d_gpu_flush_column_batch();
}

static inline void d3d_gpu_queue_affine_span(uint32_t fb_addr,
                                             uint32_t tex_addr,
                                             int32_t s, int32_t t,
                                             int32_t sstep, int32_t tstep,
                                             uint16_t count, uint8_t light,
                                             uint8_t flags,
                                             uint8_t colormap_id,
                                             int32_t fb_step,
                                             uint16_t tex_width,
                                             uint16_t tex_w_mask,
                                             uint16_t tex_h_mask)
{
    /* Vertical columns (s and sstep both 0 — every vline/mvline/vline4/
     * tvline lane) ride the 5-word 0x4C lane form when the bitstream
     * has it; everything else stays on the 7-word 0x48 affine form. */
    if (gpu_use_column_list && !d3d_gpu_force_affine_columns &&
        s == 0 && sstep == 0) {
        d3d_gpu_queue_column(fb_addr, tex_addr, t, tstep, count, light,
                             flags, colormap_id, fb_step,
                             tex_width, tex_w_mask, tex_h_mask);
        return;
    }

    /* Painter's order: staged columns must land before this span. */
    if (column_batch_count != 0)
        d3d_gpu_flush_column_batch();

    if (!d3d_gpu_affine_batch_compatible(flags, fb_step,
                                         tex_width, tex_w_mask,
                                         tex_h_mask)) {
        d3d_gpu_flush_batch();
        memset(&affine_batch, 0, sizeof(affine_batch));
        affine_batch.flags = flags;
        affine_batch.tex_width = tex_width;
        affine_batch.tex_w_mask = tex_w_mask;
        affine_batch.tex_h_mask = tex_h_mask;
        affine_batch.fb_step = fb_step;
    }

    int lane = affine_batch_count++;
    affine_batch.fb_addr[lane] = fb_addr;
    affine_batch.tex_addr[lane] = tex_addr;
    affine_batch.count[lane] = count;
    affine_batch.s[lane] = s;
    affine_batch.t[lane] = t;
    affine_batch.sstep[lane] = sstep;
    affine_batch.tstep[lane] = tstep;
    affine_batch.light[lane] = (uint8_t)(light & 0x3F);
    affine_batch.colormap_id[lane] = (uint8_t)(colormap_id & 0x0F);

    if (affine_batch_count == (int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES)
        d3d_gpu_flush_batch();
}

static inline void d3d_gpu_emit_span_encoded(int path,
                                             uint32_t fb_addr,
                                             uint32_t tex_addr,
                                             int32_t s, int32_t t,
                                             int32_t sstep, int32_t tstep,
                                             uint16_t count, uint8_t light,
                                             uint8_t flags,
                                             uint8_t colormap_id,
                                             int16_t fb_stride,
                                             uint16_t tex_width,
                                             uint16_t tex_w_mask,
                                             uint16_t tex_h_mask)
{
    perf_note_span_values(path, count, flags);
    if (D3D_GPU_SKIP_SUBMIT) return;

    d3d_gpu_clean_cpu_fb_before_gpu();
    if (flags & OF_GPU_SPAN_TRANSLUC)
        d3d_gpu_barrier_before_fb_read();

    d3d_gpu_queue_affine_span(fb_addr, tex_addr, s, t,
                              sstep, tstep, count, light,
                              flags, colormap_id, fb_stride,
                              tex_width, tex_w_mask, tex_h_mask);
    if (flags & OF_GPU_SPAN_TRANSLUC)
        d3d_gpu_flush_batch();
    mark_gpu_fb_dirty();
    gpu_fb_read_barrier_needed = 1;
}

static inline void d3d_gpu_emit_span_group_encoded(int path,
                                                   uint32_t fb_addr,
                                                   uint16_t count,
                                                   uint8_t lane_count,
                                                   int16_t lane_delta,
                                                   uint8_t flags,
                                                   const uint8_t *colormap_id,
                                                   int16_t fb_stride,
                                                   uint16_t tex_width,
                                                   uint16_t tex_w_mask,
                                                   uint16_t tex_h_mask,
                                                   const uint32_t *tex_addr,
                                                   const int32_t *t,
                                                   const int32_t *tstep,
                                                   const uint8_t *light)
{
    if (lane_count > 4)
        lane_count = 4;

    if (!D3D_GPU_SKIP_SUBMIT) {
        d3d_gpu_clean_cpu_fb_before_gpu();
        if (flags & OF_GPU_SPAN_TRANSLUC)
            d3d_gpu_barrier_before_fb_read();
    }

    for (uint8_t lane = 0; lane < lane_count; lane++) {
        perf_note_span_values(path, count, flags);
        if (D3D_GPU_SKIP_SUBMIT)
            continue;

        uint32_t lane_fb_addr =
            fb_addr + (uint32_t)((int32_t)lane_delta * (int32_t)lane);

        d3d_gpu_queue_affine_span(
            lane_fb_addr,
            tex_addr[lane],
            0, t[lane],
            0, tstep[lane],
            count, light[lane],
            flags, colormap_id[lane],
            fb_stride,
            tex_width, tex_w_mask, tex_h_mask);
    }
    if (flags & OF_GPU_SPAN_TRANSLUC)
        d3d_gpu_flush_batch();
    mark_gpu_fb_dirty();
    gpu_fb_read_barrier_needed = 1;
}

void d3d_gpu_set_fb(uint8_t *fb_pixels, int stride_pixels)
{
    d3d_gpu_clean_cpu_fb_before_gpu();

    uint8_t *fb_cached = d3d_gpu_cached_sdram_alias(fb_pixels);

    /* Cache fb_base for the GPU-off CPU memset path in
     * d3d_gpu_clear_rect_fb.  Stride is no longer cached — span
     * emitters read BUILD's bytesperline live so they track
     * setviewtotile's stride flip without a separate hook. */
    fb_base = fb_cached;
    gpu_fb_drained_for_cpu = 0;
    cpu_fb_read_coherent = 0;
    gpu_fb_read_barrier_needed = 0;
    if (!d3d_gpu_present) return;

    if (d3d_gpu_perf_enable)
        gpu_perf.setfb_calls++;
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;

    /* Retargeting is a synchronization point: submit any spans that still
     * belong to the old framebuffer before changing SET_FB state.
     * Texture cache invalidation is driven by tile/palette mutation paths;
     * keep warm texels across ordinary frame retargets. */
    d3d_gpu_flush_batch();

    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb_cached,
                           (uint16_t)stride_pixels);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.setfb_us, &gpu_perf.max_setfb_us, dt);
    }
}

/* Upload the active palookup into GPU SDRAM slot 0.  Duke's runtime
 * numpalookups is the authoritative shade-row count; the fabric can
 * address up to 64 rows × 256 entries in each 16 KB slot. */
void d3d_gpu_upload_palookup(const uint8_t *palookup_table, int num_shades)
{
    if (!d3d_gpu_present || !palookup_table) return;
    if (num_shades <= 0)  num_shades = d3d_gpu_pal_shades();
    if (num_shades > D3D_GPU_MAX_SHADES)  num_shades = D3D_GPU_MAX_SHADES;

    /* Palookup slots are sampled by GPU fragments but updated through
     * CPU-visible SDRAM, not through the command stream.  Retire queued
     * spans before overwriting slot 0. */
    d3d_gpu_finish_for_lookup_update();

    if (d3d_gpu_perf_enable)
        gpu_perf.pal_uploads++;
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_palookup_upload(0, palookup_table, (uint32_t)num_shades * 256u);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.pal_upload_us,
                      &gpu_perf.max_pal_upload_us, dt);
    }
    d3d_gpu_shade_cache_clear();
}

void d3d_gpu_upload_transluc(const uint8_t *table, uint32_t size)
{
    if (!table) return;
    if (size != 65536) return;
    if (!d3d_gpu_translucent_spans_enabled) return;
    if (!d3d_gpu_present) {
        pending_transluc_table = table;
        pending_transluc_size = size;
        return;
    }

    pending_transluc_table = table;
    pending_transluc_size = size;

    /* Duke's CPU TRANS_NORMAL computes:
     *     transluc[(dst << 8) | src]
     * but the fabric blend unit has a fixed 32 KB key:
     *     { src[7:1], dst[7:0] }
     * Upload a transposed, source-decimated table so the normal Duke
     * path stays GPU-accelerated.  TRANS_REVERSE falls back to the CPU
     * because the current hardware no longer has a per-span reverse bit.
     */
    d3d_gpu_finish_for_lookup_update();

    uint32_t wait_start = of_time_us();
    while (GPU_STATUS & GPU_STATUS_TRANSLUC_BUSY) {
        if ((uint32_t)(of_time_us() - wait_start) >
            D3D_GPU_TRANSLUC_WAIT_TIMEOUT_US)
            goto transluc_timeout;
    }
    GPU_TRANSLUC_ADDR = 0;
    for (int s7 = 0; s7 < 128; s7++) {
        int src = s7 << 1;
        for (int d = 0; d < 256; d += 4) {
            uint32_t w =
                ((uint32_t)table[((d + 0) << 8) | src]      ) |
                ((uint32_t)table[((d + 1) << 8) | src] <<  8) |
                ((uint32_t)table[((d + 2) << 8) | src] << 16) |
                ((uint32_t)table[((d + 3) << 8) | src] << 24);
            wait_start = of_time_us();
            while (GPU_STATUS & GPU_STATUS_TRANSLUC_BUSY) {
                if ((uint32_t)(of_time_us() - wait_start) >
                    D3D_GPU_TRANSLUC_WAIT_TIMEOUT_US)
                    goto transluc_timeout;
            }
            GPU_TRANSLUC_DATA = w;
        }
    }
    wait_start = of_time_us();
    while (GPU_STATUS & GPU_STATUS_TRANSLUC_BUSY) {
        if ((uint32_t)(of_time_us() - wait_start) >
            D3D_GPU_TRANSLUC_WAIT_TIMEOUT_US)
            goto transluc_timeout;
    }
    return;

transluc_timeout:
    d3d_gpu_translucent_spans_enabled = 0;
    printf("[d3d_gpu] translucency LUT upload timed out; GPU translucency disabled\n");
}

int d3d_gpu_translucent_spans_ready(void)
{
    return d3d_gpu_translucent_spans_enabled;
}

void d3d_gpu_clear_rect_fb(uint8_t *dest, uint16_t w, uint16_t h, uint8_t color)
{
    if (!dest) return;
    if (w == 0 || h == 0) return;
    SPIN_TAG(8);
    if (!d3d_gpu_present || !d3d_gpu_use_spans) {
        /* CPU memset fallback for GPU-disabled or no-GPU builds.
         * Live bytesperline tracks setviewtotile's stride flip. */
        for (int y = 0; y < (int)h; y++)
            memset(dest + y * bytesperline, color, w);
        SPIN_TAG(0);
        return;
    }
    d3d_gpu_clean_cpu_fb_before_gpu();
    /* Flush any pending spans first so they execute BEFORE the
     * clear_rect.  Without this, the batch header would land in the
     * ring AFTER the CLEAR_RECT command (which is written via direct
     * MMIO ring writes), and the GPU would clear the rect first and
     * then draw the (stale) spans on top — ordering inversion. */
    d3d_gpu_flush_batch();
    mark_gpu_fb_dirty();
    if (d3d_gpu_perf_enable) {
        gpu_perf.clear_rects++;
        gpu_perf.clear_pixels += (uint32_t)w * (uint32_t)h;
    }
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    /* Per-command stride: CMD_CLEAR_RECT now carries its own stride
     * (openfpgaOS commit landing the cr-gpu-clear-rect-stride.md fix),
     * so we don't need to resync the global SET_FB stride before each
     * clear.  This matters because BUILD's setviewtotile flips
     * bytesperline mid-frame between screen stride (320) and tile
     * stride (e.g. 160 in low-detail). */
    /* 4-word CLEAR_RECT — guarded like the draw batches so a frozen
     * ring drops the clear instead of trapping. */
    if (d3d_gpu_reserve_or_drop(4u * 4u)) {
        of_gpu_clear_rect_strided((uint32_t)(uintptr_t)dest,
                                   w, h,
                                   (uint16_t)bytesperline,
                                   color);
        gpu_fb_read_barrier_needed = 1;
    }
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.clear_emit_us,
                      &gpu_perf.max_clear_emit_us, dt);
    }
    SPIN_TAG(0);
}

void d3d_gpu_pre_cpu_fb_access(void)
{
    if (!d3d_gpu_present) return;
    if (cpu_fb_read_coherent) {
        cpu_fb_dirty_for_gpu = 1;
        return;
    }
    uint32_t sync_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    SPIN_TAG(3);
    if (!gpu_fb_drained_for_cpu) {
        /* Flush any pending span batch so its writes land in SDRAM before
         * we wait for the GPU to drain. */
        d3d_gpu_flush_batch();
        /* Drain GPU writes so CPU fallback/readback paths see current
         * framebuffer pixels before touching cached frameplace. */
        SPIN_TAG(4);
        uint32_t finish_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_gpu_prepare_framebuffer_for_cpu();
        if (d3d_gpu_perf_enable) {
            uint32_t dt = perf_dt_us(finish_t0);
            gpu_perf.finish_calls++;
            perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
        }
        gpu_fb_drained_for_cpu = 1;
    }
    d3d_gpu_clean_cpu_fb_range();
    cpu_fb_dirty_for_gpu = 1;
    cpu_fb_read_coherent = 1;
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(sync_t0);
        gpu_perf.cpu_syncs++;
        perf_add_time(&gpu_perf.cpu_sync_us, &gpu_perf.max_cpu_sync_us, dt);
    }
    SPIN_TAG(0);
}

void d3d_gpu_prepare_cpu_fb_write(void)
{
    if (!d3d_gpu_present) return;
    if (gpu_fb_drained_for_cpu) {
        cpu_fb_dirty_for_gpu = 1;
        cpu_fb_read_coherent = 0;
        return;
    }
    uint32_t sync_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    SPIN_TAG(3);
    d3d_gpu_flush_batch();
    SPIN_TAG(4);
    uint32_t finish_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_prepare_framebuffer_for_cpu();
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(finish_t0);
        gpu_perf.finish_calls++;
        perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
    }
    gpu_fb_drained_for_cpu = 1;
    cpu_fb_read_coherent = 0;
    cpu_fb_dirty_for_gpu = 1;
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(sync_t0);
        gpu_perf.cpu_syncs++;
        perf_add_time(&gpu_perf.cpu_sync_us, &gpu_perf.max_cpu_sync_us, dt);
    }
    SPIN_TAG(0);
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
    SPIN_TAG(2);
    d3d_gpu_flush_batch();
    SPIN_TAG(4);
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_finish();
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        gpu_perf.finish_calls++;
        perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
    }
    gpu_fb_drained_for_cpu = 1;
    cpu_fb_read_coherent = 0;
    SPIN_TAG(0);
}

void d3d_gpu_tex_invalidate(void)
{
    d3d_gpu_flush_tex_cache_now();
}

void d3d_gpu_mark_tex_dirty(void)
{
    /* Legacy no-op.  Hot tile paths now drain before mutating BUILD's tile
     * cache and call d3d_gpu_tex_invalidate() once the new bytes are in DRAM. */
}

void d3d_gpu_drain(void)
{
    if (!d3d_gpu_present) return;
    SPIN_TAG(1);
    d3d_gpu_flush_batch();
    SPIN_TAG(4);
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_finish();
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        gpu_perf.finish_calls++;
        perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
    }
    gpu_fb_drained_for_cpu = 1;
    cpu_fb_read_coherent = 0;
    SPIN_TAG(0);
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
 * GPU equivalent uses an affine span group with tex_width=1 so address-gen
 * is `tex_addr + (t >> 16)`.  We renormalise vplce / vince from "texel-
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

static inline uint16_t column_tex_h_mask(uint8_t v_shift)
{
    uint8_t bits = (uint8_t)(32u - (uint32_t)(v_shift & 31u));
    if (bits >= 16)
        return 0xFFFFu;
    return (uint16_t)((1u << bits) - 1u);
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

    /* Stale or first time: retire queued spans before resetting slot 0.
     * Palette slot memory is outside the command stream, so this is a real
     * rendering boundary. */
    d3d_gpu_finish_for_lookup_update();

    for (int i = 0; i < D3D_GPU_PAL_SLOTS; i++)
        gpu_pal_base_of_slot[i] = NULL;
    d3d_gpu_shade_cache_clear();
    if (d3d_gpu_perf_enable)
        gpu_perf.pal_uploads++;
    uint32_t upload_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    uint32_t pal_bytes = d3d_gpu_pal_bytes();
    of_gpu_palookup_upload(0, palookup[0], pal_bytes);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(upload_t0);
        perf_add_time(&gpu_perf.pal_upload_us,
                      &gpu_perf.max_pal_upload_us, dt);
    }
    gpu_pal_base_of_slot[0] = palookup[0];
    gpu_pal_next_slot = 1;
    /* The current resolved slot starts at pal0. */
    gpu_current_slot = 0;
}

/* Returns the shade index (0..63) and slot for the given palookup row, or -1
 * if no GPU slot is available.  Slot selection is encoded per lane in the
 * affine span group metadata, so palette changes do not force batch splits. */
int d3d_gpu_shade_slot_for(const uint8_t *palookupoffse, int *slot_out)
{
    uint32_t pal_bytes = d3d_gpu_pal_bytes();
    if (slot_out)
        *slot_out = -1;
    if (!palookupoffse) {
        if (d3d_gpu_perf_enable)
            gpu_perf.pal_misses++;
        return -1;
    }
    ensure_pal0_uploaded();
    if (!gpu_pal_base_of_slot[0]) {
        if (d3d_gpu_perf_enable)
            gpu_perf.pal_misses++;
        return -1;   /* palookup[0] not ready */
    }

    int cached = d3d_gpu_shade_cache_lookup(palookupoffse, slot_out);
    if (cached >= 0)
        return cached;

    /* Hot path: same slot as previous call? */
    if (gpu_current_slot >= 0) {
        const uint8_t *base = gpu_pal_base_of_slot[gpu_current_slot];
        if (base) {
            ptrdiff_t off = palookupoffse - base;
            if ((uintptr_t)off < pal_bytes) {
                if (slot_out)
                    *slot_out = gpu_current_slot;
                int shade = (int)(off >> 8);
                d3d_gpu_shade_cache_store(palookupoffse, shade,
                                          gpu_current_slot);
                return shade;
            }
        }
    }

    /* Warm path: scan slots already uploaded. */
    for (int s = 0; s < gpu_pal_next_slot; s++) {
        const uint8_t *base = gpu_pal_base_of_slot[s];
        if (!base) continue;
        ptrdiff_t off = palookupoffse - base;
        if ((uintptr_t)off < pal_bytes) {
            gpu_current_slot = s;
            if (slot_out)
                *slot_out = s;
            int shade = (int)(off >> 8);
            d3d_gpu_shade_cache_store(palookupoffse, shade, s);
            return shade;
        }
    }

    /* Cold path: locate the BUILD palookup[i] containing this row,
     * upload it to a fresh slot, and return that explicit per-span slot. */
    for (int pal_id = 0; pal_id < 256; pal_id++) {
        const uint8_t *p = palookup[pal_id];
        if (!p) continue;
        ptrdiff_t off = palookupoffse - p;
        if ((uintptr_t)off >= pal_bytes) continue;

        if (gpu_pal_next_slot >= D3D_GPU_PAL_SLOTS) {
            if (d3d_gpu_perf_enable)
                gpu_perf.pal_misses++;
            return -1;   /* slot table full — caller falls back to CPU */
        }

        int s = gpu_pal_next_slot++;
        if (d3d_gpu_perf_enable)
            gpu_perf.pal_uploads++;
        uint32_t upload_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_gpu_palookup_upload((uint8_t)s, p, pal_bytes);
        if (d3d_gpu_perf_enable) {
            uint32_t dt = perf_dt_us(upload_t0);
            perf_add_time(&gpu_perf.pal_upload_us,
                          &gpu_perf.max_pal_upload_us, dt);
        }
        gpu_pal_base_of_slot[s] = p;
        gpu_current_slot = s;
        if (slot_out)
            *slot_out = s;
        int shade = (int)(off >> 8);
        d3d_gpu_shade_cache_store(palookupoffse, shade, s);
        return shade;
    }
    if (d3d_gpu_perf_enable)
        gpu_perf.pal_misses++;
    return -1;   /* not in any palookup at all (unusual) */
}

int d3d_gpu_shade_for(const uint8_t *palookupoffse)
{
    return d3d_gpu_shade_slot_for(palookupoffse, NULL);
}

/* Internal: build + emit a single COLUMN span. */
static inline void emit_column_span_slot(uint8_t *dest, int num_pixels, int shade,
                                         int32_t t, int32_t tstep,
                                         const uint8_t *texture,
                                         uint16_t tex_h_mask,
                                         uint8_t flags, int path, int slot)
{
    /* BUILD's vline index is (vplce >> v_shift), which naturally wraps to
     * the tile-height bit range.  Keep that same T mask on the GPU; leave S
     * unconstrained because column spans always use s=0/sstep=0. */
    d3d_gpu_emit_span_encoded(path,
                              (uint32_t)(uintptr_t)dest,
                              (uint32_t)(uintptr_t)texture,
                              0, t, 0, tstep,
                              (uint16_t)num_pixels,
                              (uint8_t)(shade & 0x3F),
                              flags, span_colormap_id_for_slot(slot),
                              (int16_t)bytesperline, 1, 0, tex_h_mask);
}

static inline void emit_column_span(uint8_t *dest, int num_pixels, int shade,
                                    int32_t t, int32_t tstep,
                                    const uint8_t *texture,
                                    uint16_t tex_h_mask,
                                    uint8_t flags, int path)
{
    emit_column_span_slot(dest, num_pixels, shade, t, tstep, texture,
                          tex_h_mask, flags, path, gpu_current_slot);
}

void d3d_gpu_vline(uint8_t *dest, int num_pixels, int shade,
                   uint32_t vplce, uint32_t vince, uint8_t v_shift,
                   const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();
    uint32_t t0 = perf_path_begin();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);
    uint16_t tex_h_mask = column_tex_h_mask(v_shift);

    /* fb_stride alone selects column vs row walk: emit_column_span
     * sets fb_stride = bytesperline so the rasteriser steps one row
     * per fragment automatically. */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture, tex_h_mask,
                     OF_GPU_SPAN_COLORMAP, PERF_PATH_VLINE);
    perf_note_path_time(PERF_PATH_VLINE, t0);
}

void d3d_gpu_mvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;

    ensure_pal0_uploaded();
    uint32_t t0 = perf_path_begin();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);
    uint16_t tex_h_mask = column_tex_h_mask(v_shift);

    /* SPAN_SKIP_ZERO drops texels matching the GPU's transparent index
     * (0xFF on this fabric — see gpudemo's sprite path).  BUILD's
     * TRANSPARENT_COLOR is also 0xFF, so this is a 1:1 map: any texel
     * the SW path skipped via `if (temp != TRANSPARENT_COLOR)` the GPU
     * skips at the fragment stage. */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture, tex_h_mask,
                     OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO,
                     PERF_PATH_MVLINE);
    perf_note_path_time(PERF_PATH_MVLINE, t0);
}

static inline void emit_vline4_span(uint8_t *fb_at_y0, int num_pixels,
                                    const int shade[4],
                                    const uint32_t vplce[4],
                                    const uint32_t vince[4],
                                    uint8_t v_shift,
                                    const uint8_t *const texture[4],
                                    const uint8_t colormap_id[4],
                                    uint8_t flags, int path)
{
    uint32_t tex_addr[4];
    int32_t t[4];
    int32_t tstep[4];
    uint8_t light[4];
    for (int c = 0; c < 4; c++) {
        to_16_16(vplce[c], vince[c], v_shift, &t[c], &tstep[c]);
        tex_addr[c] = (uint32_t)(uintptr_t)texture[c];
        light[c] = (uint8_t)(shade[c] & 0x3F);
    }
    d3d_gpu_emit_span_group_encoded(path,
                                    (uint32_t)(uintptr_t)fb_at_y0,
                                    (uint16_t)num_pixels,
                                    4, 1,
                                    flags,
                                    colormap_id,
                                    (int16_t)bytesperline, 1, 0,
                                    column_tex_h_mask(v_shift),
                                    tex_addr, t, tstep, light);
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
    uint32_t t0 = perf_path_begin();
    uint8_t colormap_id[4] = {
        current_span_colormap_id(),
        current_span_colormap_id(),
        current_span_colormap_id(),
        current_span_colormap_id()
    };
    emit_vline4_span(fb_at_y0, num_pixels, shade, vplce, vince, v_shift,
                     texture, colormap_id, OF_GPU_SPAN_COLORMAP,
                     PERF_PATH_VLINE4);
    perf_note_path_time(PERF_PATH_VLINE4, t0);
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
    uint32_t t0 = perf_path_begin();
    uint8_t colormap_id[4] = {
        current_span_colormap_id(),
        current_span_colormap_id(),
        current_span_colormap_id(),
        current_span_colormap_id()
    };

    emit_vline4_span(fb_at_y0, num_pixels, shade, vplce, vince, v_shift,
                     texture, colormap_id,
                     (uint8_t)(OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO),
                     PERF_PATH_MVLINE4);
    perf_note_path_time(PERF_PATH_MVLINE4, t0);
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
 * inline in vlineasm4 (an function), GCC's lazy-save
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
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present || !fb_base) return 0;
    int slot;
    int shade = d3d_gpu_shade_slot_for(palookupoffse, &slot);
    if (shade < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

    /* num_pixels here matches the SW path's `numPixels++; while(numPixels)`
     * — caller passes (numPixels + 1) explicitly. */
    uint32_t t0 = perf_path_begin();
    int32_t t, tstep;
    to_16_16((uint32_t)vplce, (uint32_t)vince, v_shift, &t, &tstep);
    emit_column_span_slot(dest, num_pixels, shade, t, tstep, texture,
                          column_tex_h_mask(v_shift),
                          OF_GPU_SPAN_COLORMAP, PERF_PATH_VLINE, slot);
    perf_note_path_time(PERF_PATH_VLINE, t0);
    if (vplce_out) *vplce_out = vplce + vince * num_pixels;
    return 1;
}

int d3d_gpu_try_mvline1(uint8_t *dest, int num_pixels,
                        const uint8_t *palookupoffse,
                        int32_t vplce, int32_t vince,
                        uint8_t v_shift, const uint8_t *texture,
                        int32_t *vplce_out)
{
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present || !fb_base) return 0;
    int slot;
    int shade = d3d_gpu_shade_slot_for(palookupoffse, &slot);
    if (shade < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

    uint32_t t0 = perf_path_begin();
    int32_t t, tstep;
    to_16_16((uint32_t)vplce, (uint32_t)vince, v_shift, &t, &tstep);
    emit_column_span_slot(dest, num_pixels, shade, t, tstep, texture,
                          column_tex_h_mask(v_shift),
                          OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO,
                          PERF_PATH_MVLINE, slot);
    perf_note_path_time(PERF_PATH_MVLINE, t0);
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
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present || !fb_base) return 0;

    int    shades[4];
    int    slots[4];
    const uint8_t *tex[4];

    shades[0] = d3d_gpu_shade_slot_for(palookupoffse[0], &slots[0]);
    if (shades[0] < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }
    tex[0] = (const uint8_t *)bufplce[0];

    const uint8_t *slot0_base = NULL;
    uint32_t pal_bytes = d3d_gpu_pal_bytes();
    if (slots[0] >= 0 && slots[0] < gpu_pal_next_slot)
        slot0_base = gpu_pal_base_of_slot[slots[0]];

    for (int c = 1; c < 4; c++) {
        if (slot0_base && palookupoffse[c]) {
            ptrdiff_t off = palookupoffse[c] - slot0_base;
            if ((uintptr_t)off < pal_bytes) {
                shades[c] = (int)(off >> 8);
                slots[c] = slots[0];
                d3d_gpu_shade_cache_store(palookupoffse[c],
                                          shades[c], slots[c]);
                tex[c] = (const uint8_t *)bufplce[c];
                continue;
            }
        }

        shades[c] = d3d_gpu_shade_slot_for(palookupoffse[c], &slots[c]);
        if (shades[c] < 0) {
            d3d_gpu_perf_note_cpu_fallback();
            d3d_gpu_prepare_cpu_fb_write();
            return 0;
        }
        tex[c] = (const uint8_t *)bufplce[c];
    }
    int path = masked ? PERF_PATH_MVLINE4 : PERF_PATH_VLINE4;
    uint8_t flags = OF_GPU_SPAN_COLORMAP;
    uint32_t t0 = perf_path_begin();
    if (masked)
        flags = (uint8_t)(flags | OF_GPU_SPAN_SKIP_ZERO);
    uint8_t colormap_id[4] = {
        span_colormap_id_for_slot(slots[0]),
        span_colormap_id_for_slot(slots[1]),
        span_colormap_id_for_slot(slots[2]),
        span_colormap_id_for_slot(slots[3])
    };
    emit_vline4_span(framebuffer, num_pixels, shades,
                     (const uint32_t *)vplce,
                     (const uint32_t *)vince, v_shift, tex,
                     colormap_id, flags, path);
    perf_note_path_time(path, t0);

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

static inline void emit_hline_span(uint8_t *dest_right, int num_pixels,
                                   int shade_x256,
                                   uint32_t i4, uint32_t i5,
                                   uint32_t asm1, uint32_t asm2,
                                   uint8_t width_bits, uint8_t shifter,
                                   const uint8_t *texture);

int d3d_gpu_try_hline(uint8_t *dest_right, int num_pixels, int shade_x256,
                      uint32_t i4, uint32_t i5,
                      uint32_t asm1, uint32_t asm2,
                      uint8_t width_bits, uint8_t shifter,
                      const uint8_t *texture)
{
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present || !fb_base) return 0;
    /* hlineasm4 reads via globalpalwritten[shade|s]; resolve that row's
     * palookup slot so the emitted span can carry it explicitly. */
    extern uint8_t *globalpalwritten;
    if (d3d_gpu_shade_for(globalpalwritten) < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

    uint32_t t0 = perf_path_begin();
    emit_hline_span(dest_right, num_pixels, shade_x256, i4, i5,
                    asm1, asm2, width_bits, shifter, texture);
    perf_note_path_time(PERF_PATH_HLINE, t0);
    return 1;
}

/* ----------------------------------------------------------------------
 * Translucent paths (transluc[] BLEND unit).
 *
 * No try/return fallback per project_gpu_owns_framebuffer.md — once a
 * call site is converted, the CPU loop is dead code.  If no palookup
 * slot is available (d3d_gpu_shade_for returns -1), the helper drops the
 * draw rather than rendering with the wrong shading.
 * -------------------------------------------------------------------- */

void d3d_gpu_tvline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t vplce, uint32_t vince, uint8_t v_shift,
                    const uint8_t *texture, int reverse)
{
    if (num_pixels <= 0) return;
    if (!d3d_gpu_present || !fb_base) return;
    if (shade < 0) return;

    ensure_pal0_uploaded();
    uint32_t t0 = perf_path_begin();

    int32_t t, tstep;
    to_16_16(vplce, vince, v_shift, &t, &tstep);
    uint16_t tex_h_mask = column_tex_h_mask(v_shift);

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* draw.c calls this only for TRANS_NORMAL. */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture, tex_h_mask, flags,
                     PERF_PATH_TVLINE);
    perf_note_path_time(PERF_PATH_TVLINE, t0);
}

void d3d_gpu_tvline2(uint8_t *dest_a, int num_pixels,
                     int shade_a, int shade_b,
                     int slot_a, int slot_b,
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
    uint32_t t0 = perf_path_begin();

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* draw.c calls this only for TRANS_NORMAL. */

    int32_t ta, tstepa, tb, tstepb;
    to_16_16(vplce_a, vince_a, v_shift, &ta, &tstepa);
    to_16_16(vplce_b, vince_b, v_shift, &tb, &tstepb);
    uint16_t tex_h_mask = column_tex_h_mask(v_shift);

    uint32_t tex_addr[2] = {
        (uint32_t)(uintptr_t)tex_a,
        (uint32_t)(uintptr_t)tex_b
    };
    int32_t t[2] = { ta, tb };
    int32_t tstep[2] = { tstepa, tstepb };
    uint8_t light[2] = {
        (uint8_t)(shade_a & 0x3F),
        (uint8_t)(shade_b & 0x3F)
    };
    uint8_t colormap_id[2] = {
        span_colormap_id_for_slot(slot_a),
        span_colormap_id_for_slot(slot_b)
    };

    d3d_gpu_emit_span_group_encoded(PERF_PATH_TVLINE2,
                                    (uint32_t)(uintptr_t)dest_a,
                                    (uint16_t)num_pixels,
                                    2, 1,
                                    flags,
                                    colormap_id,
                                    (int16_t)bytesperline, 1, 0,
                                    tex_h_mask,
                                    tex_addr, t, tstep, light);
    perf_note_path_time(PERF_PATH_TVLINE2, t0);
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
                                        const uint8_t *texture, uint8_t flags,
                                        int path)
{
    /* Same column-major-via-swapped-S/T model as d3d_gpu_sprite_vline
     * but with NEGATIVE per-pixel steps (BUILD walks `texture -= …`)
     * and fb_stride = -1 (dest writes go dest[-1], dest[-2], …).
     * fb_addr is dest - 1 so the first GPU write lands at dest[-1].
     *
     * gpu_core sign-extends S but its row multiply zero-extends T.  A
     * rotated sprite can legitimately step T below zero relative to the
     * per-line `texture` base; without a bias, that -1 row becomes 65535
     * rows and samples unrelated SDRAM.  The Duke death tilt path renders
     * the world into MAXTILES-2 and immediately rotates it back, making
     * that unsigned-row wrap show up as full-screen static while the HUD
     * remains correct.  Bias T into the unsigned range and subtract the
     * matching byte offset from the texture base so the final address is
     * unchanged for both positive and negative row deltas. */
    enum { T_BIAS_ROWS = 32768 };
    uint32_t tex_addr = (uint32_t)(uintptr_t)texture -
                        (uint32_t)T_BIAS_ROWS * (uint32_t)rs_tileHeight;
    int32_t t_biased = (int32_t)((uint32_t)(bx_frac >> 16) +
                                 ((uint32_t)T_BIAS_ROWS << 16));

    d3d_gpu_emit_span_encoded(path,
                              (uint32_t)(uintptr_t)(dest - 1),
                              tex_addr,
                              (int32_t)(by_frac >> 16),
                              t_biased,
                              -(int32_t)rs_yv2_full,
                              -(int32_t)rs_xv2_full,
                              (uint16_t)num_pixels,
                              (uint8_t)(shade & 0x3F),
                              flags, current_span_colormap_id(),
                              (int16_t)-1, (uint16_t)rs_tileHeight,
                              0, 0);
}

int d3d_gpu_rhline(uint8_t *dest, int num_pixels, int shade,
                   uint32_t bx_frac, uint32_t xv2_step,
                   uint32_t by_frac, uint32_t yv2_step,
                   uint16_t tile_height,
                   const uint8_t *texture)
{
    (void)xv2_step; (void)yv2_step; (void)tile_height;  /* sourced from rs_* */
    if (num_pixels <= 0) return 1;
    if (!d3d_gpu_present || !fb_base) return 0;
    if (shade < 0) return 0;
    ensure_pal0_uploaded();
    uint32_t t0 = perf_path_begin();
    emit_rotsprite_hline(dest, num_pixels, shade, bx_frac, by_frac,
                         texture, OF_GPU_SPAN_COLORMAP, PERF_PATH_RHLINE);
    perf_note_path_time(PERF_PATH_RHLINE, t0);
    return 1;
}

int d3d_gpu_rmhline(uint8_t *dest, int num_pixels, int shade,
                    uint32_t bx_frac, uint32_t xv2_step,
                    uint32_t by_frac, uint32_t yv2_step,
                    uint16_t tile_height,
                    const uint8_t *texture)
{
    (void)xv2_step; (void)yv2_step; (void)tile_height;
    if (num_pixels <= 0) return 1;
    if (!d3d_gpu_present || !fb_base) return 0;
    if (shade < 0) return 0;
    ensure_pal0_uploaded();
    uint32_t t0 = perf_path_begin();
    emit_rotsprite_hline(dest, num_pixels, shade, bx_frac, by_frac,
                         texture, OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO,
                         PERF_PATH_RMHLINE);
    perf_note_path_time(PERF_PATH_RMHLINE, t0);
    return 1;
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
    uint32_t t0 = perf_path_begin();

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* draw.c calls this only for TRANS_NORMAL. */

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
    d3d_gpu_emit_span_encoded(PERF_PATH_SPRITE,
                              (uint32_t)(uintptr_t)dest,
                              (uint32_t)(uintptr_t)texture,
                              (int32_t)(by_frac >> 16),
                              (int32_t)(bx_frac >> 16),
                              (int32_t)yv_step,
                              (int32_t)xv_step,
                              (uint16_t)num_pixels,
                              (uint8_t)(shade & 0x3F),
                              flags, current_span_colormap_id(),
                              (int16_t)bytesperline, tile_height,
                              0, 0);
    perf_note_path_time(PERF_PATH_SPRITE, t0);
}

/* Internal: shared hline emission for forward-walking floor/ceiling
 * spans (mhlineskipmodify, thlineskipmodify).  Same shld→GPU mapping
 * as d3d_gpu_hline below, but fb_stride=+1 and S/T steps are positive
 * (BUILD's mhline does i2 += asm1; i5 += asm2; dest++ per pixel). */
static inline void emit_fwd_hline(uint8_t *dest, int num_pixels, int shade_x256,
                                  uint32_t i2, uint32_t i5,
                                  uint32_t asm1, uint32_t asm2,
                                  uint8_t width_bits, uint8_t shifter,
                                  const uint8_t *texture, uint8_t flags,
                                  int path)
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
    d3d_gpu_emit_span_encoded(path,
                              (uint32_t)(uintptr_t)dest,
                              (uint32_t)(uintptr_t)texture,
                              sp_s, sp_t, sp_sstep, sp_tstep,
                              (uint16_t)num_pixels,
                              (uint8_t)((shade_x256 >> 8) & 0x3F),
                              flags, current_span_colormap_id(),
                              (int16_t)+1, tex_w,
                              (uint16_t)(tex_w - 1),
                              (uint16_t)(tex_h - 1));
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
    uint32_t t0 = perf_path_begin();
    /* Caller (mhlineskipmodify) resolved the palookup slot — see hook in
     * draw.c. */
    emit_fwd_hline(dest, num_pixels, shade_x256, i2, i5, asm1, asm2,
                   width_bits, shifter, texture,
                   OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO,
                   PERF_PATH_MHLINE);
    perf_note_path_time(PERF_PATH_MHLINE, t0);
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
    uint32_t t0 = perf_path_begin();
    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* draw.c calls this only for TRANS_NORMAL. */
    emit_fwd_hline(dest, num_pixels, shade_x256, i2, i5, asm1, asm2,
                   width_bits, shifter, texture, flags, PERF_PATH_THLINE);
    perf_note_path_time(PERF_PATH_THLINE, t0);
}

static inline void emit_hline_span(uint8_t *dest_right, int num_pixels,
                                   int shade_x256,
                                   uint32_t i4, uint32_t i5,
                                   uint32_t asm1, uint32_t asm2,
                                   uint8_t width_bits, uint8_t shifter,
                                   const uint8_t *texture)
{
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
    d3d_gpu_emit_span_encoded(PERF_PATH_HLINE,
                              (uint32_t)(uintptr_t)dest_right,
                              (uint32_t)(uintptr_t)texture,
                              sp_s, sp_t, sp_sstep, sp_tstep,
                              (uint16_t)num_pixels,
                              (uint8_t)((shade_x256 >> 8) & 0x3F),
                              OF_GPU_SPAN_COLORMAP,
                              current_span_colormap_id(),
                              (int16_t)-1, tex_w,
                              (uint16_t)(tex_w - 1),
                              (uint16_t)(tex_h - 1));
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
    uint32_t t0 = perf_path_begin();
    /* Caller (hlineasm4) must have already resolved the floor palookup
     * slot via the public predicate — see hlineasm4's gate in draw.c. */
    emit_hline_span(dest_right, num_pixels, shade_x256, i4, i5,
                    asm1, asm2, width_bits, shifter, texture);
    perf_note_path_time(PERF_PATH_HLINE, t0);
}
