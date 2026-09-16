#ifndef DOOMGENERIC_PS_H
#define DOOMGENERIC_PS_H

/*
 * Six doomgeneric callbacks — doomgeneric.c calls these.
 */
void DG_Init(void);
void DG_DrawFrame(void);
void DG_SleepMs(unsigned int ms);
unsigned int DG_GetTicksMs(void);
int  DG_GetKey(int *pressed, unsigned char *doomKey);
void DG_SetWindowTitle(const char *title);

/*
 * Wire this from your i_sound_ps.c mixer output into the audio ring buffer.
 * pcm          = interleaved stereo S16  (L,R,L,R,...)
 * sample_count = stereo frame count (not bytes)
 */
void dg_audio_callback(const short *pcm, int sample_count);

#endif
