/*
 * of_syscall_numbers.h -- Canonical syscall numbers for openfpgaOS
 *
 * Single source of truth for all openfpgaOS HAL syscall numbers.
 * Used by both the kernel (syscall dispatch) and the API (user-side).
 */

#ifndef OF_SYSCALL_NUMBERS_H
#define OF_SYSCALL_NUMBERS_H

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

/* Save */
#define OF_SYS_SAVE_READ               0x1030
#define OF_SYS_SAVE_WRITE              0x1031
#define OF_SYS_SAVE_FLUSH              0x1032
#define OF_SYS_SAVE_ERASE              0x1033
#define OF_SYS_SAVE_FLUSH_SIZE         0x1034

/* Analogizer */
#define OF_SYS_ANALOGIZER_GET_STATE    0x1040
#define OF_SYS_ANALOGIZER_IS_ENABLED   0x1041

/* Terminal */
#define OF_SYS_TERM_PUTCHAR            0x1050
#define OF_SYS_TERM_CLEAR              0x1051
#define OF_SYS_TERM_PRINTF             0x1052
#define OF_SYS_TERM_SET_POS            0x1053

/* Link cable */
#define OF_SYS_LINK_SEND               0x1060
#define OF_SYS_LINK_RECV               0x1061
#define OF_SYS_LINK_GET_STATUS         0x1062

/* Timer */
#define OF_SYS_TIMER_GET_US            0x1070
#define OF_SYS_TIMER_GET_MS            0x1071
#define OF_SYS_TIMER_DELAY_US          0x1072
#define OF_SYS_TIMER_DELAY_MS          0x1073

/* File (data slots) */
#define OF_SYS_FILE_READ               0x1080
#define OF_SYS_FILE_SIZE               0x1081

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

/* File slot registry */
#define OF_SYS_FILE_SLOT_COUNT         0x10B1
#define OF_SYS_FILE_SLOT_GET           0x10B2
#define OF_SYS_FILE_SLOT_REGISTER      0x10B3

/* Idle hook */
#define OF_SYS_SET_IDLE_HOOK           0x10B4

/* Audio ring buffer */
#define OF_SYS_AUDIO_ENQUEUE           0x10B5
#define OF_SYS_AUDIO_RING_FREE         0x10B6

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

/* Audio Codec */
#define OF_SYS_CODEC_PARSE_VOC         0x10D8
#define OF_SYS_CODEC_PARSE_WAV         0x10D9

/* LZW Compression */
#define OF_SYS_LZW_COMPRESS            0x10E0
#define OF_SYS_LZW_UNCOMPRESS          0x10E1

#endif /* OF_SYSCALL_NUMBERS_H */
