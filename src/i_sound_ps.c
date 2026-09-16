/*
 * i_sound_ps.c — PS4/PS5 audio output hook for doomgeneric
 *
 * doomgeneric's own i_sound.c performs the mixing and calls
 * I_SubmitSound() once per game tic.  We override that hook and
 * forward the mixed stereo S16 buffer to dg_audio_callback(), which
 * pushes it into the ring buffer drained by DG_DrawFrame().
 *
 * Do not define any other I_* symbol here — i_sound.c already
 * provides them.  Doing so causes a "multiple definition" link error.
 */

#include "doomtype.h"
#include "i_sound.h"
#include "doomgeneric_ps.h"

/* doomgeneric mixes 512 stereo frames per tic by default. */
#define MIX_FRAMES 512

/* Owned by doomgeneric/i_sound.c */
extern short *mixsound;

void I_SubmitSound(void) {
    if (mixsound)
        dg_audio_callback(mixsound, MIX_FRAMES);
}
