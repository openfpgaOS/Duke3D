/*
 * openfpgaOS MIDI Demo Application
 * Plays a MIDI file (loaded from data slot 3) through the OPL3 (YMF262) FM synthesizer.
 * Uses the of_midi library for parsing and playback.
 *
 * Controls:
 *   START   = play/pause
 *   SELECT  = restart
 */

#include "of.h"
#include <stdio.h>

/* Maximum MIDI file size we support */
#define MIDI_MAX_SIZE   (256 * 1024)

/* Buffer for the loaded MIDI file (aligned for DMA) */
static uint8_t midi_buf[MIDI_MAX_SIZE] __attribute__((aligned(512)));
static uint32_t midi_len;

static int load_midi_file(void) {
    FILE *f = fopen("music.mid", "rb");
    if (!f)
        return -1;

    size_t n = fread(midi_buf, 1, MIDI_MAX_SIZE, f);
    fclose(f);

    if (n < 14)
        return -2;

    if (midi_buf[0] != 'M' || midi_buf[1] != 'T' ||
        midi_buf[2] != 'h' || midi_buf[3] != 'd')
        return -10;

    midi_len = (uint32_t)n;
    return 0;
}

int main(void) {
    of_file_slot_register(3, "music.mid");

    printf("\033[2J\033[H");
    printf("    openfpgaOS MIDI Player\n");
    printf("    ======================\n\n");
    printf(" Loading MIDI file...\n");

    int rc = load_midi_file();
    if (rc < 0) {
        printf(" Error loading MIDI! rc=%d\n", rc);
        while (1) {}
    }

    printf(" Loaded %lu bytes\n", (unsigned long)midi_len);

    rc = of_midi_init();
    if (rc < 0) {
        printf(" MIDI init failed! rc=%d\n", rc);
        while (1) {}
    }

    rc = of_midi_play(midi_buf, midi_len, 1 /* loop */);
    if (rc < 0) {
        printf(" MIDI play failed! rc=%d\n", rc);
        while (1) {}
    }

    printf(" Playing (18-ch OPL3)...\n\n");
    printf(" START=play/pause  SEL=restart\n");

    int paused = 0;
    int volume = 255;

    while (1) {
        of_input_poll();
        of_input_state_t state;
        of_input_state(0, &state);

        if (state.buttons_pressed & OF_BTN_START) {
            if (!of_midi_playing()) {
                /* Restart after finished */
                of_midi_play(midi_buf, midi_len, 1);
                paused = 0;
                printf("\033[9;2H[PLAYING]  ");
            } else if (paused) {
                of_midi_resume();
                paused = 0;
                printf("\033[9;2H[PLAYING]  ");
            } else {
                of_midi_pause();
                paused = 1;
                printf("\033[9;2H[PAUSED]   ");
            }
        }

        if (state.buttons_pressed & OF_BTN_SELECT) {
            of_midi_stop();
            of_midi_play(midi_buf, midi_len, 1);
            paused = 0;
            printf("\033[9;2H[PLAYING]  ");
        }

        /* L1/R1 adjust volume */
        if (state.buttons_pressed & OF_BTN_L1) {
            volume -= 32;
            if (volume < 0) volume = 0;
            of_midi_set_volume(volume);
            printf("\033[10;2HVol: %3d/255  ", volume);
        }
        if (state.buttons_pressed & OF_BTN_R1) {
            volume += 32;
            if (volume > 255) volume = 255;
            of_midi_set_volume(volume);
            printf("\033[10;2HVol: %3d/255  ", volume);
        }

        of_midi_pump();
        of_delay_ms(1);
    }

    return 0;
}
