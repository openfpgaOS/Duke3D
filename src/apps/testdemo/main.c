/*
 * openfpgaOS Kernel Test Suite
 * Tests syscalls, malloc, file I/O, terminal, timer, cache.
 * Two-column display. On failure: pauses, then shows summary page.
 */

#include "test.h"

int pass_count, fail_count;
char __buf[80];

/* ================================================================
 * Failure log — records up to MAX_FAILS for the summary page
 * ================================================================ */
#define MAX_FAILS 32
#define MAX_DETAIL 32
static struct {
    const char *name;
    char detail[MAX_DETAIL];
} fail_log[MAX_FAILS];
static int fail_log_count;

/* ================================================================
 * Two-column layout state
 * ================================================================ */
#define COL_W   19      /* characters per column */
#define COL1_X  1       /* left column x (0-based) */
#define COL2_X  21      /* right column x */
#define ROW_TOP 4       /* first data row */
#define ROW_MAX 28      /* max rows before overflow */

static int col_row;     /* current row within columns */
static int col_side;    /* 0 = left, 1 = right */
static int section_pass;
static int section_fail;
static const char *section_name;
static int any_fail_this_run;

static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void next_slot(void) {
    if (col_side == 0) {
        col_side = 1;
    } else {
        col_side = 0;
        col_row++;
    }
}

static void section_update(void) {
    int p = pass_count - section_pass;
    int f = fail_count - section_fail;
    int row = ROW_TOP + col_row;
    int col = col_side == 0 ? COL1_X : COL2_X;
    move_cursor(row, col);
    if (f == 0)
        printf("%-12s%3d \033[92mok\033[0m", section_name, p);
    else
        printf("%-12s%3d \033[91m%dF\033[0m", section_name, p + f, f);
}

void test_pass(const char *name) {
    (void)name;
    pass_count++;
    section_update();
}

void test_fail(const char *name, const char *detail) {
    fail_count++;
    any_fail_this_run = 1;
    section_update();

    /* Log for summary page — copy detail into per-entry storage so
     * subsequent failures don't overwrite our message via __buf. */
    if (fail_log_count < MAX_FAILS) {
        fail_log[fail_log_count].name = name;
        int dlen = strlen(detail);
        if (dlen >= MAX_DETAIL) dlen = MAX_DETAIL - 1;
        memcpy(fail_log[fail_log_count].detail, detail, dlen);
        fail_log[fail_log_count].detail[dlen] = 0;
        fail_log_count++;
    }
}

void section_start(const char *name) {
    section_name = name;
    section_pass = pass_count;
    section_fail = fail_count;
    int row = ROW_TOP + col_row;
    int col = col_side == 0 ? COL1_X : COL2_X;
    move_cursor(row, col);
    printf("%-12s...", name);
}

void section_end(void) {
    section_update();
    next_slot();
}

/* Wait for A button press */
static void wait_press(void) {
    /* Wait for release first */
    while (1) {
        of_input_poll();
        if (!of_btn(OF_BTN_A)) break;
        usleep(16000);
    }
    /* Wait for press */
    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) break;
        usleep(16000);
    }
}

/* Show failure summary page */
static void show_fail_summary(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[91mFailed Tests: %d\033[0m\n", fail_log_count);
    printf("  --------------------------------\n");

    for (int i = 0; i < fail_log_count && i < 24; i++) {
        /* Truncate detail to fit screen width (40 - 4 - 14 - 1 = 21 chars) */
        char det[22];
        int dlen = strlen(fail_log[i].detail);
        if (dlen > 21) dlen = 21;
        memcpy(det, fail_log[i].detail, dlen);
        det[dlen] = 0;
        printf("  \033[91m%2d\033[0m %-14s%s\n", i + 1, fail_log[i].name, det);
    }
    if (fail_log_count > 24)
        printf("  ... and %d more\n", fail_log_count - 24);

    printf("\n  Press A to continue\n");
    wait_press();
}

/* ================================================================
 * Test sections — grouped by function
 * ================================================================ */
typedef void (*test_fn)(void);

static const test_fn tests[] = {
    test_timer,
    test_timer_edge,
    test_malloc,
    test_malloc_edge,
    test_malloc_free,
    test_memset_stack,
    test_psram_memory,
    test_cram0_256k,
    test_cache_primitives,
    test_cache,
    test_cache_cram0,
    test_cache_cram1,
    test_file_slots,
    test_file_negative,
    test_file_io,
    test_saves,
    test_posix_saves,
    test_dma_cache,
    test_posix_file_io,
    test_lseek_readahead,
    test_lseek_large_read,
    test_oversize_read,
    test_file_limit,
    test_shutdown,
    test_mixer,
    test_mixer_adv,
    test_mixer_stress,
    test_opl3,
    test_midi,
    test_net,
    test_interact,
    test_audio,
    test_audio_stream,
    test_printf,
    test_printf_edge,
    test_string,
    test_string_edge,
    test_lzw,
    test_version,
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))
#define NUM_ITERATIONS 10

int main(void) {
    for (int iteration = 0; iteration < NUM_ITERATIONS; iteration++) {
        pass_count = 0;
        fail_count = 0;
        fail_log_count = 0;
        col_row = 0;
        col_side = 0;
        any_fail_this_run = 0;

        printf("\033[2J\033[H");
        printf("\n  \033[93mopenfpgaOS Test Suite  [%d/%d]\033[0m\n",
               iteration + 1, NUM_ITERATIONS);
        printf("  --------------------------------\n");

        for (unsigned i = 0; i < NUM_TESTS; i++) {
            tests[i]();
            /* Wrap to next page if columns overflow */
            if (col_row >= ROW_MAX) {
                int row = ROW_TOP + col_row + 1;
                move_cursor(row, 1);
                printf("  ... more (A to continue)");
                wait_press();
                col_row = 0;
                col_side = 0;
                printf("\033[2J\033[H");
                printf("\n  \033[93mopenfpgaOS Test Suite  [%d/%d] cont.\033[0m\n",
                       iteration + 1, NUM_ITERATIONS);
                printf("  --------------------------------\n");
            }
        }

        /* Summary line */
        int row = ROW_TOP + col_row + 1;
        move_cursor(row, 1);
        printf("  --------------------------------\n");
        printf("  Total: %d passed", pass_count);
        if (fail_count > 0)
            printf(", \033[91m%d failed\033[0m", fail_count);
        printf("\n");

        if (any_fail_this_run) {
            printf("  \033[91mFAILED\033[0m — Press A for details\n");
            wait_press();
            show_fail_summary();
            break;
        }

        if (iteration < NUM_ITERATIONS - 1) {
            printf("  \033[92mPASS\033[0m — next in 2s\n");
            usleep(2000 * 1000);
        } else {
            printf("  \033[92mALL %d ITERATIONS PASSED\033[0m\n", NUM_ITERATIONS);
        }
    }

    while (1)
        usleep(100 * 1000);
    return 0;
}
