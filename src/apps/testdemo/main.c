/*
 * openfpgaOS Kernel Test Suite
 * Tests syscalls, malloc, file I/O, terminal, timer.
 * Runs multiple iterations to catch intermittent issues.
 */

#include "test.h"

int pass_count, fail_count;
char __buf[80];

static int section_pass;
static int section_fail;
static const char *section_name;

static void section_update(void) {
    int p = pass_count - section_pass;
    int f = fail_count - section_fail;
    if (f == 0)
        printf("\r  %-16s \033[92m%d\033[0m ", section_name, p);
    else
        printf("\r  %-16s \033[92m%d\033[0m \033[91m%d fail\033[0m ", section_name, p, f);
}

void test_pass(const char *name) {
    (void)name;
    pass_count++;
    section_update();
}

void test_fail(const char *name, const char *detail) {
    fail_count++;
    section_update();
    printf("\n    \033[91mFAIL\033[0m %s: %s", name, detail);
}

void section_start(const char *name) {
    section_name = name;
    section_pass = pass_count;
    section_fail = fail_count;
    printf("  %-16s ", name);
}

void section_end(void) {
    int p = pass_count - section_pass;
    int f = fail_count - section_fail;
    if (f == 0)
        printf("\r  %-16s \033[92m%d ok\033[0m\n", section_name, p);
    else
        printf("\r  %-16s \033[91m%d ok %d fail\033[0m\n", section_name, p - f, f);
}

/* --- Main --- */
#define NUM_ITERATIONS 10

int main(void) {
    int iteration;
    for (iteration = 0; iteration < NUM_ITERATIONS; iteration++) {
        pass_count = 0;
        fail_count = 0;

        printf("\033[2J\033[H");
        printf("\n  \033[93mopenfpgaOS Test Suite  [%d/%d]\033[0m\n",
               iteration + 1, NUM_ITERATIONS);
        printf("  --------------------------------\n");

        test_timer();
        test_timer_edge();
        test_malloc();
        test_malloc_edge();
        test_malloc_free();
        test_memset_stack();
        test_psram_memory();
        test_cram0_256k();
        test_file_slots();
        test_file_negative();
        test_file_io();
        test_saves();
        test_posix_saves();
        test_dma_cache();
        test_posix_file_io();
        test_lseek_readahead();
        test_lseek_large_read();
        test_oversize_read();
        test_file_limit();
        test_shutdown();
        test_mixer();
        test_net();
        test_interact();
        test_audio();
        test_printf();
        test_printf_edge();
        test_string();
        test_string_edge();
        test_lzw();
        test_version();

        printf("  --------------------------------\n");
        printf("  Total: %d passed", pass_count);
        if (fail_count > 0)
            printf(", \033[91m%d failed\033[0m", fail_count);
        printf("\n");

        if (fail_count > 0) {
            printf("  \033[91mFAILED\033[0m\n");
            break;
        }

        if (iteration < NUM_ITERATIONS - 1) {
            printf("  \033[92mPASS\033[0m -- next in 2s\n");
            usleep(2000 * 1000);
        } else {
            printf("  \033[92mALL %d ITERATIONS PASSED\033[0m\n", NUM_ITERATIONS);
        }
    }

    while (1)
        usleep(100 * 1000);
    return 0;
}
