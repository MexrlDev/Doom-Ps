/*
 * i_sound_ps.c — PS4/PS5 sound backend for doomgeneric
 *
 * Implements the I_Sound* and I_Music* symbols that doomgeneric's
 * linker expects.  We route PCM output through dg_audio_callback(),
 * which enqueues it into the ring buffer drained by DG_DrawFrame().
 *
 * This file is compiled with the same flags as main.c (ffreestanding,
 * nostdlib) but it also #includes doomgeneric's own headers so we get
 * the exact struct/function signatures right.
 *
 * How the mixer reaches us:
 *   doomgeneric's i_sound.c declares  "extern short *mixsound;"
 *   and calls I_SubmitSound() after mixing one game-tic of audio.
 *   We pick that buffer up and forward it here.
 *
 *   If your cloned doomgeneric doesn't expose mixsound (the name
 *   changed in some forks), see the NOTE below for alternatives.
 */

/* doomgeneric headers — present only after 'git clone doomgeneric' */
#include "doomtype.h"
#include "i_sound.h"

/* Our callback declared in doomgeneric_ps.h */
#include "doomgeneric_ps.h"

/* How many stereo S16 frames doomgeneric mixes per tic.
 * doomgeneric defaults to 512.  Match SAMPLES_PER_BUF in core.h. */
#define MIX_FRAMES 512

/* =========================================================================
 * Mixer output hook
 *
 * doomgeneric's default i_sound.c calls I_SubmitSound() after each tic.
 * It fills the global buffer pointed to by 'mixsound' (stereo S16,
 * MIX_FRAMES stereo pairs).
 *
 * NOTE: if your doomgeneric fork uses a different symbol name, change
 * "mixsound" below to whatever it exports, or add:
 *
 *   extern short mixbuffer[];
 *
 * and use that instead.
 * ========================================================================= */
extern short *mixsound;

void I_SubmitSound(void) {
    if (mixsound)
        dg_audio_callback(mixsound, MIX_FRAMES);
}

/* =========================================================================
 * Required I_Sound* stubs
 * doomgeneric calls these to initialise / drive the sound system.
 * We delegate the sfx look-up to doomgeneric's WAD layer; everything
 * else is a no-op or stub because the real channel mixing is already
 * done inside doomgeneric's i_sound.c — we only need to deliver the
 * mixed output.
 * ========================================================================= */

void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) {}

/*
 * I_GetSfxLumpNum — look up "ds<name>" in the WAD.
 * W_CheckNumForName is provided by doomgeneric's w_wad.c.
 */
int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    extern int W_CheckNumForName(const char *name);
    char buf[16];
    buf[0] = 'd'; buf[1] = 's';
    int i = 2;
    const char *n = sfx->name;
    while (*n && i < 14) buf[i++] = *n++;
    buf[i] = 0;
    return W_CheckNumForName(buf);
}

int  I_StartSound(sfxinfo_t *sfx, int ch, int vol, int sep)
     { (void)sfx; (void)ch; (void)vol; (void)sep; return ch; }
void I_StopSound(int handle)            { (void)handle; }
boolean I_SoundIsPlaying(int handle)    { (void)handle; return false; }
void I_UpdateSound(void)                {}
void I_UpdateSoundParams(int h, int v, int s) { (void)h; (void)v; (void)s; }

/* =========================================================================
 * Music stubs — Doom runs fine without MIDI on console.
 * ========================================================================= */
void    I_InitMusic(void)               {}
void    I_ShutdownMusic(void)           {}
void    I_SetMusicVolume(int v)         { (void)v; }
void    I_PauseSong(void)               {}
void    I_ResumeSong(void)              {}
void   *I_RegisterSong(void *d, int l)  { (void)d; (void)l; return 0; }
void    I_PlaySong(void *h, boolean l)  { (void)h; (void)l; }
void    I_StopSong(void)                {}
void    I_UnRegisterSong(void *h)       { (void)h; }
boolean I_MusicIsPlaying(void)          { return false; }
