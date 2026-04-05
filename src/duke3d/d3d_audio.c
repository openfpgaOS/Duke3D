/*
 * d3d_audio.c — Sound playback for Duke3D on openfpgaOS
 *
 * Routes all game audio through the OS hardware mixer (of_mixer.h).
 * Handles VOC/WAV parsing, CRAM1 upload, voice tracking, completion
 * callbacks, and 3D positional audio (distance → volume, angle → L/R pan).
 *
 * A 60Hz timer interrupt drives MIDI playback and voice completion
 * polling, ensuring consistent timing independent of frame rate.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "of.h"
#include "of_file.h"
#include "of_mixer.h"
#include "of_codec.h"
#include "of_timer.h"
#include "of_midi.h"

/* Game headers */
#include "duke3d.h"
#include "filesystem.h"

/* BUILD engine sine table: short[2048], range -16383 to +16383
 * sintable[ang & 2047]       = sin(ang * pi/1024) * 16383
 * sintable[(ang+512) & 2047] = cos(ang * pi/1024) * 16383 */
extern short sintable[];

static int audio_initialized = 0;

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
 * 3D audio: angle + distance → stereo L/R volumes
 * ================================================================ */

/* Convert Duke angle (0-2047 BAMS) + distance (0-255) to L/R volumes.
 * angle 0 = ahead, 512 = right, 1024 = behind, 1536 = left.
 * Uses BUILD engine sintable for smooth panning. */
static void angle_dist_to_lr(int angle, int distance,
                              int *out_vol_l, int *out_vol_r)
{
    int volume = 255 - distance;
    if (volume < 0) volume = 0;

    /* sintable[(angle+512)&2047] = cos(angle) → front/back
     * sintable[angle&2047]       = sin(angle) → left/right
     * sin > 0 when angle 0-1023 (right side)
     * sin < 0 when angle 1024-2047 (left side) */
    int s = sintable[angle & 2047];  /* -16383 to +16383 */

    /* Map sine to pan: 0 (full left) to 255 (full right), 128 = center */
    int pan = 128 + ((s * 127) >> 14);

    *out_vol_l = (volume * (255 - pan)) >> 8;
    *out_vol_r = (volume * pan) >> 8;
}

/* ================================================================
 * Timer-driven audio pump (60 Hz)
 * ================================================================ */

#define AUDIO_TICK_HZ 60

static volatile int pump_pending = 0;

static void audio_timer_tick(void)
{
    pump_pending = 1;
}

/* ================================================================
 * Public API
 * ================================================================ */

void d3d_audio_init(void)
{
    of_mixer_init(MAX_ACTIVE_VOICES, OF_MIXER_OUTPUT_RATE);
    of_mixer_free_samples();
    init_voice_tracking();
    memset(decoded, 0, sizeof(decoded));
    audio_initialized = 1;

    /* Start timer for MIDI + voice completion polling */
    of_timer_set_callback(audio_timer_tick, AUDIO_TICK_HZ);
}

void d3d_audio_shutdown(void)
{
    if (!audio_initialized) return;
    of_timer_stop();
    of_mixer_stop_all();
    audio_initialized = 0;
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

    /* Allocate sample memory via kernel and copy decoded PCM */
    uint32_t byte_len = result.pcm_len * sizeof(int16_t);
    int16_t *cram_ptr = (int16_t *)of_mixer_alloc_samples(byte_len);
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
                              priority, volume / 2);

    if (voice >= 0)
        track_voice(voice, num, 0);

    return voice;
}

/*
 * Play with 3D positioning. Angle (0-2047 BAMS) + distance (0-255)
 * mapped to stereo L/R volumes via sine-based panning.
 */
int d3d_sound_play_3d(int num, int priority, int angle, int distance)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    if (!ensure_decoded(num))
        return -1;

    int vol_l, vol_r;
    angle_dist_to_lr(angle, distance, &vol_l, &vol_r);

    /* Start voice at center volume, then set L/R immediately */
    int avg = (vol_l + vol_r) >> 1;
    int voice = of_mixer_play((const uint8_t *)decoded[num].pcm,
                              decoded[num].sample_count,
                              decoded[num].sample_rate,
                              priority, avg);

    if (voice >= 0) {
        of_mixer_set_vol_lr(voice, vol_l, vol_r);
        track_voice(voice, num, 0);
    }

    return voice;
}

/*
 * Update panning for an active 3D sound (called per-frame from pan3dsound).
 */
void d3d_sound_set_pan(int voice, int angle, int distance)
{
    if (!audio_initialized || voice < 0) return;

    int vol_l, vol_r;
    angle_dist_to_lr(angle, distance, &vol_l, &vol_r);
    of_mixer_set_vol_lr(voice, vol_l, vol_r);
}

/*
 * Enable looping on a voice (loops entire sample).
 */
void d3d_sound_set_loop(int voice)
{
    if (!audio_initialized || voice < 0) return;
    of_mixer_set_loop(voice, 0, -1);
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
 * Called from timer interrupt and also from display loop as fallback.
 */
void d3d_audio_pump(void)
{
    if (!audio_initialized) return;

    /* Process timer-driven work */
    if (pump_pending) {
        pump_pending = 0;
        of_midi_pump();
    }

    /* Single register read: bitmask of voices that finished since last poll */
    uint32_t ended = of_mixer_poll_ended();
    while (ended) {
        int i = __builtin_ctz(ended);
        ended &= ended - 1;

        if (active_voices[i].voice >= 0) {
            int snd = active_voices[i].sound_num;
            int owned = active_voices[i].has_owner;
            untrack_voice(i);
            if (owned) {
                extern void testcallback(int32_t num);
                testcallback(snd);
            }
        }
    }
}
