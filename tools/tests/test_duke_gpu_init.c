#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "gpu_features.h"

struct of_capabilities { uint32_t gpu_base, hw_features; };
static struct of_capabilities caps;
static int no_caps, probe_ok, init_calls, probe_calls, upload_calls;
static int d3d_gpu_use_spans, d3d_gpu_present, gpu_use_column_list;
static int gpu_perf_hw_valid;
static uint32_t gpu_perf_last_report_ms;
static const uint8_t *pending_transluc_table;
static uint32_t pending_transluc_size;
static void perf_reset_interval(void) {}
static uint32_t of_time_ms(void) { return 123; }
static const struct of_capabilities *of_get_caps(void) { return no_caps ? NULL : &caps; }
static int of_has_feature(unsigned f) { return (caps.hw_features & f) != 0; }
static void of_gpu_init(void) { init_calls++; }
static int d3d_gpu_probe_column_list(void) { probe_calls++; return probe_ok; }
static void d3d_gpu_upload_transluc(const uint8_t *p, uint32_t n)
{ assert(p && n == 65536); upload_calls++; }
#define printf(...) ((void)0)
#include "duke_init.h"
#undef printf

int main(void)
{
    const unsigned features[] = {OF_HW_GPU_SPAN, OF_HW_GPU_FRAGPIPE,
        OF_HW_GPU_PARAM_SPAN_LIST, OF_HW_GPU_SPAN_GROUP, OF_HW_GPU_COLUMN_LIST};
    for (unsigned bits = 0; bits < 32; bits++)
    for (unsigned mode = 0; mode < 5; mode++) {
        caps.hw_features = 0;
        for (unsigned i = 0; i < 5; i++) if (bits & (1u << i)) caps.hw_features |= features[i];
        no_caps = mode == 0;
        caps.gpu_base = mode == 1 ? 0 : 0x40000000u;
        d3d_gpu_use_spans = mode != 2;
        probe_ok = mode != 3;
        d3d_gpu_present = gpu_use_column_list = 0;
        init_calls = probe_calls = upload_calls = 0;
        pending_transluc_table = (const uint8_t *)&caps;
        pending_transluc_size = 65536;
        d3d_gpu_init();
        int enabled = mode >= 3 && (bits & 15) == 15;
        assert(d3d_gpu_present == enabled && init_calls == enabled);
        assert(upload_calls == enabled);
        assert(probe_calls == (enabled && (bits & 16) != 0));
        assert(gpu_use_column_list == (probe_calls && probe_ok));
    }
    puts("PASS 160 GPU initialization cases: capabilities, disable switch, probe fallback");
}
