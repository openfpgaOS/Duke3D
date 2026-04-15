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
    uint8_t *pcm;           /* CRAM1 pointer to decoded PCM */
    uint32_t sample_count;
    uint32_t sample_rate;
    int      is_16bit;      /* 1 = 16-bit signed, 0 = 8-bit signed */
} decoded_sound_t;

static decoded_sound_t decoded[NUM_SOUNDS];

/* ================================================================
 * Voice tracking for completion callbacks
 * ================================================================ */
#define MAX_ACTIVE_VOICES 32

typedef struct {
    int      voice;       /* OS mixer voice index (0-31), or -1 */
    int      sound_num;   /* Duke sound number */
    int      has_owner;   /* 1 = xyzsound (tracked in SoundOwner), 0 = fire-and-forget */
    uint32_t expire_ms;   /* of_time_ms() when sound finishes, 0 = looping/never */
} active_voice_t;

static active_voice_t active_voices[MAX_ACTIVE_VOICES];

static void init_voice_tracking(void) {
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        active_voices[i].voice = -1;
}

static void track_voice_timed(int voice, int sound_num, int has_owner,
                              uint32_t duration_ms) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].voice = voice;
    active_voices[voice].sound_num = sound_num;
    active_voices[voice].has_owner = has_owner;
    active_voices[voice].expire_ms = duration_ms ? (of_time_ms() + duration_ms) : 0;
}

static void untrack_voice(int voice) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].voice = -1;
}

static void complete_voice(int i) {
    int snd = active_voices[i].sound_num;
    int owned = active_voices[i].has_owner;
    /* Timer-based expiry: the voice has been silent for ~50ms by the
     * time we get here. If the mixer reused the slot for a new sound,
     * track_voice_timed would have overwritten expire_ms with a future
     * time, so the expiry check wouldn't have fired. Safe to stop. */
    of_mixer_stop(i);
    untrack_voice(i);
    if (owned) {
        extern void testcallback(int32_t num);
        testcallback(snd);
    }
}

/* ================================================================
 * 3D audio: angle + distance → stereo L/R volumes
 * ================================================================ */

/* Convert Duke angle (0-2047 BAMS) + distance (0-255) to L/R volumes.
 * angle 0 = ahead, 512 = right, 1024 = behind, 1536 = left.
 * Uses BUILD engine sintable for smooth panning. */
/* Compute volume (0-255) and pan (0=left, 128=center, 255=right)
 * from BUILD angle + distance. */
static void angle_dist_to_vol_pan(int angle, int distance,
                                  int *out_vol, int *out_pan)
{
    int volume = 255 - distance;
    if (volume < 0) volume = 0;

    int s = sintable[angle & 2047];  /* -16383 to +16383 */
    int pan = 128 + ((s * 127) >> 14);

    *out_vol = volume;
    *out_pan = pan;
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

    /* No timer interrupt — pump cooperatively from the game loop like
     * mididemo. A timer ISR racing with the video path (FB_SWAP_CTRL
     * writes in of_video_flip, or the busy-wait in of_video_wait_flip)
     * has been observed to hang; pumping from _nextpage / sampletimer
     * avoids the race entirely. */
}

void d3d_audio_shutdown(void)
{
    if (!audio_initialized) return;
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

    /* Always convert to 16-bit signed for playback.
     * 8-bit hardware path exists but hasn't been validated on device yet.
     * TODO: switch to of_mixer_play_8bit once 8-bit mode is tested. */
    uint32_t sample_count = result.pcm_len;
    uint32_t byte_len = sample_count * sizeof(int16_t);
    int16_t *cram_ptr = (int16_t *)of_mixer_alloc_samples(byte_len);
    if (cram_ptr == NULL)
        return 0;

    if (result.bits_per_sample == 8) {
        const uint8_t *src = result.pcm;
        for (uint32_t i = 0; i < sample_count; i++)
            cram_ptr[i] = (int16_t)((src[i] - 128) << 8);
    } else {
        memcpy(cram_ptr, result.pcm, byte_len);
    }

    decoded[num].pcm = (uint8_t *)cram_ptr;
    decoded[num].sample_count = sample_count;
    decoded[num].sample_rate = result.sample_rate;
    decoded[num].is_16bit = 1;  /* always 16-bit for now */

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

    int vol = volume * 2;
    if (vol > 255) vol = 255;

    int voice = of_mixer_play(decoded[num].pcm,
                              decoded[num].sample_count,
                              decoded[num].sample_rate,
                              priority, vol);

    if (voice >= 0) {
        uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                     / decoded[num].sample_rate + 50;
        track_voice_timed(voice, num, 0, dur);
    }

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

    int vol, pan;
    angle_dist_to_vol_pan(angle, distance, &vol, &pan);

    int scaled_vol = vol * 2;
    if (scaled_vol > 255) scaled_vol = 255;

    int voice = of_mixer_play(decoded[num].pcm,
                              decoded[num].sample_count,
                              decoded[num].sample_rate,
                              priority, scaled_vol);

    if (voice >= 0) {
        of_mixer_set_pan(voice, pan);
        uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                     / decoded[num].sample_rate + 50;
        track_voice_timed(voice, num, 0, dur);
    }

    return voice;
}

/*
 * Update panning for an active 3D sound (called per-frame from pan3dsound).
 */
void d3d_sound_set_pan(int voice, int angle, int distance)
{
    if (!audio_initialized || voice < 0) return;

    int vol, pan;
    angle_dist_to_vol_pan(angle, distance, &vol, &pan);

    /* Only update pan — volume was set at play time and the hardware
     * ramp rate (default 0) blocks post-play volume changes. Pan is
     * a separate hardware path that works regardless of ramp. */
    of_mixer_set_pan(voice, pan);
}

/*
 * Enable looping on a voice (loops entire sample).
 */
void d3d_sound_set_loop(int voice)
{
    if (!audio_initialized || voice < 0) return;
    of_mixer_set_loop(voice, 0, -1);
    /* Cancel timer expiry — looping sounds play until explicitly stopped */
    if (voice < MAX_ACTIVE_VOICES)
        active_voices[voice].expire_ms = 0;
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
 * Pump MIDI + expire finished voices.  Called from sampletimer
 * (via getpackets, frequent) and _nextpage (once per frame).
 *
 * Voice completion is purely timer-based: we know each sound's
 * duration (sample_count / sample_rate) and expire it after that
 * time.  No hardware poll_ended register, no IRQs, no races.
 */
void d3d_audio_pump(void)
{
    if (!audio_initialized) return;

    of_midi_pump();

    /* Drain the hardware ended register so it doesn't overflow,
     * but we don't act on it — timer expiry handles everything. */
    of_mixer_poll_ended();

    uint32_t now = of_time_ms();
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice < 0) continue;
        if (active_voices[i].expire_ms == 0) continue;  /* looping — no expiry */
        if ((int32_t)(now - active_voices[i].expire_ms) >= 0)
            complete_voice(i);
    }
}
