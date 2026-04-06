/*
 * of_services.h -- openfpgaOS OS Services Table
 *
 * Direct function pointer table for OS services. Apps call through
 * this table instead of ecall, saving ~50 cycles per call.
 *
 * The table lives at 0x7A00 in BRAM. The kernel populates it before
 * launching the app. Old apps compiled against the ecall-based SDK
 * still work unchanged.
 *
 *   #define OF_SVC ((const struct of_services_table *)0x7A00)
 *   uint8_t *fb = OF_SVC->video_get_surface();
 */

#ifndef OF_SERVICES_H
#define OF_SERVICES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_SVC_MAGIC    0x4F535643  /* 'OSVC' */
#define OF_SVC_VERSION  1
#define OF_SVC_ADDR     0x00007A00  /* BRAM */

/* Forward declare input state struct */
struct of_input_state;

struct of_services_table {
    uint32_t magic;
    uint32_t version;
    uint32_t count;         /* Number of function pointers */

    /* -- Video (12) -- */
    void      (*video_init)(void);
    uint8_t * (*video_get_surface)(void);
    uint8_t * (*video_flip)(void);
    void      (*video_wait_flip)(void);
    void      (*video_vsync)(void);
    void      (*video_set_palette)(uint8_t index, uint32_t rgb);
    void      (*video_set_palette_bulk)(const uint32_t *pal, int count);
    void      (*video_set_palette_vga4)(const uint8_t *vga_pal, int count);
    void      (*video_clear)(uint8_t color);
    void      (*video_flush_cache)(void);
    void      (*video_set_display_mode)(int mode);
    void      (*video_set_color_mode)(int mode);

    /* -- Input (4) -- */
    void      (*input_poll)(void);
    void      (*input_get_state)(int player, void *out);
    void      (*input_poll_p0)(void *out);
    void      (*input_set_deadzone)(int16_t deadzone);

    /* -- Mixer (22) -- */
    void      (*mixer_init)(int max_voices, int output_rate);
    int       (*mixer_play)(const uint8_t *pcm_s16, uint32_t sample_count,
                            uint32_t sample_rate, int priority, int volume);
    void      (*mixer_stop)(int voice);
    void      (*mixer_stop_all)(void);
    void      (*mixer_set_volume)(int voice, int volume);
    void      (*mixer_set_pan)(int voice, int pan);
    int       (*mixer_voice_active)(int voice);
    void      (*mixer_pump)(void);
    void      (*mixer_set_loop)(int voice, int loop_start, int loop_end);
    void      (*mixer_set_rate)(int voice, int sample_rate_hz);
    void      (*mixer_set_rate_raw)(int voice, uint32_t rate_fp16);
    void      (*mixer_set_vol_lr)(int voice, int vol_l, int vol_r);
    void      (*mixer_set_bidi)(int voice, int enable);
    int       (*mixer_get_position)(int voice);
    void      (*mixer_set_position)(int voice, int sample_offset);
    void      (*mixer_set_voice)(int voice, int sample_rate_hz, int vol_l, int vol_r);
    void      (*mixer_set_voice_raw)(int voice, uint32_t rate_fp16, int vol_l, int vol_r);
    void      (*mixer_set_vol_rate)(int voice, int rate);
    uint32_t  (*mixer_poll_ended)(void);
    void *    (*mixer_alloc_samples)(uint32_t size);
    void      (*mixer_free_samples)(void);
    void      (*mixer_set_end_callback)(void (*cb)(uint32_t ended_mask));

    /* -- Audio (5) -- */
    void      (*audio_init)(void);
    int       (*audio_write)(const int16_t *samples, int count);
    int       (*audio_get_free)(void);
    void      (*opl_write)(uint16_t reg, uint8_t val);
    void      (*opl_reset)(void);

    /* -- Timer (5) -- */
    void      (*timer_set_callback)(void (*cb)(void), uint32_t hz);
    void      (*timer_stop)(void);
    uint32_t  (*timer_get_us)(void);
    uint32_t  (*timer_get_ms)(void);
    void      (*timer_delay_us)(uint32_t us);

    /* -- Cache (3) -- */
    void      (*cache_flush)(void);
    void      (*cache_clean_range)(void *addr, uint32_t size);
    void      (*cache_inval_range)(void *addr, uint32_t size);

    /* -- Vsync callback (1) -- */
    void      (*video_set_vsync_callback)(void (*cb)(void));

    /* -- File (2) -- */
    long      (*file_size)(const char *path);
    long      (*file_size_fd)(int fd);

    /* -- Mixer extensions (append-only to preserve ABI) -- */
    void      (*mixer_retrigger)(int voice, const uint8_t *pcm_s16,
                                 uint32_t sample_count, uint32_t sample_rate,
                                 int volume);
    int       (*mixer_play_8bit)(const uint8_t *pcm_s8, uint32_t sample_count,
                                 uint32_t sample_rate, int priority, int volume);
    void      (*mixer_set_group)(int voice, int group);
    void      (*mixer_set_group_volume)(int group, int volume);
    void      (*mixer_set_master_volume)(int volume);
    int       (*audio_stream_open)(int sample_rate);
    int       (*audio_stream_write)(const int16_t *samples, int count);
    int       (*audio_stream_ready)(void);
    void      (*audio_stream_close)(void);
};

#ifndef OF_PC

#define OF_SVC ((const struct of_services_table *)OF_SVC_ADDR)

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SERVICES_H */
