/*
 * openfpgaOS Interact Demo
 *
 * Shows live values from the Analogue Pocket menu (interact.json).
 * Open the Pocket menu and change "App Option 1/2/3" to see values
 * update in real time.
 *
 * Interact variables are at SDRAM address 0x103FE000+:
 *   [0] = App Option 1 (On/Off)
 *   [1] = App Option 2 (slider 0-10)
 *   [2] = App Option 3 (Easy/Normal/Hard)
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>

static const char *opt1_names[] = { "Off", "On" };
static const char *opt3_names[] = { "Easy", "Normal", "Hard" };

int main(void) {
    printf("\033[2J\033[H");  /* clear screen */
    printf("\033[93m  Interact Demo\033[0m\n\n");
    printf("  Open Pocket menu and change\n");
    printf("  App Option 1/2/3 to see\n");
    printf("  values update live.\n\n");

    uint32_t prev[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };

    while (1) {
        uint32_t v0 = of_interact_get(0);
        uint32_t v1 = of_interact_get(1);
        uint32_t v2 = of_interact_get(2);

        /* Only redraw when values change */
        if (v0 != prev[0] || v1 != prev[1] || v2 != prev[2]) {
            prev[0] = v0;
            prev[1] = v1;
            prev[2] = v2;

            printf("\033[8;3H");  /* row 8, col 3 */

            /* Option 1: On/Off */
            const char *s1 = (v0 < 2) ? opt1_names[v0] : "???";
            printf("Option 1: \033[92m%-6s\033[0m  (raw: %d)\n", s1, (int)v0);

            /* Option 2: slider */
            printf("   Option 2: \033[92m%-3d\033[0m     (slider)\n", (int)v1);

            /* Option 2: visual bar */
            printf("   [");
            for (int i = 0; i < 10; i++)
                printf("%c", i < (int)v1 ? '#' : '.');
            printf("]\n");

            /* Option 3: difficulty */
            const char *s3 = (v2 < 3) ? opt3_names[v2] : "???";
            printf("   Option 3: \033[92m%-8s\033[0m (raw: %d)\n", s3, (int)v2);
        }

        usleep(50 * 1000);
    }

    return 0;
}
