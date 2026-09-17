/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * This file REPLACES doomgeneric/i_sound.c (excluded in Makefile).
 * It implements Doom's I_* sound API.  Sound effects are mixed into
 * a stereo buffer and submitted via dg_audio_callback() which pushes
 * them through sceAudioOutOutput on the console.
 *
 * Music (Redbook / MIDI) is not supported yet — game sounds play fine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "s_sound.h"
#include "z_zone.h"

#include "core.h"
#include "doomgeneric_ps.h"

/* dg_audio_callback is implemented in main.c; pushes to sceAudioOut */
extern void dg_audio_callback(const short *pcm, int sample_count);

#define NCHANNELS   16
#define MIXBUF      512   /* stereo frames per submit */

typedef struct {
    int active;
    int handle;
    int sfx_id;
    int volume;          /* 0-127 Doom volume */
    int pan;             /* 0=left, 128=center, 255=right */
    int step;            /* pitch, 16.16 fixed point */
    int pos;             /* current pos, 16.16 fixed point */
    const unsigned char *data;
    int length;          /* sample count */
} channel_t;

static channel_t channels[NCHANNELS];
static short mixbuf[MIXBUF * 2];
static int next_handle = 1;

/* ====== Doom I_* sound API ====== */

void I_InitSound(void) {
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
}

void I_UpdateSound(void) {
    /* Nothing per-tick; we mix on Submit */
}

void I_SubmitSound(void) {
    /* Clear mix buffer */
    for (int i = 0; i < MIXBUF * 2; i++) mixbuf[i] = 0;

    /* Mix all active channels */
    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        int pos = ch->pos;
        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= ch->length) { ch->active = 0; break; }

            /* Doom DMX: 8-bit unsigned samples, centered at 128 */
            int sample = (int)ch->data[idx] - 128;

            /* Scale by volume (Doom volume is 0-127) */
            sample = sample * ch->volume / 96;

            /* Stereo pan */
            int left  = sample * (255 - ch->pan) / 255;
            int right = sample * ch->pan / 255;

            mixbuf[i * 2]     += (short)left;
            mixbuf[i * 2 + 1] += (short)right;

            pos += ch->step;
        }
        ch->pos = pos;
    }

    /* Soft clip */
    for (int i = 0; i < MIXBUF * 2; i++) {
        if (mixbuf[i] > 32767) mixbuf[i] = 32767;
        else if (mixbuf[i] < -32768) mixbuf[i] = -32768;
    }

    /* Push to console */
    dg_audio_callback(mixbuf, MIXBUF);
}

void I_ShutdownSound(void) {
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    return W_GetNumForName(namebuf);
}

void I_SetChannels(void) {}

void I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    /* Find a free channel */
    int cidx = -1;
    for (int i = 0; i < NCHANNELS; i++) {
        if (!channels[i].active) { cidx = i; break; }
    }
    if (cidx < 0) cidx = 0;   /* steal channel 0 if all busy */

    sfxinfo_t *sfx = &S_sfx[id];
    int lumpnum = sfx->lumpnum;
    if (lumpnum < 0) {
        lumpnum = I_GetSfxLumpNum(sfx);
        sfx->lumpnum = lumpnum;
    }

    unsigned char *lump =
        (unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return;

    /* DMX sound format: [0x03 0x00 rate_lo rate_hi len32 pad16 samples] */
    if (lump[0] != 0x03 || lump[1] != 0x00) return;

    int length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);
    unsigned char *samples = lump + 24;

    channel_t *ch = &channels[cidx];
    ch->active = 1;
    ch->handle = next_handle++;
    ch->sfx_id = id;
    ch->volume = vol;
    ch->pan = sep;
    /* Pitch from Doom is a value where 128 = normal.
     * We convert to 16.16 fixed-point step. */
    int step = (pitch > 0) ? ((128 << 16) / pitch) : (1 << 16);
    ch->step = step;
    ch->pos = 0;
    ch->data = samples;
    ch->length = length;
}

void I_StopSound(int handle) {
    for (int i = 0; i < NCHANNELS; i++) {
        if (channels[i].active && channels[i].handle == handle)
            channels[i].active = 0;
    }
}

int I_SoundIsPlaying(int handle) {
    for (int i = 0; i < NCHANNELS; i++) {
        if (channels[i].active && channels[i].handle == handle)
            return 1;
    }
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    for (int i = 0; i < NCHANNELS; i++) {
        if (channels[i].active && channels[i].handle == handle) {
            channels[i].volume = vol;
            channels[i].pan = sep;
            int step = (pitch > 0) ? ((128 << 16) / pitch) : (1 << 16);
            channels[i].step = step;
        }
    }
}

/* ====== Music (not implemented) ====== */

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) {}
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *data, int len) { (void)data; (void)len; return NULL; }
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(void) {}
int I_IsSongPlaying(void) { return 0; }
