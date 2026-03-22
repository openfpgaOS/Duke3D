/*
 * pocket_audio.c — Sound playback for Duke3D on openfpgaOS
 *
 * Uses the OS mixer (of_mixer.h) and codec (of_codec.h) APIs.
 * Game-specific GRP loading is kept here; mixing/resampling/VOC
 * parsing are handled by the OS.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "of.h"
#include "of_file.h"
#include "of_mixer.h"
#include "of_codec.h"

/* Game headers */
#include "duke3d.h"
#include "filesystem.h"

static int audio_initialized = 0;

/* Forward declaration for idle hook registration */
void pocket_audio_pump(void);

/* ================================================================
 * Public API
 * ================================================================ */

void pocket_audio_init(void)
{
    of_mixer_init(4, 48000);
    audio_initialized = 1;
}

/*
 * Play a sound effect. Loads from GRP on first use.
 * Returns voice handle or -1.
 */
int pocket_sound_play(int num, int priority, int volume)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    /* Load sound from GRP if not cached */
    if (Sound[num].ptr == NULL) {
        short fp = TCkopen4load(sounds[num], 0);
        if (fp == -1) return -1;
        int32_t l = kfilelength(fp);
        soundsiz[num] = l;
        Sound[num].ptr = (uint8_t *)malloc(l);
        if (Sound[num].ptr) {
            kread(fp, Sound[num].ptr, l);
        }
        kclose(fp);
        if (Sound[num].ptr == NULL) return -1;
    }

    /* Parse VOC via OS codec */
    of_codec_result_t result;
    if (of_codec_parse_voc(Sound[num].ptr, soundsiz[num], &result) < 0)
        return -1;
    if (result.pcm == NULL || result.pcm_len == 0)
        return -1;

    return of_mixer_play(result.pcm, result.pcm_len, result.sample_rate,
                         priority, volume);
}

void pocket_sound_stop_all(void)
{
    of_mixer_stop_all();
}

/*
 * Pump the mixer. Called from sampletimer() as a backup —
 * the OS also pumps during DMA waits via of_mixer_pump()
 * in file_wait_complete().
 */
void pocket_audio_pump(void)
{
    if (!audio_initialized) return;
    of_mixer_pump();
}
