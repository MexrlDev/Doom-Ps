/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v5: removed ring buffer / dg_audio_callback indirection.  dg_audio_callback
 *     now submits directly.  The audio THREAD (created in main.c) calls
 *     I_SubmitSound in a tight loop, blocking on sceAudioOutOutput.
 *     This decouples audio from the render loop.
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

extern void dg_audio_callback(const short *pcm, int sample_count);
extern void ps_sound_log(const char *msg);

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
static int sound_start_count = 0;

/* -------------------------------------------------------------------- */
void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
    ps_sound_log("I_InitSound done");
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

/* -------------------------------------------------------------------- */
void I_SubmitSound(void) {
    for (int i = 0; i < MIXBUF * 2; i++) mixbuf[i] = 0;

    int active_channels = 0;
    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;
        active_channels++;

        int pos = ch->pos;
        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= ch->length) { ch->active = 0; break; }

            /* DMX: 8-bit unsigned PCM centered at 128 → 16-bit signed */
            int sample = ((int)ch->data[idx] - 128) << 8;

            /* Doom volume is 0-127.  96 = "full" in original code.
             * Divide by 80 to get a modest boost so it's audible. */
            sample = sample * ch->volume / 80;

            /* Pan: 0=left, 128=center, 255=right.  Boost center. */
            int left  = sample * (255 - ch->pan) / 200;
            int right = sample * ch->pan / 200;

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

    /* Submit (blocks until hardware consumes) */
    dg_audio_callback(mixbuf, MIXBUF);

    (void)active_channels;
}

/* -------------------------------------------------------------------- */
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

    if (sound_start_count < 20) {
        sound_start_count++;
        char b[64]; int p = 0;
        const char *m = "Snd: start ";
        while (*m) b[p++] = *m++;
        const char *n = DEH_String(sfxinfo->name);
        int i = 0;
        while (n[i] && p < 50) b[p++] = n[i++];
        b[p++] = ' '; b[p++] = 'v'; b[p++] = 'o'; b[p++] = 'l'; b[p++] = '=';
        if (ch->volume >= 100) b[p++] = '0' + (ch->volume / 100) % 10;
        if (ch->volume >= 10)  b[p++] = '0' + (ch->volume / 10) % 10;
        b[p++] = '0' + ch->volume % 10;
        b[p++] = '\n'; b[p] = 0;
        ps_sound_log(b);
    }

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

/* Music stubs */
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
