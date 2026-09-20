#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "gpu_types.h"

#define GPU_CMD_DRAW_PARAM_SPAN_LIST 0x48
#define GPU_CMD_DRAW_COLUMN_LIST 0x4c
#define OF_HW_GPU_SPAN_GROUP 1
#define OF_HW_GPU_COLUMN_LIST 2
#define d3d_gpu_perf_enable 0
#define SPIN_TAG(tag) ((void)0)

static int of_has_feature(unsigned bit) { (void)bit; return 1; }
static int gpu_use_column_list, d3d_gpu_force_affine_columns;
static int gpu_emit_stalled, reserve_fail;
static uint32_t gpu_emit_dropped_lanes;
static int _gpu_span_hdr_valid;
static struct {
    uint32_t flushes, batches, max_batch, max_batch_submit_us;
    uint64_t batch_submit_us;
} gpu_perf;
static uint32_t of_time_us(void) { assert(0); return 0; }
static uint32_t perf_dt_us(uint32_t t) { assert(0); return t; }
static void perf_add_time(uint64_t *a, uint32_t *b, uint32_t t)
{ (void)a; (void)b; (void)t; assert(0); }

typedef struct { unsigned at, kind, value; } event_t;
typedef struct {
    uint32_t words[16384];
    unsigned cursor, reserved, staged;
    event_t events[4096];
    unsigned event_count;
} trace_t;
static trace_t traces[2], *trace;
static uint64_t words_checked, events_checked;
#ifdef D3D_BENCH
static uint64_t instruction_total;
static inline uint32_t instructions(void)
{
    uint32_t n;
    __asm__ volatile(".insn i 0x73, 2, %0, x0, -1022" : "=r"(n) :: "memory");
    return n;
}
#endif
static void event(unsigned kind, unsigned value)
{
    assert(trace->event_count < 4096);
    trace->events[trace->event_count++] = (event_t){trace->cursor, kind, value};
}
static int d3d_gpu_reserve_or_drop(uint32_t bytes)
{
    event(1, bytes);
    if (reserve_fail) { gpu_emit_stalled = 1; return 0; }
    return 1;
}
static void of_gpu_kick(void) { event(2, 0); }
static void _gpu_cmd_header(unsigned opcode, unsigned words)
{
    assert(trace->cursor == trace->reserved);
    assert(trace->cursor + words + 1 <= 16384);
    if (trace->staged + words + 1 > 4095) {
        event(3, trace->staged);
        trace->staged = 0;
    }
    trace->staged += words + 1;
    trace->reserved = trace->cursor + words + 1;
    trace->words[trace->cursor++] = opcode << 24 | words;
}
static uint32_t *_gpu_ring_claim(void) { return trace->words + trace->cursor; }
static void _gpu_ring_commit(unsigned words)
{ trace->cursor += words; assert(trace->cursor == trace->reserved); }

#include "sdk_emit.h"
#include "duke_batch.h"
#include "duke_math.h"

/* Independent queue oracle: stage complete input lanes and use the public
 * production SDK packers. This also checks order and grouping when command
 * kinds or any shared header field changes. */
typedef struct {
    uint32_t fb, tex;
    int32_t s, t, ds, dt, step;
    uint16_t count, width, wm, hm;
    uint8_t light, flags, cmap;
} lane_t;
static lane_t ref_lanes[8];
static unsigned ref_count;
static int ref_column;

static void ref_flush(void)
{
    unsigned n = ref_count;
    if (!n) return;
    if (gpu_emit_stalled ||
        !d3d_gpu_reserve_or_drop((((n + 3) / 4) * 5 + n * (ref_column ? 5 : 7)) * 4)) {
        gpu_emit_dropped_lanes += n;
        ref_count = 0;
        return;
    }
    lane_t *key = ref_lanes;
    of_gpu_affine_span_group_t a = {0};
    of_gpu_column_list_group_t c = {0};
    a.lane_count = c.lane_count = n;
    a.flags = c.flags = key->flags;
    a.tex_width = c.tex_width = key->width;
    a.tex_w_mask = c.tex_w_mask = key->wm;
    a.tex_h_mask = c.tex_h_mask = key->hm;
    a.fb_step = c.fb_step = key->step;
    for (unsigned i = 0; i < n; i++) {
        lane_t *p = &ref_lanes[i];
        a.fb_addr[i] = c.fb_addr[i] = p->fb;
        a.tex_addr[i] = c.tex_addr[i] = p->tex;
        a.count[i] = c.count[i] = p->count;
        a.light[i] = c.light[i] = p->light;
        a.colormap_id[i] = c.colormap_id[i] = p->cmap;
        a.t[i] = c.t[i] = p->t;
        a.tstep[i] = c.tstep[i] = p->dt;
        a.s[i] = p->s; a.sstep[i] = p->ds;
    }
    if (ref_column) of_gpu_draw_column_list(&c);
    else of_gpu_draw_affine_span_group(&a);
    ref_count = 0;
    of_gpu_kick();
}
static void ref_queue(lane_t p)
{
    int column = gpu_use_column_list && !d3d_gpu_force_affine_columns &&
                 p.s == 0 && p.ds == 0;
    lane_t *key = ref_lanes;
    if (ref_count && (column != ref_column || p.flags != key->flags ||
        p.width != key->width || p.wm != key->wm || p.hm != key->hm || p.step != key->step))
        ref_flush();
    ref_column = column;
    ref_lanes[ref_count++] = p;
    if (ref_count == 8) ref_flush();
}
static uint32_t seed = 0x418035efu;
static uint32_t rnd(void)
{ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void check_batches(void)
{
    static const uint16_t counts[] = {0, 1, 200, 320, 4095, 4096, 65535};
    unsigned cases = 0;
    for (unsigned sequence = 0; sequence < 12000; sequence++) {
        lane_t lanes[256];
        unsigned actions[256];
        lane_t p = {0};
        for (unsigned i = 0; i < 256; i++) {
            if (i % (1 + sequence % 17) == 0) {
                p.s = rnd() & 1 ? (int32_t)rnd() : 0;
                p.ds = p.s ? (int32_t)rnd() : 0;
                p.flags = rnd(); p.width = rnd(); p.wm = rnd(); p.hm = rnd();
                p.step = (int[]){0, 1, -1, 320, -320, INT_MIN, INT_MAX}[rnd() % 7];
            }
            p.fb = rnd(); p.tex = rnd(); p.t = rnd(); p.dt = rnd();
            p.count = sequence % 19 == 0 ? 0 : counts[rnd() % 7];
            p.light = rnd(); p.cmap = rnd();
            lanes[i] = p;
            actions[i] = rnd();
        }
        unsigned dropped[2], valid[2];
        for (unsigned side = 0; side < 2; side++) {
            trace = &traces[side];
            memset(trace, 0, sizeof(*trace));
            trace->staged = sequence % 4096;
            ref_count = affine_batch_count = column_batch_count = 0;
            /* Poison unused lanes: no reliance on zero-initialized storage. */
#if TEST_PACKED_BATCHES
            memset(affine_lanes, 0xa5, sizeof(affine_lanes));
            memset(column_lanes, 0x5a, sizeof(column_lanes));
#endif
            gpu_emit_stalled = 0; gpu_emit_dropped_lanes = 0;
            _gpu_span_hdr_valid = 1;
            gpu_use_column_list = sequence & 1;
#ifdef D3D_BENCH
            uint32_t t0 = instructions();
#endif
            for (unsigned i = 0; i < 256; i++) {
                lane_t q = lanes[i];
                unsigned action = actions[i];
                d3d_gpu_force_affine_columns = (action & 31) == 0;
                reserve_fail = sequence % 7 == 0 && (action & 7) == 0;
                if (i % 64 == 0) gpu_emit_stalled = 0;
                if (side == 0) ref_queue(q);
                else d3d_gpu_queue_affine_span(q.fb, q.tex, q.s, q.t, q.ds, q.dt,
                        q.count, q.light, q.flags, q.cmap, q.step, q.width, q.wm, q.hm);
                if ((action & 15) == 0) {
                    if (side == 0) ref_flush(); else d3d_gpu_flush_batch();
                }
            }
            if (side == 0) ref_flush(); else d3d_gpu_flush_batch();
#ifdef D3D_BENCH
            if (side == 1) instruction_total += instructions() - t0;
#endif
            dropped[side] = gpu_emit_dropped_lanes;
            valid[side] = _gpu_span_hdr_valid;
        }
        assert(dropped[0] == dropped[1] && valid[0] == valid[1]);
        assert(traces[0].cursor == traces[1].cursor);
        assert(traces[0].event_count == traces[1].event_count);
        assert(!memcmp(traces[0].words, traces[1].words, traces[0].cursor * 4));
        assert(!memcmp(traces[0].events, traces[1].events, traces[0].event_count * sizeof(event_t)));
        words_checked += traces[0].cursor;
        events_checked += traces[0].event_count;
        cases++;
    }
    printf("PASS %u GPU sequences, %llu words, %llu reserve/kick/staging events\n",
           cases, (unsigned long long)words_checked, (unsigned long long)events_checked);
}
static unsigned math_cases;
static void check_div(int32_t n, int32_t d)
{
    if (!d) return;
    int32_t reference = (int32_t)(((int64_t)n * 4096) / d);
    assert(build_divscale12(n, d) == reference);
    math_cases++;
}
static void check_math(void)
{
    const int32_t edges[] = {INT_MIN, INT_MIN + 1, INT_MAX, INT_MAX - 1,
        -1048577, -1048576, -1048575, -4096, -1, 0, 1, 4095, 4096,
        1048575, 1048576, 1048577};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(*edges); i++)
        for (unsigned j = 0; j < sizeof(edges) / sizeof(*edges); j++)
            check_div(edges[i], edges[j]);
    for (int32_t n = -2048; n <= 2048; n++)
        for (int32_t d = -256; d <= 256; d++) check_div(n, d);
    for (unsigned i = 0; i < 2000000; i++) {
        check_div((int32_t)rnd(), (int32_t)rnd());
        check_div((int32_t)(rnd() & 0x1fffff) - 1048576, (int32_t)rnd());
    }
    printf("PASS exact signed 20.12 division: %u cases including boundaries\n", math_cases);
}
int main(void)
{
    check_batches();
#ifdef D3D_BENCH
    printf("GPU queue/flush instructions: %llu\n", (unsigned long long)instruction_total);
    puts("BENCH_COMPLETE");
#else
    check_math();
#endif
    return 0;
}
