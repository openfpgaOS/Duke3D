/*
 * d3d_audio.h — Sound system for Duke3D on openfpgaOS
 */

#ifndef D3D_AUDIO_H
#define D3D_AUDIO_H

void d3d_audio_init(void);
int  d3d_sound_play(int sound_num, int priority, int volume);
int  d3d_sound_play_3d(int sound_num, int priority, int angle, int distance);
void d3d_sound_stop(int voice);
void d3d_sound_stop_all(void);
void d3d_sound_set_volume(int voice, int volume);
void d3d_sound_set_owned(int voice);  /* mark voice as tracked in SoundOwner */
void d3d_audio_pump(void);

#endif
