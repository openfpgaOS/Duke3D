/*
 * d3d_audio.h — Minimal sound mixer for Duke3D on openfpgaOS
 */

#ifndef D3D_AUDIO_H
#define D3D_AUDIO_H

void d3d_audio_init(void);
int  d3d_sound_play(int sound_num, int priority, int volume);
void d3d_sound_stop_all(void);
void d3d_audio_pump(void);

#endif
