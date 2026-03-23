/*
 * openfpgaOS WAV Player Demo
 *
 * Loads a WAV file from data slot 3 and plays it through the
 * 48kHz stereo audio FIFO. Supports 8/16-bit, mono/stereo WAV
 * files at any sample rate (resampled to 48kHz).
 *
 * Controls:
 *   START  = play/pause
 *   SELECT = restart
 */

#include "of.h"
#include <stdio.h>
#include <string.h>

#define WAV_SLOT_ID     3
#define MAX_WAV_SIZE    (4 * 1024 * 1024)  /* 4MB max */
#define OUTPUT_RATE     48000

/* WAV file buffer */
static uint8_t wav_buf[MAX_WAV_SIZE] __attribute__((aligned(4)));

/* WAV header parsing */
typedef struct {
    uint16_t format;        /* 1 = PCM */
    uint16_t channels;      /* 1 = mono, 2 = stereo */
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    const uint8_t *data;    /* pointer to PCM data */
    uint32_t data_size;     /* size of PCM data in bytes */
} wav_info_t;

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wav_parse(const uint8_t *buf, uint32_t len, wav_info_t *info) {
    if (len < 44) return -1;
    if (memcmp(buf, "RIFF", 4) != 0) return -2;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return -3;

    /* Walk chunks to find fmt and data */
    uint32_t pos = 12;
    int have_fmt = 0;

    while (pos + 8 <= len) {
        uint32_t chunk_size = read32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (chunk_size < 16) return -4;
            info->format = read16(buf + pos + 8);
            info->channels = read16(buf + pos + 10);
            info->sample_rate = read32(buf + pos + 12);
            info->byte_rate = read32(buf + pos + 16);
            info->block_align = read16(buf + pos + 20);
            info->bits_per_sample = read16(buf + pos + 22);
            have_fmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            if (!have_fmt) return -5;
            info->data = buf + pos + 8;
            info->data_size = chunk_size;
            if (pos + 8 + chunk_size > len)
                info->data_size = len - pos - 8;
            return 0;
        }
        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;  /* chunks are word-aligned */
    }
    return -6;  /* no data chunk */
}

/* Resample buffer */
#define RESAMPLE_BUF_SIZE 512
static int16_t resample_buf[RESAMPLE_BUF_SIZE * 2];  /* stereo pairs */

int main(void) {
    of_file_slot_register(3, "audio.wav");
    printf("\033[2J\033[H");  /* clear screen */
    printf("  WAV Player Demo\n\n");
    printf("  Loading WAV file...\n");

    /* Determine file size by walking — read whole file */
    FILE *f = fopen("audio.wav", "rb");
    if (!f) {
        /* Try slot directly */
        f = fopen("slot:3", "rb");
    }
    if (!f) {
        printf("  Error: cannot open WAV file\n");
        printf("  Place a .wav file in data slot 3\n");
        while (1) of_delay_ms(100);
    }

    uint32_t file_size = fread(wav_buf, 1, MAX_WAV_SIZE, f);
    fclose(f);
    printf("  Read %d bytes\n", (int)file_size);

    if (file_size == 0) {
        printf("Error: empty file\n");
        while (1) of_delay_ms(100);
    }

    /* Parse WAV header */
    wav_info_t wav;
    int rc = wav_parse(wav_buf, file_size, &wav);
    if (rc < 0) {
        printf("WAV parse error: %d\n", rc);
        while (1) of_delay_ms(100);
    }

    if (wav.format != 1) {
        printf("Error: not PCM (format=%d)\n", wav.format);
        while (1) of_delay_ms(100);
    }

    printf("  Format: %d-bit %s %dHz\n",
           wav.bits_per_sample,
           wav.channels == 1 ? "mono" : "stereo",
           (int)wav.sample_rate);
    printf("  Data: %d bytes (%.1f sec)\n",
           (int)wav.data_size,
           (float)wav.data_size / wav.byte_rate);
    printf("\n  START=play/pause SELECT=restart\n");

    /* Init audio */
    of_audio_init();

    /* Playback state */
    uint32_t play_pos = 0;
    int playing = 1;
    int paused = 0;

    /* Resampling state: 16.16 fixed point */
    uint32_t resample_frac = 0;
    uint32_t resample_step = (uint32_t)(
        ((uint64_t)wav.sample_rate << 16) / OUTPUT_RATE);

    uint32_t total_samples = wav.data_size / wav.block_align;

    printf("\n  [PLAYING]\n");

    while (1) {
        /* Input */
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);

        if (st.buttons_pressed & OF_BTN_START) {
            paused = !paused;
            playing = !paused;
            printf("\033[8;3H%s     \n",
                   paused ? "[PAUSED] " : "[PLAYING]");
        }
        if (st.buttons_pressed & OF_BTN_SELECT) {
            play_pos = 0;
            resample_frac = 0;
            paused = 0;
            playing = 1;
            printf("\033[8;1H[PLAYING]     \n");
        }

        if (!playing || play_pos >= total_samples) {
            if (play_pos >= total_samples && playing) {
                printf("\033[8;3H[FINISHED]    \n");
                playing = 0;
            }
            of_delay_ms(16);
            continue;
        }

        /* Fill audio FIFO */
        int free_pairs = of_audio_free();
        if (free_pairs <= 0) {
            of_delay_ms(1);
            continue;
        }
        if (free_pairs > RESAMPLE_BUF_SIZE)
            free_pairs = RESAMPLE_BUF_SIZE;

        /* Resample to 48kHz stereo int16 */
        int out_idx = 0;
        for (int i = 0; i < free_pairs; i++) {
            uint32_t src_idx = resample_frac >> 16;
            if (src_idx + play_pos >= total_samples) {
                play_pos = total_samples;
                break;
            }

            uint32_t sample_pos = play_pos + src_idx;
            int16_t left, right;

            if (wav.bits_per_sample == 16) {
                const int16_t *pcm = (const int16_t *)(wav.data + sample_pos * wav.block_align);
                left = pcm[0];
                right = (wav.channels >= 2) ? pcm[1] : left;
            } else {
                /* 8-bit unsigned → 16-bit signed */
                const uint8_t *pcm = wav.data + sample_pos * wav.block_align;
                left = ((int16_t)pcm[0] - 128) << 8;
                right = (wav.channels >= 2) ?
                        ((int16_t)pcm[1] - 128) << 8 : left;
            }

            resample_buf[out_idx++] = left;
            resample_buf[out_idx++] = right;
            resample_frac += resample_step;
        }

        if (out_idx > 0) {
            of_audio_write(resample_buf, out_idx / 2);

            /* Advance play position */
            uint32_t consumed = resample_frac >> 16;
            play_pos += consumed;
            resample_frac &= 0xFFFF;
        }

        /* Progress bar: [███████░░░░░░░░] 50% */
        {
            int pct = (int)((uint64_t)play_pos * 100 / total_samples);
            int filled = pct * 30 / 100;
            printf("\033[10;2H[");
            for (int i = 0; i < 30; i++)
                printf("%c", (char)(unsigned char)(i < filled ? 219 : 176));
            printf("] %d%%  ", pct);
        }
    }

    return 0;
}
