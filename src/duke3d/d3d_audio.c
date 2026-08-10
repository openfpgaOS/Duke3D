/*
 * d3d_audio.c — Sound playback for Duke3D on openfpgaOS
 *
 * Routes all game audio through the OS hardware mixer (of_mixer.h).
 * Handles VOC/WAV parsing, CRAM1 upload, voice tracking, completion
 * callbacks, and 3D positional audio.
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
#include "of_smp_voice.h"

/* Game headers */
#include "duke3d.h"
#include "filesystem.h"
#include "pitch.h"

static int audio_initialized = 0;

void d3d_audio_pump_loading(void);

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
static int all_sounds_precached = 0;

#define D3D_SOUND_BULK_CHUNK (512u * 1024u)

typedef struct {
    int     num;
    int32_t grpID;
    int32_t offset;
    int32_t size;
} d3d_sound_preload_item_t;

static d3d_sound_preload_item_t sound_preload_items[NUM_SOUNDS];

static void release_raw_sound(int num)
{
    if (num < 0 || num >= NUM_SOUNDS)
        return;
    free(Sound[num].ptr);
    Sound[num].ptr = NULL;
    Sound[num].length = 0;
    if (Sound[num].lock < 200)
        Sound[num].lock = 199;
}

/* ================================================================
 * Voice tracking for completion callbacks
 * ================================================================ */
#define MAX_ACTIVE_VOICES 32

typedef struct {
    int      voice;       /* OS mixer voice index (0-31), or -1 */
    of_mixer_handle_t mixer_handle; /* stable OS mixer logical voice */
    int      handle;      /* Duke/MultiVoc-style generation handle */
    int      sound_num;   /* Duke sound number */
    int      has_owner;   /* 1 = xyzsound (tracked in SoundOwner), 0 = fire-and-forget */
    int      priority;    /* Duke/MultiVoc priority used for eviction */
    uint32_t expire_ms;   /* of_time_ms() when sound finishes, 0 = looping/never */
} active_voice_t;

static active_voice_t active_voices[MAX_ACTIVE_VOICES];
static int next_voice_handle = 1;
static int d3d_sfx_user_volume = 255;

/* L/R target debounce cache for d3d_sound_set_pan.  Invalidated when a
 * slot is freed so a new sound's initial stereo volume always writes. */
/* Orphaned-music-voice reclaim (see d3d_audio_pump).  250 ms is well inside
 * the window where a leaked looping voice would be noticed, and slow enough
 * that the scan never shows up next to the per-frame work. */
#define D3D_ORPHAN_REAP_INTERVAL_MS 250
static uint32_t next_orphan_reap_ms;
static uint32_t orphan_reap_total;

static uint8_t last_vol_l[MAX_ACTIVE_VOICES];
static uint8_t last_vol_r[MAX_ACTIVE_VOICES];
static uint8_t last_vol_valid[MAX_ACTIVE_VOICES];

static void init_voice_tracking(void) {
    next_voice_handle = 1;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        active_voices[i].voice = -1;
        active_voices[i].mixer_handle = OF_MIXER_HANDLE_INVALID;
        active_voices[i].handle = 0;
    }
}

static void untrack_voice(int voice) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    active_voices[voice].voice = -1;
    active_voices[voice].mixer_handle = OF_MIXER_HANDLE_INVALID;
    active_voices[voice].handle = 0;
    last_vol_valid[voice] = 0;
}

static int handle_in_use(int handle)
{
    if (handle <= 0) return 0;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        if (active_voices[i].voice >= 0 && active_voices[i].handle == handle)
            return 1;
    return 0;
}

static int alloc_voice_handle(void)
{
    int handle;
    do {
        handle = next_voice_handle++;
        if (next_voice_handle <= 0)
            next_voice_handle = 1;
    } while (handle <= 0 || handle_in_use(handle));
    return handle;
}

static int voice_from_handle(int handle)
{
    if (handle <= 0) return -1;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        if (active_voices[i].voice == i && active_voices[i].handle == handle)
            return i;
    return -1;
}

static void retire_voice_owner_for_reuse(int voice)
{
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return;
    if (active_voices[voice].voice < 0) return;

    int snd = active_voices[voice].sound_num;
    int owned = active_voices[voice].has_owner;
    untrack_voice(voice);

    if (owned) {
        extern void testcallback(int32_t num);
        testcallback(snd);
    }
}

static int track_voice_timed(int voice, of_mixer_handle_t mixer_handle,
                             int sound_num, int has_owner,
                             int priority, uint32_t duration_ms) {
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return -1;
    retire_voice_owner_for_reuse(voice);
    active_voices[voice].voice = voice;
    active_voices[voice].mixer_handle = mixer_handle;
    active_voices[voice].handle = alloc_voice_handle();
    active_voices[voice].sound_num = sound_num;
    active_voices[voice].has_owner = has_owner;
    active_voices[voice].priority = priority;
    active_voices[voice].expire_ms = duration_ms ? (of_time_ms() + duration_ms) : 0;
    return active_voices[voice].handle;
}

static int d3d_voice_is_sfx_or_unknown(int voice)
{
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return 0;
    of_mixer_handle_t mh = active_voices[voice].mixer_handle;
    if (mh == OF_MIXER_HANDLE_INVALID || !of_mixer_handle_active(mh))
        return 0;
    int group = of_mixer_handle_group(mh);
    return group < 0 || group == OF_MIXER_GROUP_SFX;
}

static void d3d_stop_sfx_voice_if_owned(int voice)
{
    if (d3d_voice_is_sfx_or_unknown(voice))
        of_mixer_stop_h(active_voices[voice].mixer_handle);
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
    of_mixer_handle_t ended[8];
    uint32_t count;

    while ((count = of_mixer_poll_ended_h(ended, 8)) != 0) {
        for (uint32_t e = 0; e < count; e++) {
            for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
                if (active_voices[i].voice < 0) continue;
                if (active_voices[i].mixer_handle != ended[e]) continue;
                complete_ended_voice(i);
                break;
            }
        }
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

static uint16_t d3d_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t d3d_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int d3d_parse_voc_local(const uint8_t *data, uint32_t size,
                               of_codec_result_t *out)
{
    if (data == NULL || out == NULL || size < 0x1A)
        return -1;
    if (data[0] != 'C' || data[1] != 'r')
        return -1;

    memset(out, 0, sizeof(*out));
    out->sample_rate = 11025;
    out->bits_per_sample = 8;
    out->channels = 1;

    uint32_t data_offset = d3d_rd16(data + 0x14);
    if (data_offset >= size)
        return -1;

    const uint8_t *p = data + data_offset;
    const uint8_t *end = data + size;

    while (p + 4 <= end) {
        uint8_t block_type = p[0];
        if (block_type == 0)
            break;

        uint32_t block_len = (uint32_t)p[1] | ((uint32_t)p[2] << 8)
                           | ((uint32_t)p[3] << 16);
        p += 4;

        if ((uint32_t)(end - p) < block_len)
            break;

        if (block_type == 1 && block_len >= 2) {
            uint8_t time_constant = p[0];
            out->sample_rate = 1000000u / (256u - (uint32_t)time_constant);
            out->bits_per_sample = 8;
            out->channels = 1;
            out->pcm_len = block_len - 2;
            out->pcm = p + 2;
            return 0;
        }

        if (block_type == 9 && block_len >= 12) {
            out->sample_rate = d3d_rd32(p);
            out->bits_per_sample = p[4];
            out->channels = p[5];
            out->pcm_len = block_len - 12;
            out->pcm = p + 12;
            return 0;
        }

        p += block_len;
    }

    return -1;
}

static int d3d_parse_wav_local(const uint8_t *data, uint32_t size,
                               of_codec_result_t *out)
{
    if (data == NULL || out == NULL || size < 44)
        return -1;
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F')
        return -1;
    if (data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E')
        return -1;

    memset(out, 0, sizeof(*out));

    const uint8_t *p = data + 12;
    const uint8_t *end = data + size;
    int found_fmt = 0;

    while (p + 8 <= end) {
        uint32_t chunk_id = d3d_rd32(p);
        uint32_t chunk_size = d3d_rd32(p + 4);
        const uint8_t *chunk_data = p + 8;

        if ((uint32_t)(end - chunk_data) < chunk_size)
            break;

        if (chunk_id == 0x20746D66u && chunk_size >= 16) { /* "fmt " */
            uint16_t audio_format = d3d_rd16(chunk_data);
            if (audio_format != 1)
                return -1;
            out->channels = (uint8_t)d3d_rd16(chunk_data + 2);
            out->sample_rate = d3d_rd32(chunk_data + 4);
            out->bits_per_sample = (uint8_t)d3d_rd16(chunk_data + 14);
            found_fmt = 1;
        } else if (chunk_id == 0x61746164u) { /* "data" */
            if (!found_fmt)
                return -1;
            out->pcm = chunk_data;
            out->pcm_len = chunk_size;
            return 0;
        }

        p = chunk_data + chunk_size;
        if (chunk_size & 1)
            p++;
    }

    return -1;
}

static int d3d_parse_sound_local(const uint8_t *data, uint32_t size,
                                 of_codec_result_t *out)
{
    if (data == NULL || size == 0)
        return -1;
    if (data[0] == 'C')
        return d3d_parse_voc_local(data, size, out);
    return d3d_parse_wav_local(data, size, out);
}

static int load_raw_sound_from_file(int num)
{
    if (Sound[num].ptr != NULL)
        return 1;
    if (sounds[num][0] == '\0')
        return 0;

    short fp = TCkopen4load(sounds[num], 0);
    if (fp == -1)
        return 0;

    int32_t l = kfilelength(fp);
    if (l <= 0) {
        kclose(fp);
        return 0;
    }

    Sound[num].lock = 199;
    Sound[num].length = l;
    soundsiz[num] = l;
    Sound[num].ptr = (uint8_t *)malloc(l);
    if (Sound[num].ptr == NULL) {
        kclose(fp);
        return 0;
    }

    int32_t got = kread(fp, Sound[num].ptr, l);
    kclose(fp);

    if (got != l) {
        free(Sound[num].ptr);
        Sound[num].ptr = NULL;
        soundsiz[num] = 0;
        Sound[num].length = 0;
        return 0;
    }

    return 1;
}

static int decode_raw_sound(int num, const uint8_t *raw, uint32_t raw_size)
{
    if (decoded[num].pcm != NULL)
        return 1;
    if (raw == NULL || raw_size == 0)
        return 0;

    of_codec_result_t result;
    int rc = d3d_parse_sound_local(raw, raw_size, &result);
    if (rc < 0) {
        if (*raw == 'C')
            rc = of_codec_parse_voc(raw, raw_size, &result);
        else
            rc = of_codec_parse_wav(raw, raw_size, &result);
    }

    if (rc < 0 || result.pcm == NULL || result.pcm_len == 0)
        return 0;

    /* Always convert to 16-bit signed for playback.
     * The OS 8-bit helper expands to 16-bit at play time, so doing this
     * once during preload avoids first-use stalls in gameplay. */
    uint32_t sample_count = result.pcm_len;
    if (result.bits_per_sample == 16)
        sample_count /= sizeof(int16_t);
    else if (result.bits_per_sample != 8)
        return 0;
    if (sample_count == 0)
        return 0;

    uint32_t byte_len = sample_count * sizeof(int16_t);
    int16_t *cram_ptr = (int16_t *)of_mixer_alloc_samples(byte_len);
    if (cram_ptr == NULL)
        return 0;

    if (result.bits_per_sample == 8) {
        const uint8_t *src = result.pcm;
        for (uint32_t i = 0; i < sample_count; i++) {
            cram_ptr[i] = (int16_t)((src[i] - 128) << 8);
            if ((i & 4095u) == 4095u)
                d3d_audio_pump_loading();
        }
    } else {
        memcpy(cram_ptr, result.pcm, byte_len);
    }

    decoded[num].pcm = (uint8_t *)cram_ptr;
    decoded[num].sample_count = sample_count;
    decoded[num].sample_rate = result.sample_rate;
    decoded[num].is_16bit = 1;
    soundsiz[num] = raw_size;
    Sound[num].lock = 199;

    return 1;
}

static int decode_loaded_sound(int num)
{
    if (decoded[num].pcm != NULL)
        return 1;
    if (Sound[num].ptr == NULL || soundsiz[num] <= 0)
        return 0;

    int ok = decode_raw_sound(num, Sound[num].ptr, (uint32_t)soundsiz[num]);
    if (ok)
        release_raw_sound(num);
    return ok;
}

static int d3d_sound_preload_cmp(const void *a, const void *b)
{
    const d3d_sound_preload_item_t *ia = (const d3d_sound_preload_item_t *)a;
    const d3d_sound_preload_item_t *ib = (const d3d_sound_preload_item_t *)b;

    if (ia->grpID != ib->grpID)
        return (ia->grpID < ib->grpID) ? -1 : 1;
    if (ia->offset != ib->offset)
        return (ia->offset < ib->offset) ? -1 : 1;
    return ia->num - ib->num;
}

/* ================================================================
 * 3D audio: match Duke's MultiVoc pan table
 * ================================================================ */

#define D3D_MV_MAX_VOLUME        63
#define D3D_MV_NUM_PAN_POSITIONS 32
#define D3D_AUDIO_SFX_RAMP_RATE  255
#define D3D_AUDIO_STEREO_3D_PAN      1
/* Keep SFX below full-scale so overlapping effects and music do not
 * immediately saturate the hardware mixer.  Duke's default FXVolume is 220,
 * which maps to about 193/255 with this cap. */
#define D3D_AUDIO_SFX_GROUP_VOLUME_MAX 224

static int d3d_mv_mix_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    return (volume * (D3D_MV_MAX_VOLUME + 1)) >> 8;
}

static void angle_dist_to_vol_lr(int angle, int distance,
                                 int *out_l, int *out_r)
{
    if (distance < 0) {
        distance = -distance;
        angle += 1024;
    }

    int volume = d3d_mv_mix_volume(distance);
    int level = (255 * (D3D_MV_MAX_VOLUME - volume)) / D3D_MV_MAX_VOLUME;

#if !D3D_AUDIO_STEREO_3D_PAN
    (void)angle;
    *out_l = level;
    *out_r = level;
    return;
#else
    int pos = (angle >> 6) & (D3D_MV_NUM_PAN_POSITIONS - 1);
    int left = level;
    int right = level;

    if (pos > 0 && pos < 16) {
        int a = pos;
        if (a > 16 - pos)
            a = 16 - pos;
        left = level - ((level * a) / (D3D_MV_NUM_PAN_POSITIONS / 4));
    } else if (pos > 16) {
        int a = pos - 16;
        if (a > 32 - pos)
            a = 32 - pos;
        right = level - ((level * a) / (D3D_MV_NUM_PAN_POSITIONS / 4));
    }

    *out_l = left;
    *out_r = right;
#endif
}

static int d3d_max2(int a, int b)
{
    return a > b ? a : b;
}

static int d3d_clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

static int d3d_scaled_sfx_group_volume(void)
{
    return (d3d_sfx_user_volume * D3D_AUDIO_SFX_GROUP_VOLUME_MAX + 127) / 255;
}

static void d3d_prepare_sfx_voice(of_mixer_handle_t voice)
{
    of_mixer_set_volume_ramp_h(voice, D3D_AUDIO_SFX_RAMP_RATE);
}

static int d3d_voice_can_steal_for_sfx(int voice, int priority, int allow_looped)
{
    if (voice < 0 || voice >= MAX_ACTIVE_VOICES) return 0;
    if (active_voices[voice].voice < 0) return 0;
    if (active_voices[voice].priority > priority) return 0;
    if (!allow_looped && active_voices[voice].expire_ms == 0) return 0;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return 0;
    return 1;
}

static int d3d_sfx_steal_score(int voice)
{
    int score = active_voices[voice].priority << 16;
    if (active_voices[voice].expire_ms == 0)
        score += 0x7FFF;
    else {
        uint32_t now = of_time_ms();
        int32_t remain = (int32_t)(active_voices[voice].expire_ms - now);
        if (remain < 0) remain = 0;
        if (remain > 0x7FFF) remain = 0x7FFF;
        score += remain;
    }
    return score;
}

static int d3d_steal_sfx_voice_for_priority(int priority)
{
    for (int allow_looped = 0; allow_looped <= 1; allow_looped++) {
        int best = -1;
        int best_score = 0x7FFFFFFF;

        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            if (!d3d_voice_can_steal_for_sfx(i, priority, allow_looped))
                continue;

            int score = d3d_sfx_steal_score(i);
            if (score < best_score) {
                best = i;
                best_score = score;
            }
        }

        if (best >= 0) {
            complete_voice(best);
            return 1;
        }
    }

    return 0;
}

static of_mixer_handle_t d3d_alloc_sfx_voice(const uint8_t *pcm,
                                             uint32_t sample_count,
                                             uint32_t rate,
                                             int priority,
                                             int volume)
{
    of_mixer_handle_t voice = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_SFX, pcm,
                                                         sample_count, rate,
                                                         priority, volume);
    if (voice != OF_MIXER_HANDLE_INVALID)
        return voice;

    /* MultiVoc allowed an equal-priority sound to evict an existing voice.
     * The OS allocator only steals strictly lower priorities, so do the
     * Duke-owned eviction here and retry once. */
    if (!d3d_steal_sfx_voice_for_priority(priority))
        return OF_MIXER_HANDLE_INVALID;

    return of_mixer_alloc_for_group_h(OF_MIXER_GROUP_SFX, pcm,
                                      sample_count, rate,
                                      priority, volume);
}

/* ================================================================
 * Public API
 * ================================================================ */

void d3d_audio_init(void)
{
    /* Hardware mixer has 32 voices total (MIXER_MAX_VOICES in the
     * kernel HAL).  Duke's MIDI soft polyphony is set by SMP_MAX_VOICES;
     * the group-aware allocator keeps MUSIC and SFX at opposite ends of
     * the hardware slot range under normal load. */
    of_mixer_init(MAX_ACTIVE_VOICES, OF_MIXER_OUTPUT_RATE);
    /* Mirror the mididemo's mixer-volume setup — the of_mixer_init
     * defaults leave master + group volumes at 0 on this kernel, so
     * without these explicit sets every of_mixer_play would emit
     * silence even though the voice slot allocates fine. */
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, d3d_scaled_sfx_group_volume());
    /* DO NOT call of_mixer_free_samples() here.  The kernel allocated
     * the SoundFont at SAMPLE_POOL_BASE during boot; free_samples
     * resets the pool head to that same address, so duke3d's first SFX
     * decode would happily overwrite the bank's sample blob and the
     * mixer DMA would feed garbage to the AWE — totally distorted MIDI.
     * The mididemo never calls free_samples for the same reason. */
    init_voice_tracking();
    memset(decoded, 0, sizeof(decoded));
    all_sounds_precached = 0;
    audio_initialized = 1;
}

void d3d_sound_set_fx_volume(int volume)
{
    d3d_sfx_user_volume = d3d_clamp_u8(volume);
    if (audio_initialized)
        of_mixer_set_group_volume(OF_MIXER_GROUP_SFX,
                                  d3d_scaled_sfx_group_volume());
}

void d3d_audio_shutdown(void)
{
    if (!audio_initialized) return;
    of_mixer_stop_all();
    audio_initialized = 0;
}

/* Decode a sound from GRP and upload to CRAM1 (cached). */
static int ensure_decoded(int num, int allow_io)
{
    if (decoded[num].pcm != NULL)
        return 1;  /* already cached */

    if (!allow_io)
        return 0;

    if (!load_raw_sound_from_file(num))
        return 0;

    return decode_loaded_sound(num);
}

int d3d_sound_precache(int sound_num)
{
    if (!audio_initialized) return 0;
    if (sound_num < 0 || sound_num >= NUM_SOUNDS) return 0;
    return ensure_decoded(sound_num, 1);
}

int d3d_sound_precache_all(void)
{
    if (!audio_initialized)
        return 0;

    all_sounds_precached = 0;

    int item_count = 0;
    int decoded_count = 0;

    for (int i = 0; i < NUM_SOUNDS; i++) {
        int32_t grpID, fileIndex, offset, size;

        if ((i & 7) == 0)
            d3d_audio_pump_loading();

        if (decoded[i].pcm != NULL)
            continue;
        if (sounds[i][0] == '\0')
            continue;
        if (Sound[i].ptr != NULL) {
            decoded_count += decode_loaded_sound(i) ? 1 : 0;
            continue;
        }

        if (kgrp_find_file(sounds[i], &grpID, &fileIndex, &offset, &size) == 0 &&
            size > 0) {
            (void)fileIndex;
            sound_preload_items[item_count].num = i;
            sound_preload_items[item_count].grpID = grpID;
            sound_preload_items[item_count].offset = offset;
            sound_preload_items[item_count].size = size;
            item_count++;
        } else {
            decoded_count += ensure_decoded(i, 1) ? 1 : 0;
        }
    }

    if (item_count == 0) {
        all_sounds_precached = 1;
        return decoded_count;
    }

    qsort(sound_preload_items, item_count, sizeof(sound_preload_items[0]),
          d3d_sound_preload_cmp);

    uint8_t *chunk = (uint8_t *)malloc(D3D_SOUND_BULK_CHUNK);
    if (chunk == NULL) {
        for (int i = 0; i < item_count; i++)
            decoded_count += ensure_decoded(sound_preload_items[i].num, 1) ? 1 : 0;
        d3d_audio_pump_loading();
        all_sounds_precached = 1;
        return decoded_count;
    }

    int32_t window_grp = -1;
    int32_t window_start = 0;
    int32_t window_len = 0;

    for (int i = 0; i < item_count; i++) {
        const d3d_sound_preload_item_t *item = &sound_preload_items[i];
        int num = item->num;

        if ((i & 3) == 0)
            d3d_audio_pump_loading();

        if (decoded[num].pcm != NULL) {
            decoded_count++;
            continue;
        }
        if (Sound[num].ptr != NULL) {
            decoded_count += decode_loaded_sound(num) ? 1 : 0;
            continue;
        }

        int decoded_ok = 0;
        if ((uint32_t)item->size <= D3D_SOUND_BULK_CHUNK) {
            int32_t item_end = item->offset + item->size;
            int window_has_item =
                window_grp == item->grpID &&
                item->offset >= window_start &&
                item_end <= window_start + window_len;

            if (!window_has_item) {
                window_grp = item->grpID;
                window_start = item->offset;
                d3d_audio_pump_loading();
                window_len = kgrp_read_at(window_grp, window_start,
                                          chunk, D3D_SOUND_BULK_CHUNK);
                d3d_audio_pump_loading();
                if (window_len < 0)
                    window_len = 0;
                window_has_item = item_end <= window_start + window_len;
            }

            if (window_has_item)
                decoded_ok = decode_raw_sound(num,
                                              chunk + (item->offset - window_start),
                                              (uint32_t)item->size);
        }

        if (!decoded_ok)
            decoded_ok = ensure_decoded(num, 1);

        decoded_count += decoded_ok ? 1 : 0;
        d3d_audio_pump_loading();
    }

    free(chunk);
    all_sounds_precached = 1;
    return decoded_count;
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

    if (!ensure_decoded(num, !all_sounds_precached))
        return -1;

    int vol = d3d_clamp_u8(volume);

    uint32_t rate = pitched_rate(decoded[num].sample_rate, pitch);
    of_mixer_handle_t mixer_handle = d3d_alloc_sfx_voice(decoded[num].pcm,
                                                         decoded[num].sample_count,
                                                         rate, priority, vol);
    int voice = of_mixer_handle_voice(mixer_handle);

    if (mixer_handle == OF_MIXER_HANDLE_INVALID || voice < 0)
        return -1;

    d3d_prepare_sfx_voice(mixer_handle);
    uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                 / rate + 50;
    return track_voice_timed(voice, mixer_handle, num, 0, priority, dur);
}

/*
 * Play with 3D positioning.  Angle is Duke BAMS 0..2047 and distance
 * is the same 0..255 value passed to MultiVoc's 3D API.
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

    if (!ensure_decoded(num, !all_sounds_precached))
        return -1;

    int vol_l, vol_r;
    angle_dist_to_vol_lr(angle, distance, &vol_l, &vol_r);

    uint32_t rate = pitched_rate(decoded[num].sample_rate, pitch);
    of_mixer_handle_t mixer_handle = d3d_alloc_sfx_voice(decoded[num].pcm,
                                                         decoded[num].sample_count,
                                                         rate, priority,
                                                         d3d_max2(vol_l, vol_r));
    int voice = of_mixer_handle_voice(mixer_handle);

    if (mixer_handle == OF_MIXER_HANDLE_INVALID || voice < 0)
        return -1;

    d3d_prepare_sfx_voice(mixer_handle);
    uint32_t dur = (uint32_t)decoded[num].sample_count * 1000
                 / rate + 50;
    int handle = track_voice_timed(voice, mixer_handle, num, 0, priority, dur);
    if (handle < 0)
        return -1;
    of_mixer_set_vol_lr_h(mixer_handle, vol_l, vol_r);
    last_vol_l[voice] = (uint8_t)vol_l;
    last_vol_r[voice] = (uint8_t)vol_r;
    last_vol_valid[voice] = 1;
    return handle;
}

/*
 * Update panning for an active 3D sound (called per-frame from pan3dsound).
 *
 * Per-frame volume updates were observed to cause crackle in busy passages
 * (the diagnostic stub of this function eliminated it).  Mechanism is
 * still under investigation — likely a HW response to high-rate
 * MIX_VOICE_VOL_TARGET writes (write-vs-FSM-pipeline race or AXI write
 * backpressure starving the mixer's per-sample SDRAM budget).
 *
 * Workaround: cache the last L/R targets per voice slot and only call
 * the syscall when the quantized MultiVoc table result changes.
 */
void d3d_sound_set_pan(int handle, int angle, int distance)
{
    if (!audio_initialized) return;
    int voice = voice_from_handle(handle);
    if (voice < 0) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;
    of_mixer_handle_t mixer_handle = active_voices[voice].mixer_handle;

    int vol_l, vol_r;
    angle_dist_to_vol_lr(angle, distance, &vol_l, &vol_r);

    if (last_vol_valid[voice] &&
        (uint8_t)vol_l == last_vol_l[voice] &&
        (uint8_t)vol_r == last_vol_r[voice])
        return;

    last_vol_l[voice] = (uint8_t)vol_l;
    last_vol_r[voice] = (uint8_t)vol_r;
    last_vol_valid[voice] = 1;

    of_mixer_set_vol_lr_h(mixer_handle, vol_l, vol_r);
}

/*
 * Enable looping on a voice (loops entire sample).
 */
void d3d_sound_set_loop(int handle)
{
    if (!audio_initialized) return;
    int voice = voice_from_handle(handle);
    if (voice < 0) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;

    of_mixer_set_loop_h(active_voices[voice].mixer_handle, 0, -1);
    /* Cancel timer expiry — looping sounds play until explicitly stopped */
    active_voices[voice].expire_ms = 0;
}

void d3d_sound_stop(int handle)
{
    if (!audio_initialized) return;
    int voice = voice_from_handle(handle);
    if (voice < 0) return;
    d3d_stop_sfx_voice_if_owned(voice);
    untrack_voice(voice);
}

void d3d_sound_stop_all(void)
{
    if (!audio_initialized) return;

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice < 0) continue;
        complete_voice(i);
    }
}

void d3d_sound_set_volume(int handle, int volume)
{
    if (!audio_initialized) return;
    int voice = voice_from_handle(handle);
    if (voice < 0) return;
    if (!d3d_voice_is_sfx_or_unknown(voice)) return;

    of_mixer_set_volume_h(active_voices[voice].mixer_handle, volume);
}

void d3d_sound_set_owned(int handle)
{
    int voice = voice_from_handle(handle);
    if (voice < 0) return;
    active_voices[voice].has_owner = 1;
}

/*
 * Pump MIDI + expire finished voices.  Called from sampletimer
 * (via getpackets, frequent) and _nextpage (once per frame).
 *
 * Voice completion uses the mixer ended mask when available, plus a
 * conservative duration timer as a fallback.
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

    /* Reclaim leaked MUSIC voices.  of_smp_voice.h requires this be called
     * periodically from the main thread and nothing ever did, so every voice
     * the synth dropped on a stale handle generation kept its HW slot (and
     * kept sounding, if looped).  Those orphans accumulate in a pool the two
     * groups SHARE, so the visible symptom is on the SFX side: the free scan
     * fails earlier and earlier, and since the kernel allocator steals a
     * same-group victim before crossing groups, new effects cut off older
     * effects while the dead music voices sit untouched.
     *
     * Throttled rather than run every pump: this is called from sampletimer
     * as well as once per frame, and the scan costs a service call per
     * unowned slot.  Orphans are rare and never urgent — the reaper's own
     * grace window already defers voices mid-fade. */
    if ((int32_t)(now - next_orphan_reap_ms) >= 0) {
        next_orphan_reap_ms = now + D3D_ORPHAN_REAP_INTERVAL_MS;
        int reaped = smp_voice_reap_orphans();
        if (reaped > 0) {
            orphan_reap_total += (uint32_t)reaped;
            printf("[d3d_audio] reaped %d orphaned music voice%s (%u total)\n",
                   reaped, reaped == 1 ? "" : "s",
                   (unsigned)orphan_reap_total);
        }
    }
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
        if (active_voices[i].voice < 0) continue;
        if (active_voices[i].expire_ms == 0) continue;  /* looping — no expiry */
        if ((int32_t)(now - active_voices[i].expire_ms) >= 0)
            complete_voice(i);
    }
}

void d3d_audio_pump_loading(void)
{
    if (!audio_initialized) return;

    /* Loading/preload paths can spend long stretches in file I/O, cache
     * flushes and SFX decode without reaching sampletimer(). Keep the mixer
     * ring fed, but avoid advancing Duke timers or running game callbacks.
     */
    of_mixer_pump();
}
