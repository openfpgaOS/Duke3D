/*
 * openfpgaOS Slot Demo
 * Displays registered file slots with a box-drawn table.
 */

#include "of.h"
#include <stdio.h>

static void draw_hline(int w, int sep, char left, char mid, char right) {
    printf("  %c", left);
    for (int i = 1; i < w - 1; i++)
        printf("%c", (i == sep) ? mid : ACS_HLINE);
    printf("%c\n", right);
}

int main(void) {
    /* FTAB auto-populates from Chip32 loader — no manual registration needed */

    printf("\033[2J\033[H");  /* clear screen, cursor home */

    int count = of_file_slot_count();

    printf("\n  openfpgaOS File Slot Registry\n\n");
    printf("  Registered file slots: %d\n\n", count);

    if (count == 0) {
        printf("  (none)\n");
    } else {
        int sep = 7;
        int w = 37;

        draw_hline(w, sep, ACS_ULCORNER, ACS_TTEE, ACS_URCORNER);

        printf("  %c  ID  %c  Filename                  %c\n",
               ACS_VLINE, ACS_VLINE, ACS_VLINE);

        draw_hline(w, sep, ACS_LTEE, ACS_PLUS, ACS_RTEE);

        for (int i = 0; i < count; i++) {
            of_file_slot_t slot;
            if (of_file_slot_get(i, &slot) < 0)
                continue;
            printf("  %c %3d  %c  %-25s %c\n",
                   ACS_VLINE, (int)slot.slot_id, ACS_VLINE,
                   slot.filename, ACS_VLINE);
        }

        draw_hline(w, sep, ACS_LLCORNER, ACS_BTEE, ACS_LRCORNER);
    }

    while (1)
        of_delay_ms(100);

    return 0;
}
