/*
 * d3d_audio.h — Sound system for Duke3D on openfpgaOS
 */

#ifndef D3D_AUDIO_H
#define D3D_AUDIO_H

void d3d_audio_init(void);
void d3d_sound_set_fx_volume(int volume);
int  d3d_sound_precache(int sound_num);
int  d3d_sound_precache_all(void);
int  d3d_sound_play(int sound_num, int priority, int volume);
int  d3d_sound_play_3d(int sound_num, int priority, int angle, int distance);
int  d3d_sound_play_pitch(int sound_num, int priority, int volume, int pitch);
int  d3d_sound_play_3d_pitch(int sound_num, int priority, int angle, int distance, int pitch);
void d3d_sound_stop(int handle);
void d3d_sound_stop_all(void);
void d3d_sound_set_volume(int handle, int volume);
void d3d_sound_set_pan(int handle, int angle, int distance);
void d3d_sound_set_loop(int handle);
void d3d_sound_set_owned(int handle);  /* mark handle as tracked in SoundOwner */
void d3d_audio_pump(void);
void d3d_audio_pump_loading(void);
void d3d_audio_shutdown(void);

#endif
