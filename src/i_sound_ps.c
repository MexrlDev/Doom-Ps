/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v3: removed `snd_channels` — it's defined in doomgeneric/s_sound.c,
 *     redefining it caused a link error ("multiple definition").
 *
 * Implements the modern doomgeneric I_* sound API.  Sound effects
 * play through dg_audio_callback() (defined in main.c), which pushes
 * samples via sceAudioOutOutput on the console.  Music is stubbed.
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

/* Implemented in main.c — pushes stereo samples to sceAudioOut */
extern void dg_audio_callback(const short *pcm, int sample_count);

/* ============================================================
 * Global sound configuration.
 * NOTE: snd_channels is defined in s_sound.c — DO NOT redefine.
 * ============================================================ */
int snd_musicdevice = 0;
int snd_sfxdevice   = 0;
int snd_musicvolume = 8;
int snd_sfxvolume   = 8;

#define NCHANNELS   16
#define MIXBUF      512

typedef struct {
    int active;
    int volume;
    int pan;
    int step;
    int pos;
    const unsigned char *data;
    int length;
} channel_t;

static channel_t channels[NCHANNELS];
static short mixbuf[MIXBUF * 2];

/* ============================================================
 * Config variable binding
 * ============================================================ */
void I_BindSoundVariables(void) {
    /* No config vars exposed to user yet. */
}

/* ============================================================
 * Sound lifecycle
 * ============================================================ */
void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
}

void I_ShutdownSound(void) {
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    return W_GetNumForName(namebuf);
}

void I_UpdateSound(void) {}
void I_SetChannels(void) {}
void I_SetSfxVolume(int volume) { (void)volume; }

void I_SubmitSound(void) {
    for (int i = 0; i < MIXBUF * 2; i++) mixbuf[i] = 0;

    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        int pos = ch->pos;
        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= ch->length) { ch->active = 0; break; }

            int sample = (int)ch->data[idx] - 128;
            sample = sample * ch->volume / 96;

            int left  = sample * (255 - ch->pan) / 255;
            int right = sample * ch->pan / 255;

            mixbuf[i * 2]     += (short)left;
            mixbuf[i * 2 + 1] += (short)right;

            pos += ch->step;
        }
        ch->pos = pos;
    }

    for (int i = 0; i < MIXBUF * 2; i++) {
        if (mixbuf[i] > 32767) mixbuf[i] = 32767;
        else if (mixbuf[i] < -32768) mixbuf[i] = -32768;
    }

    dg_audio_callback(mixbuf, MIXBUF);
}

/* ============================================================
 * Sound effects
 * ============================================================ */
int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= NCHANNELS) return -1;
    if (!sfxinfo) return -1;

    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) {
        lumpnum = I_GetSfxLumpNum(sfxinfo);
        sfxinfo->lumpnum = lumpnum;
    }

    unsigned char *lump =
        (unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return -1;

    /* DMX sound format: [0x03 0x00 rate_lo rate_hi len32 pad16 samples] */
    if (lump[0] != 0x03 || lump[1] != 0x00) return -1;

    int length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);
    unsigned char *samples = lump + 24;

    channel_t *ch = &channels[channel];
    ch->active = 1;
    ch->volume = vol;
    ch->pan = sep;
    ch->step = 1 << 16;
    ch->pos = 0;
    ch->data = samples;
    ch->length = length;

    return channel;
}

void I_StopSound(int channel) {
    if (channel < 0 || channel >= NCHANNELS) return;
    channels[channel].active = 0;
}

boolean I_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= NCHANNELS) return 0;
    return channels[channel].active ? 1 : 0;
}

void I_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= NCHANNELS) return;
    if (!channels[channel].active) return;
    channels[channel].volume = vol;
    channels[channel].pan = sep;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

/* ============================================================
 * Music (stubbed to silence)
 * ============================================================ */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *data, int len) { (void)data; (void)len; return NULL; }
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
void I_StopSong(void) {}
boolean I_IsSongPlaying(void) { return 0; }
boolean I_MusicIsPlaying(void) { return 0; }
