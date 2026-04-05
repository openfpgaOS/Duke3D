/*
 * of_syscall_numbers.h -- Canonical syscall numbers for openfpgaOS
 *
 * Single source of truth for all openfpgaOS HAL syscall numbers.
 * Used by both the kernel (syscall dispatch) and the API (user-side).
 */

#ifndef OF_SYSCALL_NUMBERS_H
#define OF_SYSCALL_NUMBERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Canonical syscall numbers (OF_SYS_* prefix)
 * ====================================================================== */

/* Video */
#define OF_SYS_VIDEO_INIT              0x1000
#define OF_SYS_VIDEO_FLIP              0x1001
#define OF_SYS_VIDEO_WAIT_FLIP         0x1002
#define OF_SYS_VIDEO_SET_PALETTE       0x1003
#define OF_SYS_VIDEO_GET_SURFACE       0x1004
#define OF_SYS_VIDEO_SET_DISPLAY_MODE  0x1005
#define OF_SYS_VIDEO_CLEAR             0x1006
#define OF_SYS_VIDEO_SET_PALETTE_BULK  0x1007
#define OF_SYS_VIDEO_FLUSH_CACHE       0x1008
#define OF_SYS_VIDEO_SET_COLOR_MODE    0x1009
#define OF_SYS_VIDEO_SET_PALETTE_VGA4 0x100A
#define OF_SYS_VIDEO_VSYNC             0x100B

/* Audio */
#define OF_SYS_AUDIO_WRITE             0x1010
#define OF_SYS_AUDIO_GET_FREE          0x1011
#define OF_SYS_OPL_WRITE               0x1012
#define OF_SYS_OPL_RESET               0x1013
#define OF_SYS_AUDIO_INIT              0x1014

/* Input */
#define OF_SYS_INPUT_POLL              0x1020
#define OF_SYS_INPUT_GET_STATE         0x1021
#define OF_SYS_INPUT_SET_DEADZONE      0x1022
#define OF_SYS_INPUT_POLL_P0           0x1023

/* Save — handled internally by fclose() */

/* Analogizer */
#define OF_SYS_ANALOGIZER_GET_STATE    0x1040
#define OF_SYS_ANALOGIZER_IS_ENABLED   0x1041


/* Networking (replaces Link cable) */
#define OF_SYS_NET_HOST_START          0x1060
#define OF_SYS_NET_JOIN                0x1061
#define OF_SYS_NET_STOP                0x1062
#define OF_SYS_NET_STATUS              0x1063
#define OF_SYS_NET_CLIENT_COUNT        0x1064
#define OF_SYS_NET_SEND_TO             0x1065
#define OF_SYS_NET_RECV_FROM           0x1066
#define OF_SYS_NET_BROADCAST           0x1067
#define OF_SYS_NET_SEND                0x1068
#define OF_SYS_NET_RECV                0x1069
#define OF_SYS_NET_POLL                0x106A

/* Terminal */
#define OF_SYS_TERM_PUTCHAR            0x1070

/* Timer (time/delay via POSIX clock_gettime/usleep) */
#define OF_SYS_TIMER_SET_CALLBACK      0x1074
#define OF_SYS_TIMER_STOP              0x1075
#define OF_SYS_TIMER_GET_US            0x1076
#define OF_SYS_TIMER_GET_MS            0x1077
#define OF_SYS_TIMER_DELAY_US          0x1078

/* File — handled internally by fread/fseek */

/* Tile engine */
#define OF_SYS_TILE_ENABLE             0x1090
#define OF_SYS_TILE_SCROLL             0x1091
#define OF_SYS_TILE_SET                0x1092
#define OF_SYS_TILE_LOAD_MAP           0x1093
#define OF_SYS_TILE_LOAD_CHR           0x1094

/* Sprite engine */
#define OF_SYS_SPRITE_ENABLE           0x10A0
#define OF_SYS_SPRITE_SET              0x10A1
#define OF_SYS_SPRITE_MOVE             0x10A2
#define OF_SYS_SPRITE_LOAD_CHR         0x10A3
#define OF_SYS_SPRITE_HIDE             0x10A4
#define OF_SYS_SPRITE_HIDE_ALL         0x10A5

/* Version */
#define OF_SYS_GET_VERSION             0x10B0

/* Idle hook — internal, called during DMA waits */

/* Memory allocation */
#define OF_SYS_MALLOC                  0x10C0
#define OF_SYS_FREE                    0x10C1
#define OF_SYS_REALLOC                 0x10C2
#define OF_SYS_CALLOC                  0x10C3

/* Audio Mixer */
#define OF_SYS_MIXER_INIT              0x10D0
#define OF_SYS_MIXER_PLAY              0x10D1
#define OF_SYS_MIXER_STOP              0x10D2
#define OF_SYS_MIXER_STOP_ALL          0x10D3
#define OF_SYS_MIXER_SET_VOLUME        0x10D4
#define OF_SYS_MIXER_PUMP              0x10D5
#define OF_SYS_MIXER_VOICE_ACTIVE      0x10D6
#define OF_SYS_MIXER_SET_PAN           0x10D7
#define OF_SYS_MIXER_SET_LOOP          0x10D8
#define OF_SYS_MIXER_SET_RATE          0x10D9
#define OF_SYS_MIXER_SET_VOL_LR        0x10DA
#define OF_SYS_MIXER_SET_BIDI          0x10DB
#define OF_SYS_MIXER_GET_POSITION      0x10DC
#define OF_SYS_MIXER_SET_POSITION      0x10DD
#define OF_SYS_MIXER_SET_VOICE         0x10DE
#define OF_SYS_MIXER_ALLOC_SAMPLES     0x10DF
#define OF_SYS_MIXER_FREE_SAMPLES      0x10E0
#define OF_SYS_MIXER_SET_RATE_RAW      0x10E1
#define OF_SYS_MIXER_SET_VOICE_RAW     0x10E2
#define OF_SYS_MIXER_SET_VOL_RATE      0x10E3
#define OF_SYS_MIXER_POLL_ENDED        0x10E4

/* Audio Codec */
#define OF_SYS_CODEC_PARSE_VOC         0x10E5
#define OF_SYS_CODEC_PARSE_WAV         0x10E6

/* LZW Compression */
#define OF_SYS_LZW_COMPRESS            0x10E7
#define OF_SYS_LZW_UNCOMPRESS          0x10E8

/* Interact */
#define OF_SYS_INTERACT_GET            0x10F0

#ifdef __cplusplus
}
#endif

#endif /* OF_SYSCALL_NUMBERS_H */
