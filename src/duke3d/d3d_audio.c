/*
 * d3d_audio.c — Sound playback for Duke3D on openfpgaOS
 *
 * Routes all game audio through the OS hardware mixer (of_mixer.h).
 * Handles VOC/WAV parsing, CRAM1 upload, voice tracking, completion
 * callbacks, and 3D positional audio (distance → volume, angle → L/R pan).
 *
 * MIDI playback is owned by the kernel's machine-timer ISR (installed
 * by of_midi_play); voice completion polling runs from d3d_audio_pump,
 * called once per frame from _nextpage.
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
#include "pitch.h"

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

/* Pan-debounce cache for d3d_sound_set_pan — invalidated when the slot
 * is freed in untrack_voice() so a new sound's initial pan still writes. */
static uint8_t last_pan[MAX_ACTIVE_VOICES];
static uint8_t last_pan_valid[MAX_ACTIVE_VOICES];

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
    /* Invalidate the pan-debounce cache so the next sound that lands on
     * this slot writes its initial pan unconditionally. */
    last_pan_valid[voice] = 0;
}

static int d3d_voice_is_sfx_or_unknown(int voice)
{
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return 0;
    int group = of_mixer_voice_group(voice);
    return group < 0 || group == OF_MIXER_GROUP_SFX;
}

static void d3d_stop_sfx_voice_if_owned(int voice)
{
    if (d3d_voice_is_sfx_or_unknown(voice))
        of_mixer_stop(voice);
}

static void complete_voice(int i) {
    int snd = active_voices[i].sound_num;
    int owned = active_voices[i].has_owner;
    /* Timer-based expiry: the voice has been silent for ~50ms by the
     * time we get here. If the mixer reused the slot for a new sound,
     * track_voice_timed would have overwritten expire_ms with a future
     * time, so the expiry check wouldn't have fired. Safe to stop. */
    d3d_stop_sfx_voice_if_owned(i);
    untrack_voice(i);
    if (owned) {
        extern void testcallback(int32_t num);
        testcallback(snd);
    }
}

static void complete_ended_voice(int i) {
    int snd = active_voices[i].sound_num;
    int owned = active_voices[i].has_owner;
    untrack_voice(i);
    if (owned) {
        extern void testcallback(int32_t num);
        testcallback(snd);
    }
}

static void poll_ended_voices(void) {
    uint32_t ended = of_mixer_poll_ended();
    if (!ended) return;

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (!(ended & (1u << i))) continue;
        if (active_voices[i].voice < 0) continue;
        complete_ended_voice(i);
    }
}

static uint32_t pitched_rate(uint32_t sample_rate, int pitch)
{
    uint64_t scaled = (uint64_t)sample_rate * (uint64_t)PITCH_GetScale(pitch);
    scaled = (scaled + 0x8000u) >> 16;
    if (scaled == 0) scaled = 1;
    if (scaled > 0xFFFFFFFFu) scaled = 0xFFFFFFFFu;
    return (uint32_t)scaled;
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
 * Public API
 * ================================================================ */

void d3d_audio_init(void)
{
    /* Hardware mixer has 32 voices total (MIXER_MAX_VOICES in the
     * kernel hal). SF2 polyphony uses up to 28 of these, leaving 4
     * for game SFX — tight but matches the mididemo's allocation. */
    of_mixer_init(MAX_ACTIVE_VOICES, OF_MIXER_OUTPUT_RATE);
    /* Mirror the mididemo's mixer-volume setup — the of_mixer_init
     * defaults leave master + group volumes at 0 on this kernel, so
     * without these explicit sets every of_mixer_play would emit
     * silence even though the voice slot allocates fine. */
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX,   255);
    /* DO NOT call of_mixer_free_samples() here.  The kernel allocated
     * the SoundFont at SAMPLE_POOL_BASE during boot; free_samples
     * resets the pool head to that same address, so duke3d's first SFX
     * decode would happily overwrite the bank's sample blob and the
     * mixer DMA would feed garbage to the AWE — totally distorted MIDI.
     * The mididemo never calls free_samples for the same reason. */
    init_voice_tracking();
    memset(decoded, 0, sizeof(decoded));
    audio_initialized = 1;
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
    return d3d_sound_play_pitch(num, priority, volume, 0);
}

int d3d_sound_play_pitch(int num, int priority, int volume, int pitch)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    poll_ended_voices();

    if (!ensure_decoded(num))
        return -1;

    int vol = volume * 2;
    if (vol > 255) vol = 255;

    uint32_t rate = pitched_rate(decoded[num].sample_rate, pitch);
    int voice = of_mixer_alloc_for_group(OF_MIXER_GROUP_SFX,
                                         decoded[num].pcm,
                                         decoded[num].sample_count,
                                         rate,
                                         priority, vol);

    if (voice >= 0) {
        uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                     / rate + 50;
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
    return d3d_sound_play_3d_pitch(num, priority, angle, distance, 0);
}

int d3d_sound_play_3d_pitch(int num, int priority, int angle, int distance, int pitch)
{
    if (!audio_initialized) return -1;
    if (num < 0 || num >= NUM_SOUNDS) return -1;

    poll_ended_voices();

    if (!ensure_decoded(num))
        return -1;

    int vol, pan;
    angle_dist_to_vol_pan(angle, distance, &vol, &pan);

    int scaled_vol = vol * 2;
    if (scaled_vol > 255) scaled_vol = 255;

    uint32_t rate = pitched_rate(decoded[num].sample_rate, pitch);
    int voice = of_mixer_alloc_for_group(OF_MIXER_GROUP_SFX,
                                         decoded[num].pcm,
                                         decoded[num].sample_count,
                                         rate,
                                         priority, scaled_vol);

    if (voice >= 0) {
        of_mixer_set_pan(voice, pan);
        uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                     / rate + 50;
        track_voice_timed(voice, num, 0, dur);
    }

    return voice;
}

/*
 * Update panning for an active 3D sound (called per-frame from pan3dsound).
 *
 * Per-frame pan updates were observed to cause crackle in busy passages
 * (the diagnostic stub of this function eliminated it).  Mechanism is
 * still under investigation — likely a HW response to high-rate
 * MIX_VOICE_VOL_TARGET writes (write-vs-FSM-pipeline race or AXI write
 * backpressure starving the mixer's per-sample SDRAM budget).
 *
 * Workaround: cache the last pan we sent per voice slot and only call
 * the syscall when the quantized pan actually changes.  The 0..255 pan
 * is computed from a sintable + 14-bit shift so adjacent angles
 * frequently round to the same byte — most per-frame calls today are
 * issuing the same value and the syscall does nothing useful anyway.
 * Real pan changes still propagate immediately.
 */
void d3d_sound_set_pan(int voice, int angle, int distance)
{
    if (!audio_initialized || voice < 0) return;
    if (voice >= MAX_ACTIVE_VOICES) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;

    int vol, pan;
    angle_dist_to_vol_pan(angle, distance, &vol, &pan);

    if (last_pan_valid[voice] && (uint8_t)pan == last_pan[voice])
        return;

    last_pan[voice]       = (uint8_t)pan;
    last_pan_valid[voice] = 1;

    of_mixer_set_pan(voice, pan);
}

/*
 * Enable looping on a voice (loops entire sample).
 */
void d3d_sound_set_loop(int voice)
{
    if (!audio_initialized || voice < 0) return;
    if (voice >= MAX_ACTIVE_VOICES) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;

    of_mixer_set_loop(voice, 0, -1);
    /* Cancel timer expiry — looping sounds play until explicitly stopped */
    active_voices[voice].expire_ms = 0;
}

void d3d_sound_stop(int voice)
{
    if (!audio_initialized) return;
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    d3d_stop_sfx_voice_if_owned(voice);
    untrack_voice(voice);
}

void d3d_sound_stop_all(void)
{
    if (!audio_initialized) return;

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice < 0) continue;
        d3d_stop_sfx_voice_if_owned(i);
        untrack_voice(i);
    }
}

void d3d_sound_set_volume(int voice, int volume)
{
    if (!audio_initialized) return;
    if (voice < 0) return;
    if (voice >= MAX_ACTIVE_VOICES) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;

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

    /* of_midi_pump is owned by the machine-timer ISR that of_midi_play
     * installs — calling it from here too races on the M state and
     * has been observed to fault.  See of_midi.h for the contract. */

    /* Pump the SW mixer from the main thread once per frame.  The
     * MIDI envelope/LFO ISR stays in the kernel and is cheap; the
     * heavy sample-mixing work runs here so the renderer's I-cache
     * isn't trashed on every ISR fire.  of_mixer_pump loops
     * swmixer_tick internally with a sane cap, so a late call just
     * catches the audio ring back up. */
    of_mixer_pump();

    poll_ended_voices();

    uint32_t now = of_time_ms();
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice < 0) continue;
        if (active_voices[i].expire_ms == 0) continue;  /* looping — no expiry */
        if ((int32_t)(now - active_voices[i].expire_ms) >= 0)
            complete_voice(i);
    }
}
