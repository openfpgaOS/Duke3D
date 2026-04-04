/*
 * d3d_audio.c — Sound playback for Duke3D on openfpgaOS
 *
 * Routes all game audio through the OS hardware mixer (of_mixer.h).
 * Handles VOC/WAV parsing, CRAM1 upload, voice tracking, and
 * completion callbacks via polling.
 *
 * Missing OS features (looping, pitch shift) are stubbed — sounds
 * play once at original rate until the OS mixer adds support.
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

/* ================================================================
 * CRAM1 bump allocator for decoded PCM samples
 * Samples must live in CRAM1 (0x31000000+) for the hardware mixer.
 * ================================================================ */
#define CRAM1_BASE  0x31000000
#define CRAM1_SIZE  (16 * 1024 * 1024)  /* 16MB CRAM1 */

static uint32_t cram1_offset = 0;

static void *cram1_alloc(uint32_t size) {
    /* Word-align */
    size = (size + 3) & ~3;
    if (cram1_offset + size > CRAM1_SIZE)
        return NULL;
    void *ptr = (void *)(CRAM1_BASE + cram1_offset);
    cram1_offset += size;
    return ptr;
}

/* ================================================================
 * Per-sound cached decode: raw PCM in CRAM1
 * ================================================================ */
typedef struct {
    int16_t *pcm;           /* CRAM1 pointer to decoded 16-bit signed PCM */
    uint32_t sample_count;
    uint32_t sample_rate;
} decoded_sound_t;

static decoded_sound_t decoded[NUM_SOUNDS];

/* ================================================================
 * Voice tracking for completion callbacks
 * ================================================================ */
#define MAX_ACTIVE_VOICES 32

typedef struct {
    int      voice;      /* OS mixer voice index (0-31), or -1 */
    int      sound_num;  /* Duke sound number */
    int      has_owner;  /* 1 = xyzsound (tracked in SoundOwner), 0 = fire-and-forget */
} active_voice_t;

static active_voice_t active_voices[MAX_ACTIVE_VOICES];

static void init_voice_tracking(void) {
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        active_voices[i].voice = -1;
}

static void track_voice(int voice, int sound_num, int has_owner) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].voice = voice;
    active_voices[voice].sound_num = sound_num;
    active_voices[voice].has_owner = has_owner;
}

static void untrack_voice(int voice) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].voice = -1;
}

/* ================================================================
 * Public API
 * ================================================================ */

void d3d_audio_init(void)
{
    of_mixer_init(MAX_ACTIVE_VOICES, OF_MIXER_OUTPUT_RATE);
    init_voice_tracking();
    memset(decoded, 0, sizeof(decoded));
    cram1_offset = 0;
    audio_initialized = 1;
}

/* Decode a sound from GRP and upload to CRAM1 (cached). */
static int ensure_decoded(int num)
{
    if (decoded[num].pcm != NULL)
        return 1;  /* already cached */

    /* Load raw VOC/WAV from GRP if not in memory */
    if (Sound[num].ptr == NULL) {
        short fp = TCkopen4load(sounds[num], 0);
        if (fp == -1) return 0;
        int32_t l = kfilelength(fp);
        soundsiz[num] = l;
        Sound[num].ptr = (uint8_t *)malloc(l);
        if (Sound[num].ptr == NULL) { kclose(fp); return 0; }
        kread(fp, Sound[num].ptr, l);
        kclose(fp);
    }

    /* Parse VOC/WAV via OS codec */
    of_codec_result_t result;
    int rc;
    if (*Sound[num].ptr == 'C')
        rc = of_codec_parse_voc(Sound[num].ptr, soundsiz[num], &result);
    else
        rc = of_codec_parse_wav(Sound[num].ptr, soundsiz[num], &result);

    if (rc < 0 || result.pcm == NULL || result.pcm_len == 0)
        return 0;

    /* Allocate CRAM1 and copy decoded PCM */
    uint32_t byte_len = result.pcm_len * sizeof(int16_t);
    int16_t *cram_ptr = (int16_t *)cram1_alloc(byte_len);
    if (cram_ptr == NULL)
        return 0;

    memcpy(cram_ptr, result.pcm, byte_len);

    decoded[num].pcm = cram_ptr;
    decoded[num].sample_count = result.pcm_len;
    decoded[num].sample_rate = result.sample_rate;

    /* Keep Sound[num].ptr alive — pan3dsound and other engine code
     * checks ptr != NULL to avoid re-loading from GRP every frame. */

    return 1;
}

/*
 * Play a sound effect. Returns voice handle or -1.
 */
int d3d_sound_play(int num, int priority, int volume)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    if (!ensure_decoded(num))
        return -1;

    int voice = of_mixer_play((const uint8_t *)decoded[num].pcm,
                              decoded[num].sample_count,
                              decoded[num].sample_rate,
                              priority, volume);

    if (voice >= 0)
        track_voice(voice, num, 0);

    return voice;
}

/*
 * Play with 3D positioning. Angle/distance mapped to volume + pan.
 * Pan stubbed until of_mixer_set_pan is available.
 */
int d3d_sound_play_3d(int num, int priority, int angle, int distance)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    if (!ensure_decoded(num))
        return -1;

    /* Map distance to volume (0=far, 255=near) */
    int volume = 255 - distance;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    int voice = of_mixer_play((const uint8_t *)decoded[num].pcm,
                              decoded[num].sample_count,
                              decoded[num].sample_rate,
                              priority, volume);

    if (voice >= 0) {
        track_voice(voice, num, 0);
        /* TODO: of_mixer_set_pan(voice, angle) when OS supports it */
    }

    return voice;
}

void d3d_sound_stop(int voice)
{
    if (!audio_initialized) return;
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    of_mixer_stop(voice);
    untrack_voice(voice);
}

void d3d_sound_stop_all(void)
{
    of_mixer_stop_all();
    init_voice_tracking();
}

void d3d_sound_set_volume(int voice, int volume)
{
    if (!audio_initialized) return;
    if (voice < 0) return;
    of_mixer_set_volume(voice, volume);
}

void d3d_sound_set_owned(int voice)
{
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].has_owner = 1;
}

/*
 * Poll for completed voices and fire Duke's TestCallBack.
 * Called once per frame from _nextpage().
 */
void d3d_audio_pump(void)
{
    if (!audio_initialized) return;

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice >= 0) {
            if (!of_mixer_voice_active(active_voices[i].voice)) {
                int snd = active_voices[i].sound_num;
                int owned = active_voices[i].has_owner;
                untrack_voice(i);
                /* Only fire Duke's callback for voices with SoundOwner entries
                 * (played via xyzsound). Fire-and-forget voices from sound()
                 * have no SoundOwner entry — calling testcallback would
                 * underflow Sound[].num. */
                if (owned) {
                    extern void testcallback(int32_t num);
                    testcallback(snd);
                }
            }
        }
    }
}
