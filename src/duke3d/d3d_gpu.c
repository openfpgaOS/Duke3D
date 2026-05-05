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

int d3d_gpu_present  = 0;
/* Diagnostic flag — when set to 1 (e.g. via debugger), d3d_gpu_submit_span
 * counts the would-be span but skips the actual submit, isolating per-span
 * CPU formatting cost from dispatch + GPU rasterisation cost.  Frame is
 * visually wrong while set; [perf-rooms] render time reveals the upper
 * bound on what batching could save. */
int d3d_gpu_skip_submit = 0;

int d3d_gpu_use_spans = 1;   /* Master switch.  Set to 0 BEFORE the
                              * first frame to disable the GPU
                              * entirely: d3d_gpu_init() bails so
                              * d3d_gpu_present stays 0, every helper
                              * (set_fb, drain, flush, pre_cpu_fb_access,
                              * clear_rect_fb, all the span hooks)
                              * short-circuits, and BUILD's CPU loops
                              * run unmodified.  Used for the GPU-on
                              * vs GPU-off A/B perf test. */
int d3d_gpu_force_cpu_spans = 0;  /* Scoped gate: when non-zero, draw.c
                                   * try paths fall through to BUILD's
                                   * original CPU loops. */
int d3d_gpu_force_rotatesprite_cpu = 1;
int d3d_gpu_use_span4 = 0;
int d3d_gpu_use_command_stream_batch = 0;
int d3d_gpu_perf_enable = 0;
int d3d_gpu_perf_deep_enable = 0;
/* Per-path timing calls of_time_us() twice per hot helper call, which is
 * useful for short profiling runs but distorts the FPS we are measuring.
 * Keep path span/pixel/call counters on by default and make timings opt-in. */
int d3d_gpu_perf_time_paths = 0;

/* Cached framebuffer base for the GPU-disabled CPU-fallback path.
 * Stride is read from BUILD's bytesperline at submit time: setviewtotile
 * (low-detail mode, tilted/rotated screen blits) flips bytesperline
 * mid-frame between screen stride and tile stride, and a stale cache
 * here would land vline writes on every other tile row. */
static uint8_t *fb_base;
extern int32_t  bytesperline;   /* BUILD's live row stride in bytes */
static int gpu_fb_drained_for_cpu;
static int cpu_fb_read_coherent;

static inline void mark_gpu_fb_dirty(void)
{
    gpu_fb_drained_for_cpu = 0;
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

static const uint8_t *gpu_pal_base_of_slot[D3D_GPU_PAL_SLOTS];
static int            gpu_pal_next_slot;     /* number of slots in use */
static int            gpu_current_slot;      /* Last resolved span slot, -1 = unset */
extern short          numpalookups;

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
} d3d_gpu_perf_t;

static d3d_gpu_perf_t gpu_perf;
static uint32_t gpu_perf_last_report_ms;

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

static inline uint32_t perf_dt_us(uint32_t t0)
{
    return of_time_us() - t0;
}

static inline void perf_note_path_time(int path, uint32_t t0);
static inline void perf_note_path_span(int path, const of_gpu_span_t *span);

void d3d_gpu_perf_note_cpu_fallback(void)
{
    if (!d3d_gpu_perf_enable) return;
    gpu_perf.cpu_fallbacks++;
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
    if (elapsed_ms < 1000)
        return;

    uint32_t frames = gpu_perf.frames;
    uint32_t fps = elapsed_ms ? (uint32_t)((uint64_t)frames * 1000u /
                                           elapsed_ms) : 0;
    uint32_t batch_submit_avg =
        perf_avg(gpu_perf.batch_submit_us, gpu_perf.batches);
    uint32_t clear_emit_avg =
        perf_avg(gpu_perf.clear_emit_us, gpu_perf.clear_rects);
    uint32_t setfb_avg =
        perf_avg(gpu_perf.setfb_us, gpu_perf.setfb_calls);
    uint32_t pal_upload_avg =
        perf_avg(gpu_perf.pal_upload_us, gpu_perf.pal_uploads);
    uint32_t pal_switch_avg =
        perf_avg(gpu_perf.pal_switch_us, gpu_perf.pal_switches);
    uint32_t finish_avg =
        perf_avg(gpu_perf.finish_us, gpu_perf.finish_calls);
    uint32_t cpu_sync_avg =
        perf_avg(gpu_perf.cpu_sync_us, gpu_perf.cpu_syncs);
    uint32_t direct_submit_avg =
        perf_avg(gpu_perf.direct_submit_us, gpu_perf.direct_spans);

    printf("[perf] fps=%u period=%u/%u render=%u/%u page=%u/%u wait=%u/%u "
           "drain=%u/%u flip=%u/%u acq=%u/%u aud=%u/%u "
           "span=%u pix=%u mask=%u trans=%u skip=%u direct=%u/%u "
           "dsub=%u/%u batch=%u maxb=%u submit=%u/%u clear=%u/%u/%u "
           "setfb=%u/%u texfl=%u palup=%u/%u/%u palsw=%u/%u/%u miss=%u "
           "fbfin=%u/%u cpusync=%u/%u fallback=%u\n",
           fps,
           perf_avg(gpu_perf.frame_period_us, frames),
           gpu_perf.max_frame_period_us,
           perf_avg(gpu_perf.render_us, frames),
           gpu_perf.max_render_us,
           perf_avg(gpu_perf.page_us, frames),
           gpu_perf.max_page_us,
           perf_avg(gpu_perf.wait_flip_us, frames),
           gpu_perf.max_wait_flip_us,
           perf_avg(gpu_perf.drain_batch_us, frames),
           gpu_perf.max_drain_batch_us,
           perf_avg(gpu_perf.flip_emit_us, frames),
           gpu_perf.max_flip_emit_us,
           perf_avg(gpu_perf.acquire_us, frames),
           gpu_perf.max_acquire_us,
           perf_avg(gpu_perf.audio_us, frames),
           gpu_perf.max_audio_us,
           gpu_perf.spans,
           gpu_perf.span_pixels,
           gpu_perf.masked_spans,
           gpu_perf.translucent_spans,
           gpu_perf.skipped_spans,
           gpu_perf.direct_spans,
           gpu_perf.direct_pixels,
           direct_submit_avg,
           gpu_perf.max_direct_submit_us,
           gpu_perf.batches,
           gpu_perf.max_batch,
           batch_submit_avg,
           gpu_perf.max_batch_submit_us,
           gpu_perf.clear_rects,
           clear_emit_avg,
           gpu_perf.max_clear_emit_us,
           setfb_avg,
           gpu_perf.max_setfb_us,
           gpu_perf.tex_flushes,
           gpu_perf.pal_uploads,
           pal_upload_avg,
           gpu_perf.max_pal_upload_us,
           gpu_perf.pal_switches,
           pal_switch_avg,
           gpu_perf.max_pal_switch_us,
           gpu_perf.pal_misses,
           finish_avg,
           gpu_perf.max_finish_us,
           cpu_sync_avg,
           gpu_perf.max_cpu_sync_us,
           gpu_perf.cpu_fallbacks);

#define PHASE_ARGS(p) \
           gpu_perf.phase_calls[p], \
           perf_avg(gpu_perf.phase_us[p], gpu_perf.phase_calls[p]), \
           gpu_perf.phase_max_us[p]
    printf("[perf-phase] disp=%u/%u/%u rooms=%u/%u/%u masks=%u/%u/%u\n",
           PHASE_ARGS(D3D_GPU_PERF_PHASE_DISPLAYROOMS),
           PHASE_ARGS(D3D_GPU_PERF_PHASE_DRAWROOMS),
           PHASE_ARGS(D3D_GPU_PERF_PHASE_DRAWMASKS));
#undef PHASE_ARGS

#define ZONE_ARGS(z) \
           gpu_perf.zone_calls[z], \
           perf_avg(gpu_perf.zone_us[z], gpu_perf.zone_calls[z]), \
           gpu_perf.zone_max_us[z]
    uint32_t zone_call_count = 0;
    for (int z = 0; z < D3D_GPU_PERF_ZONE_COUNT; z++)
        zone_call_count += gpu_perf.zone_calls[z];
    if (zone_call_count) {
        printf("[perf-zone1] ceil=%u/%u/%u flor=%u/%u/%u wall=%u/%u/%u "
               "mscan=%u/%u/%u tscan=%u/%u/%u\n",
               ZONE_ARGS(D3D_GPU_PERF_ZONE_CEILSCAN),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_FLORSCAN),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_WALLSCAN),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_MASKWALLSCAN),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_TRANSMASKWALLSCAN));
        printf("[perf-zone2] dmwall=%u/%u/%u sprite=%u/%u/%u rot=%u/%u/%u\n",
               ZONE_ARGS(D3D_GPU_PERF_ZONE_DRAWMASKWALL),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_DRAWSPRITE),
               ZONE_ARGS(D3D_GPU_PERF_ZONE_DOROTATESPRITE));
    }
#undef ZONE_ARGS

#define PATH_ARGS(p) \
           gpu_perf.path_spans[p], \
           gpu_perf.path_pixels[p], \
           gpu_perf.path_calls[p], \
           perf_avg(gpu_perf.path_us[p], gpu_perf.path_calls[p]), \
           gpu_perf.path_max_us[p]
    printf("[perf-path1] v=%u/%u/%u/%u/%u mv=%u/%u/%u/%u/%u "
           "v4=%u/%u/%u/%u/%u mv4=%u/%u/%u/%u/%u "
           "h=%u/%u/%u/%u/%u mh=%u/%u/%u/%u/%u\n",
           PATH_ARGS(PERF_PATH_VLINE),
           PATH_ARGS(PERF_PATH_MVLINE),
           PATH_ARGS(PERF_PATH_VLINE4),
           PATH_ARGS(PERF_PATH_MVLINE4),
           PATH_ARGS(PERF_PATH_HLINE),
           PATH_ARGS(PERF_PATH_MHLINE));
    printf("[perf-path2] th=%u/%u/%u/%u/%u tv=%u/%u/%u/%u/%u "
           "tv2=%u/%u/%u/%u/%u rh=%u/%u/%u/%u/%u "
           "rmh=%u/%u/%u/%u/%u spr=%u/%u/%u/%u/%u "
           "mir=%u/%u/%u/%u/%u\n",
           PATH_ARGS(PERF_PATH_THLINE),
           PATH_ARGS(PERF_PATH_TVLINE),
           PATH_ARGS(PERF_PATH_TVLINE2),
           PATH_ARGS(PERF_PATH_RHLINE),
           PATH_ARGS(PERF_PATH_RMHLINE),
           PATH_ARGS(PERF_PATH_SPRITE),
           PATH_ARGS(PERF_PATH_MIRROR));
#undef PATH_ARGS

    memset(&gpu_perf, 0, sizeof(gpu_perf));
    gpu_perf_last_report_ms = now_ms;
}

/* GPU texture-cache invalidate is unconditional at every frame
 * boundary in d3d_gpu_set_fb — covers every BUILD path that mutates
 * tile bytes without having to hook each one.  Cache walk is ~10 µs
 * at 100 MHz; the tex cache only helps within a frame so an
 * across-frame invalidate costs nothing in steady state.
 * d3d_gpu_mark_tex_dirty is kept as an ABI no-op for tiles.c. */

void d3d_gpu_init(void)
{
    gpu_perf_last_report_ms = of_time_ms();

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

/* Batched span submission.
 *
 * The default path uses the GPU's homogeneous DRAW_SPANS_BATCH command for
 * scalar spans.  It is enough while SPAN4 is disabled, uses one fewer word
 * per span, and avoids the raw mixed-command DMA path while we are chasing
 * visual correctness.  The mixed stream buffer remains available as an A/B
 * path for future scalar+SPAN4 interleaving experiments. */
#define D3D_GPU_CMD_STREAM_WORDS      OF_GPU_COMMAND_STREAM_BATCH_WORDS
#define D3D_GPU_CMD_DRAW_SPAN_WORDS   (1u + OF_GPU_BATCH_WORDS_PER_SPAN)
#define D3D_GPU_CMD_DRAW_SPAN4_WORDS  (1u + OF_GPU_SPAN4_WORDS)

static of_gpu_span_t  span_buf[OF_GPU_BATCH_MAX_SPANS];
static int            span_buf_count;
static uint32_t       span_cmd_buf[D3D_GPU_CMD_STREAM_WORDS];
static int            span_cmd_words;
static int            span_cmd_count;
static of_gpu_span_t  span_scratch;
static of_gpu_span4_t span4_scratch;

static inline void d3d_gpu_flush_batch(void) {
    if (span_buf_count > 0) {
        SPIN_TAG(5);
        if (d3d_gpu_perf_enable) {
            gpu_perf.flushes++;
            gpu_perf.batches++;
            if ((uint32_t)span_buf_count > gpu_perf.max_batch)
                gpu_perf.max_batch = (uint32_t)span_buf_count;
        }
        uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_gpu_draw_spans_batch(span_buf, span_buf_count);
        if (d3d_gpu_perf_enable) {
            uint32_t dt = perf_dt_us(t0);
            perf_add_time(&gpu_perf.batch_submit_us,
                          &gpu_perf.max_batch_submit_us, dt);
        }
        span_buf_count = 0;
        SPIN_TAG(0);
    }
    if (span_cmd_words > 0) {
        SPIN_TAG(5);
        if (d3d_gpu_perf_enable) {
            gpu_perf.flushes++;
            gpu_perf.batches++;
            if ((uint32_t)span_cmd_count > gpu_perf.max_batch)
                gpu_perf.max_batch = (uint32_t)span_cmd_count;
        }
        uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_gpu_submit_command_stream_batch(span_cmd_buf, span_cmd_words);
        if (d3d_gpu_perf_enable) {
            uint32_t dt = perf_dt_us(t0);
            perf_add_time(&gpu_perf.batch_submit_us,
                          &gpu_perf.max_batch_submit_us, dt);
        }
        span_cmd_words = 0;
        span_cmd_count = 0;
        SPIN_TAG(0);
    }
}

static inline void d3d_gpu_reserve_cmd_words(uint32_t words) {
    if (words > D3D_GPU_CMD_STREAM_WORDS)
        __builtin_trap();
    if ((uint32_t)span_cmd_words + words > D3D_GPU_CMD_STREAM_WORDS)
        d3d_gpu_flush_batch();
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
    uint32_t token = of_gpu_flip_to(idx);
    of_gpu_kick();
    SPIN_TAG(0);
    return token;
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

    of_gpu_fence();

    of_gpu_span_t sp;
    sp.s         = 0;
    sp.t         = 0;
    sp.sstep     = 0x10000;             /* +1 pixel/pixel, forward sample */
    sp.tstep     = 0;
    sp.count     = (uint16_t)count;
    sp.light     = 0;
    sp.flags     = 0;                   /* raw byte copy, no colormap */
    sp.colormap_id = 0;
    sp.fb_stride = -1;                  /* dst walks RIGHT-to-LEFT */
    sp.tex_width = 1;                   /* affine, no t advance — ignored */
    sp.tex_w_mask = 0xFFFF;
    sp.tex_h_mask = 0xFFFF;
    sp.sdivz = sp.tdivz = sp.zi_persp = 0;
    sp.sdivz_step = sp.tdivz_step = sp.zi_step = 0;

    /* fb_addr for row 0 starts at dst's LAST pixel (so the backwards
     * walk finishes at dst[0]); tex_addr starts at src's FIRST pixel
     * (forward sample with sstep=+1 reads src[0..count-1]). */
    uint32_t dst_last = (uint32_t)(uintptr_t)(dst + (count - 1));
    uint32_t src_base = (uint32_t)(uintptr_t)src;

    for (int r = 0; r < rows; r++) {
        sp.fb_addr  = dst_last + (uint32_t)(r * row_stride);
        sp.tex_addr = src_base + (uint32_t)(r * row_stride);
        uint32_t t0 = (d3d_gpu_perf_enable && d3d_gpu_perf_time_paths) ?
            of_time_us() : 0;
        of_gpu_draw_span(&sp);
        if (d3d_gpu_perf_enable) {
            gpu_perf.direct_spans++;
            gpu_perf.direct_pixels += (uint32_t)count;
            if (t0) {
                uint32_t dt = perf_dt_us(t0);
                perf_add_time(&gpu_perf.direct_submit_us,
                              &gpu_perf.max_direct_submit_us, dt);
            }
            perf_note_path_span(PERF_PATH_MIRROR, &sp);
            perf_note_path_time(PERF_PATH_MIRROR, t0);
        }
    }
    mark_gpu_fb_dirty();
    of_gpu_kick();
}

/* Direct-write submit: caller gets a pointer to the next free span
 * slot, writes the fields directly (skipping a stack-construct +
 * struct-copy roundtrip), then calls commit_span to advance the
 * counter and trigger a buffer-full flush if needed.  Saves ~16
 * stores per span vs the stack+memcpy pattern. */
static inline of_gpu_span_t *d3d_gpu_alloc_span(void) {
    return &span_scratch;
}

static inline of_gpu_span4_t *d3d_gpu_alloc_span4(void) {
    return &span4_scratch;
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

static inline void perf_note_path_span(int path, const of_gpu_span_t *span)
{
    if (!d3d_gpu_perf_enable) return;
    if (path < 0 || path >= PERF_PATH_COUNT) return;
    gpu_perf.path_spans[path]++;
    gpu_perf.path_pixels[path] += span->count;
}

static inline void perf_note_path_span4(int path, const of_gpu_span4_t *span)
{
    if (!d3d_gpu_perf_enable) return;
    if (path < 0 || path >= PERF_PATH_COUNT) return;
    gpu_perf.path_spans[path]++;
    gpu_perf.path_pixels[path] += (uint32_t)span->count * 4u;
}

static inline uint8_t current_span_colormap_id(void)
{
    return (gpu_current_slot > 0) ? (uint8_t)(gpu_current_slot & 0xF) : 0;
}

static inline uint8_t span_colormap_id_for_slot(int slot)
{
    return (slot > 0) ? (uint8_t)(slot & 0xF) : 0;
}

static inline void d3d_gpu_commit_span(int path) {
    of_gpu_span_t *span = &span_scratch;
    if (d3d_gpu_perf_enable) {
        gpu_perf.spans++;
        gpu_perf.span_pixels += span->count;
        if (span->flags & OF_GPU_SPAN_SKIP_ZERO)
            gpu_perf.masked_spans++;
        if (span->flags & OF_GPU_SPAN_TRANSLUC)
            gpu_perf.translucent_spans++;
        if (d3d_gpu_skip_submit)
            gpu_perf.skipped_spans++;
        perf_note_path_span(path, span);
    }
    if (d3d_gpu_skip_submit) return;
    if (d3d_gpu_use_command_stream_batch) {
        d3d_gpu_reserve_cmd_words(D3D_GPU_CMD_DRAW_SPAN_WORDS);
        span_cmd_buf[span_cmd_words++] =
            ((uint32_t)GPU_CMD_DRAW_SPAN << 24) | OF_GPU_BATCH_WORDS_PER_SPAN;
        _gpu_encode_span(&span_cmd_buf[span_cmd_words], span);
        span_cmd_words += OF_GPU_BATCH_WORDS_PER_SPAN;
        span_cmd_count++;
    } else {
        if (span_buf_count >= OF_GPU_BATCH_MAX_SPANS)
            d3d_gpu_flush_batch();
        span_buf[span_buf_count++] = *span;
    }
    mark_gpu_fb_dirty();
}

static inline void d3d_gpu_commit_span4(int path) {
    of_gpu_span4_t *span = &span4_scratch;
    if (d3d_gpu_perf_enable) {
        gpu_perf.spans++;
        gpu_perf.span_pixels += (uint32_t)span->count * 4u;
        if (span->flags & OF_GPU_SPAN_SKIP_ZERO)
            gpu_perf.masked_spans++;
        if (span->flags & OF_GPU_SPAN_TRANSLUC)
            gpu_perf.translucent_spans++;
        if (d3d_gpu_skip_submit)
            gpu_perf.skipped_spans++;
        perf_note_path_span4(path, span);
    }
    if (d3d_gpu_skip_submit) return;
    if (d3d_gpu_use_command_stream_batch) {
        d3d_gpu_reserve_cmd_words(D3D_GPU_CMD_DRAW_SPAN4_WORDS);
        span_cmd_buf[span_cmd_words++] =
            ((uint32_t)GPU_CMD_DRAW_SPAN4 << 24) | OF_GPU_SPAN4_WORDS;
        _gpu_encode_span4(&span_cmd_buf[span_cmd_words], span);
        span_cmd_words += OF_GPU_SPAN4_WORDS;
        span_cmd_count++;
    } else {
        d3d_gpu_flush_batch();
        of_gpu_draw_span4(span);
    }
    mark_gpu_fb_dirty();
}

void d3d_gpu_set_fb(uint8_t *fb_pixels, int stride_pixels)
{
    /* Cache fb_base for the GPU-off CPU memset path in
     * d3d_gpu_clear_rect_fb.  Stride is no longer cached — span
     * emitters read BUILD's bytesperline live so they track
     * setviewtotile's stride flip without a separate hook. */
    fb_base = fb_pixels;
    gpu_fb_drained_for_cpu = 0;
    cpu_fb_read_coherent = 0;
    if (!d3d_gpu_present) return;

    if (d3d_gpu_perf_enable) {
        gpu_perf.setfb_calls++;
        gpu_perf.tex_flushes++;
    }
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;

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

    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb_pixels,
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
    if (d3d_gpu_perf_enable)
        gpu_perf.pal_uploads++;
    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_palookup_upload(0, palookup_table, (uint32_t)num_shades * 256u);
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(t0);
        perf_add_time(&gpu_perf.pal_upload_us,
                      &gpu_perf.max_pal_upload_us, dt);
    }
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
    SPIN_TAG(8);
    if (!d3d_gpu_present || !d3d_gpu_use_spans) {
        /* GPU off — CPU memset path so A/B tests against the GPU
         * implementation work, and the no-GPU bitstream still draws.
         * Live bytesperline tracks setviewtotile's stride flip. */
        for (int y = 0; y < (int)h; y++)
            memset(dest + y * bytesperline, color, w);
        SPIN_TAG(0);
        return;
    }
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
    of_gpu_clear_rect_strided((uint32_t)(uintptr_t)dest,
                               w, h,
                               (uint16_t)bytesperline,
                               color);
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
    if (cpu_fb_read_coherent) return;
    uint32_t sync_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    SPIN_TAG(3);
    if (!gpu_fb_drained_for_cpu) {
        /* Flush any pending span batch so its writes land in SDRAM before
         * we wait for the GPU to drain. */
        d3d_gpu_flush_batch();
        /* Drain GPU writes to SDRAM so subsequent CPU reads see current
         * pixels (without this, a CPU read of a region the GPU just
         * wrote could return stale SDRAM). */
        SPIN_TAG(4);
        uint32_t finish_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_gpu_finish();
        if (d3d_gpu_perf_enable) {
            uint32_t dt = perf_dt_us(finish_t0);
            gpu_perf.finish_calls++;
            perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
        }
        gpu_fb_drained_for_cpu = 1;
    }
    /* Invalidate L1 — without this, a CPU read of an FB byte the
     * fabric just wrote could hit a STALE clean line in L1 from the
     * previous frame.  of_cache_flush is "write-back + invalidate"
     * and is the simplest correct primitive here.  The post-mirror
     * write-back is folded into of_video_flip()'s of_cache_clean_range
     * call (covers the FB area), so we don't need a separate flush
     * after the mirror writes. */
    of_cache_flush();
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
    if (gpu_fb_drained_for_cpu) return;
    uint32_t sync_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    SPIN_TAG(3);
    d3d_gpu_flush_batch();
    SPIN_TAG(4);
    uint32_t finish_t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    of_gpu_finish();
    if (d3d_gpu_perf_enable) {
        uint32_t dt = perf_dt_us(finish_t0);
        gpu_perf.finish_calls++;
        perf_add_time(&gpu_perf.finish_us, &gpu_perf.max_finish_us, dt);
    }
    gpu_fb_drained_for_cpu = 1;
    cpu_fb_read_coherent = 0;
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
    if (!d3d_gpu_present) return;
    if (d3d_gpu_perf_enable)
        gpu_perf.tex_flushes++;
    GPU_TEX_FLUSH = 1;
}

void d3d_gpu_mark_tex_dirty(void)
{
    /* Stub — d3d_gpu_set_fb invalidates the tex cache unconditionally
     * at every frame boundary.  Kept for ABI compatibility with
     * tiles.c; no per-call work needed. */
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

    /* Stale or first time: reset cache + upload pal0 to slot 0. */
    for (int i = 0; i < D3D_GPU_PAL_SLOTS; i++)
        gpu_pal_base_of_slot[i] = NULL;
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
    /* Sticky GPU state resets to 0; resolved-slot cache starts there too. */
    gpu_current_slot = 0;
}

/* Returns the shade index (0..63) and slot for the given palookup row, or -1
 * if no GPU slot is available.  Slot changes are per-span now: word 6's
 * colormap_id nibble overrides sticky GPU state, so palette churn does not
 * force a batch flush. */
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

    /* Hot path: same slot as previous call? */
    if (gpu_current_slot >= 0) {
        const uint8_t *base = gpu_pal_base_of_slot[gpu_current_slot];
        if (base) {
            ptrdiff_t off = palookupoffse - base;
            if ((uintptr_t)off < pal_bytes) {
                if (slot_out)
                    *slot_out = gpu_current_slot;
                return (int)(off >> 8);
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
            return (int)(off >> 8);
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
        return (int)(off >> 8);
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
    of_gpu_span_t *span = d3d_gpu_alloc_span();
    span->fb_addr   = (uint32_t)(uintptr_t)dest;
    span->tex_addr  = (uint32_t)(uintptr_t)texture;
    span->s         = 0;
    span->t         = t;
    span->sstep     = 0;
    span->tstep     = tstep;
    span->count     = (uint16_t)num_pixels;
    span->light     = (uint8_t)(shade & 0x3F);
    span->flags     = flags;
    span->colormap_id = span_colormap_id_for_slot(slot);
    span->fb_stride = (int16_t)bytesperline;
    span->tex_width = 1;
    /* BUILD's vline index is (vplce >> v_shift), which naturally wraps to
     * the tile-height bit range.  Keep that same T mask on the GPU; leave S
     * unconstrained because column spans always use s=0/sstep=0. */
    span->tex_w_mask = 0;
    span->tex_h_mask = tex_h_mask;
    span->sdivz = 0;
    span->tdivz = 0;
    span->zi_persp = 0;
    span->sdivz_step = 0;
    span->tdivz_step = 0;
    span->zi_step = 0;
    d3d_gpu_commit_span(path);
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

    of_gpu_span4_t *span = d3d_gpu_alloc_span4();
    span->fb_addr = (uint32_t)(uintptr_t)fb_at_y0;
    span->count = (uint16_t)num_pixels;
    span->flags = OF_GPU_SPAN_COLORMAP;
    span->colormap_id = current_span_colormap_id();
    span->fb_stride = (int16_t)bytesperline;
    span->tex_width = 1;
    for (int c = 0; c < 4; c++) {
        int32_t t, tstep;
        to_16_16(vplce[c], vince[c], v_shift, &t, &tstep);
        span->tex_addr[c] = (uint32_t)(uintptr_t)texture[c];
        span->t[c] = t;
        span->tstep[c] = tstep;
        span->light[c] = (uint8_t)(shade[c] & 0x3F);
    }
    d3d_gpu_commit_span4(PERF_PATH_VLINE4);
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

    of_gpu_span4_t *span = d3d_gpu_alloc_span4();
    span->fb_addr = (uint32_t)(uintptr_t)fb_at_y0;
    span->count = (uint16_t)num_pixels;
    span->flags = (uint8_t)(OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO);
    span->colormap_id = current_span_colormap_id();
    span->fb_stride = (int16_t)bytesperline;
    span->tex_width = 1;
    for (int c = 0; c < 4; c++) {
        int32_t t, tstep;
        to_16_16(vplce[c], vince[c], v_shift, &t, &tstep);
        span->tex_addr[c] = (uint32_t)(uintptr_t)texture[c];
        span->t[c] = t;
        span->tstep[c] = tstep;
        span->light[c] = (uint8_t)(shade[c] & 0x3F);
    }
    d3d_gpu_commit_span4(PERF_PATH_MVLINE4);
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
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    int shade = d3d_gpu_shade_for(palookupoffse);
    if (shade < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

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
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    int shade = d3d_gpu_shade_for(palookupoffse);
    if (shade < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

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
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;

    int    shades[4];
    int    slots[4];
    const uint8_t *tex[4];
    for (int c = 0; c < 4; c++) {
        shades[c] = d3d_gpu_shade_slot_for(palookupoffse[c], &slots[c]);
        if (shades[c] < 0) {
            d3d_gpu_perf_note_cpu_fallback();
            d3d_gpu_prepare_cpu_fb_write();
            return 0;
        }
        tex[c] = (const uint8_t *)bufplce[c];
    }
    if (d3d_gpu_use_span4) {
        for (int c = 1; c < 4; c++) {
            if (slots[c] != slots[0]) {
                d3d_gpu_perf_note_cpu_fallback();
                d3d_gpu_prepare_cpu_fb_write();
                return 0;
            }
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
    } else {
        int path = masked ? PERF_PATH_MVLINE4 : PERF_PATH_VLINE4;
        uint8_t flags = OF_GPU_SPAN_COLORMAP;
        uint16_t tex_h_mask = column_tex_h_mask(v_shift);
        uint32_t t0 = perf_path_begin();
        if (masked)
            flags = (uint8_t)(flags | OF_GPU_SPAN_SKIP_ZERO);

        for (int c = 0; c < 4; c++) {
            int32_t t, tstep;
            to_16_16((uint32_t)vplce[c], (uint32_t)vince[c],
                     v_shift, &t, &tstep);
            emit_column_span_slot(framebuffer + c, num_pixels, shades[c],
                                  t, tstep, tex[c], tex_h_mask,
                                  flags, path, slots[c]);
        }
        perf_note_path_time(path, t0);
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
    if (d3d_gpu_force_cpu_spans) return 0;
    if (!d3d_gpu_use_spans || !d3d_gpu_present) return 0;
    /* hlineasm4 reads via globalpalwritten[shade|s]; resolve that row's
     * palookup slot so the emitted span can carry it explicitly. */
    extern uint8_t *globalpalwritten;
    if (d3d_gpu_shade_for(globalpalwritten) < 0) {
        d3d_gpu_perf_note_cpu_fallback();
        d3d_gpu_prepare_cpu_fb_write();
        return 0;
    }

    d3d_gpu_hline(dest_right, num_pixels, shade_x256, i4, i5,
                  asm1, asm2, width_bits, shifter, texture);
    return 1;
}

/* ----------------------------------------------------------------------
 * Stage 5: translucent paths (transluc[] BLEND unit).
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
    (void)reverse;  /* TRANSLUC_REV retired — rev path collapsed to fwd */
    emit_column_span(dest, num_pixels, shade, t, tstep, texture, tex_h_mask, flags,
                     PERF_PATH_TVLINE);
    perf_note_path_time(PERF_PATH_TVLINE, t0);
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
    uint32_t t0 = perf_path_begin();

    uint8_t flags = (uint8_t)(OF_GPU_SPAN_COLORMAP |
                              OF_GPU_SPAN_SKIP_ZERO |
                              OF_GPU_SPAN_TRANSLUC);
    (void)reverse;  /* TRANSLUC_REV retired — rev path collapsed to fwd */

    int32_t ta, tstepa, tb, tstepb;
    to_16_16(vplce_a, vince_a, v_shift, &ta, &tstepa);
    to_16_16(vplce_b, vince_b, v_shift, &tb, &tstepb);
    uint16_t tex_h_mask = column_tex_h_mask(v_shift);

    /* Two adjacent column spans.  BUILD's tvlineasm2 interleaves the
     * left/right writes per row; the GPU sequences them through the
     * same fragment pipe in submission order, so the FB-read for the
     * RMW blend on the right column always sees the left column's
     * just-written byte (and vice-versa across rows). */
    emit_column_span(dest_a,     num_pixels, shade_a, ta, tstepa, tex_a,
                     tex_h_mask, flags, PERF_PATH_TVLINE2);
    emit_column_span(dest_a + 1, num_pixels, shade_b, tb, tstepb, tex_b,
                     tex_h_mask, flags, PERF_PATH_TVLINE2);
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
     * fb_addr is dest - 1 so the first GPU write lands at dest[-1]. */
    of_gpu_span_t *span = d3d_gpu_alloc_span();
    span->fb_addr   = (uint32_t)(uintptr_t)(dest - 1);
    span->tex_addr  = (uint32_t)(uintptr_t)texture;
    span->s         = (int32_t)(by_frac >> 16);
    span->t         = (int32_t)(bx_frac >> 16);
    span->sstep     = -(int32_t)rs_yv2_full;
    span->tstep     = -(int32_t)rs_xv2_full;
    span->count     = (uint16_t)num_pixels;
    span->light     = (uint8_t)(shade & 0x3F);
    span->flags     = flags;
    span->colormap_id = current_span_colormap_id();
    span->fb_stride = (int16_t)-1;
    span->tex_width = (uint16_t)rs_tileHeight;
    span->tex_w_mask = 0;
    span->tex_h_mask = 0;
    span->sdivz = 0;
    span->tdivz = 0;
    span->zi_persp = 0;
    span->sdivz_step = 0;
    span->tdivz_step = 0;
    span->zi_step = 0;
    d3d_gpu_commit_span(path);
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
    (void)reverse;  /* TRANSLUC_REV retired — rev path collapsed to fwd */

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
    of_gpu_span_t *span = d3d_gpu_alloc_span();
    span->fb_addr   = (uint32_t)(uintptr_t)dest;
    span->tex_addr  = (uint32_t)(uintptr_t)texture;
    span->s         = (int32_t)(by_frac >> 16);     /* Y-axis init frac */
    span->t         = (int32_t)(bx_frac >> 16);     /* X-axis init frac */
    span->sstep     = (int32_t)yv_step;             /* full 16.16 yv */
    span->tstep     = (int32_t)xv_step;             /* full 16.16 xv */
    span->count     = (uint16_t)num_pixels;
    span->light     = (uint8_t)(shade & 0x3F);
    span->flags     = flags;
    span->colormap_id = current_span_colormap_id();
    span->fb_stride = (int16_t)bytesperline;
    span->tex_width = tile_height;
    span->tex_w_mask = 0;
    span->tex_h_mask = 0;
    span->sdivz = 0;
    span->tdivz = 0;
    span->zi_persp = 0;
    span->sdivz_step = 0;
    span->tdivz_step = 0;
    span->zi_step = 0;
    d3d_gpu_commit_span(PERF_PATH_SPRITE);
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
    of_gpu_span_t *span = d3d_gpu_alloc_span();
    span->fb_addr    = (uint32_t)(uintptr_t)dest;
    span->tex_addr   = (uint32_t)(uintptr_t)texture;
    span->s          = sp_s;
    span->t          = sp_t;
    span->sstep      = sp_sstep;
    span->tstep      = sp_tstep;
    span->count      = (uint16_t)num_pixels;
    span->light      = (uint8_t)((shade_x256 >> 8) & 0x3F);
    span->flags      = flags;
    span->colormap_id = current_span_colormap_id();
    span->fb_stride  = (int16_t)+1;
    span->tex_width  = tex_w;
    span->tex_w_mask = (uint16_t)(tex_w - 1);
    span->tex_h_mask = (uint16_t)(tex_h - 1);
    span->sdivz = 0;
    span->tdivz = 0;
    span->zi_persp = 0;
    span->sdivz_step = 0;
    span->tdivz_step = 0;
    span->zi_step = 0;
    d3d_gpu_commit_span(path);
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
    (void)reverse;  /* TRANSLUC_REV retired — rev path collapsed to fwd */
    emit_fwd_hline(dest, num_pixels, shade_x256, i2, i5, asm1, asm2,
                   width_bits, shifter, texture, flags, PERF_PATH_THLINE);
    perf_note_path_time(PERF_PATH_THLINE, t0);
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
    of_gpu_span_t *span = d3d_gpu_alloc_span();
    span->fb_addr    = (uint32_t)(uintptr_t)dest_right;
    span->tex_addr   = (uint32_t)(uintptr_t)texture;
    span->s          = sp_s;
    span->t          = sp_t;
    span->sstep      = sp_sstep;
    span->tstep      = sp_tstep;
    span->count      = (uint16_t)num_pixels;
    span->light      = (uint8_t)((shade_x256 >> 8) & 0x3F);
    span->flags      = OF_GPU_SPAN_COLORMAP;
    span->colormap_id = current_span_colormap_id();
    span->fb_stride  = (int16_t)-1;        /* walk left, matching SW dest-- */
    span->tex_width  = tex_w;
    span->tex_w_mask = (uint16_t)(tex_w - 1);
    span->tex_h_mask = (uint16_t)(tex_h - 1);
    span->sdivz = 0;
    span->tdivz = 0;
    span->zi_persp = 0;
    span->sdivz_step = 0;
    span->tdivz_step = 0;
    span->zi_step = 0;
    d3d_gpu_commit_span(PERF_PATH_HLINE);
    perf_note_path_time(PERF_PATH_HLINE, t0);
}
