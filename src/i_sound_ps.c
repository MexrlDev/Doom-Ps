/*
 * i_sound_ps.c — Doom sound + music backend for PS4/PS5
 *
 * v7 — fixes:
 *
 *  BUG 1 (MUSIC SILENT): I_RegisterSong receives raw MUS format data
 *  (header "MUS\x1a"), NOT pre-converted MIDI.  The v6 parser looked
 *  for "MThd" and immediately set mus_end_of_track = 1 → no music ever.
 *  Fix: implement a proper MUS parser.  MUS is id Software's own format;
 *  it is simpler than MIDI.  We parse it directly — no mus2mid conversion
 *  needed.
 *
 *  MUS format summary:
 *    Header (16 bytes):
 *      "MUS\x1a"        4 bytes  magic
 *      score_len        u16      length of score data
 *      score_start      u16      offset of score data from file start
 *      pri_channels     u16      number of primary channels
 *      sec_channels     u16      number of secondary channels
 *      num_instruments  u16
 *      reserved         u16
 *    Score data: stream of events, each event 1–5 bytes:
 *      event byte:
 *        bits 7:    "last event in this time group" flag
 *        bits 6..4: event type (0–7)
 *        bits 3..0: channel number (0–8; 9 = percussion)
 *      event types:
 *        0  release note:   1 more byte (note 0..127)
 *        1  play note:      1 or 2 more bytes
 *                           byte0 bit7=has_vol, bits6..0=note
 *                           if has_vol: byte1=volume
 *        2  pitch wheel:    1 more byte (0..255; 128=center)
 *        3  system event:   1 more byte (controller)
 *        4  change ctrl:    2 more bytes (controller, value)
 *        5  unknown/skip:   1 more byte
 *        6  score end
 *        7  unused
 *      After each event group (last-event bit set), a variable-length
 *      delay follows: read bytes while bit7 set, accumulate lower 7 bits.
 *
 *  BUG 2 (SFX WRONG PITCH): Doom SFX lumps are 11025 Hz.  We output
 *  at 48000 Hz.  ch->step must be (src_rate << 16) / dst_rate so we
 *  step through source samples at the correct rate.
 *  11025 * 65536 / 48000 = 15052.  Was hardcoded to 65536 (1:1).
 *
 *  BUG 3 (MUSIC TOO QUIET): volume scale was ×4 of a max mix of 2032.
 *  Raised to ×16 so music is clearly audible.
 *
 * Audio thread (in main.c) calls I_SubmitSound() in a tight loop.
 * sceAudioOutOutput blocks until the hardware consumes the buffer,
 * so no sleep is needed — the hardware pacing IS the loop rate.
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

/* ============================================================
 * Globals required by Doom's sound system
 * ============================================================ */
int snd_musicdevice = 0;
int snd_sfxdevice   = 0;
int snd_musicvolume = 8;   /* 0..15 */
int snd_sfxvolume   = 8;

#define NCHANNELS        16
#define MIXBUF           512
#define MUSIC_MAX_NOTES  16

/* ============================================================
 * SFX channels
 * ============================================================ */
typedef struct {
    int            active;
    int            volume;   /* 0..127 */
    int            pan;      /* 0..255; 128=center */
    unsigned int   step;     /* 16.16 fixed-point sample increment */
    unsigned int   pos;      /* 16.16 fixed-point position in sample data */
    const unsigned char *data;
    int            length;   /* sample count */
} channel_t;

static channel_t channels[NCHANNELS];
static short     mixbuf[MIXBUF * 2];
static int       sound_start_count = 0;

/*
 * SFX source rate — standard Doom WAD sound lumps are 11025 Hz.
 * We read the actual rate from the lump header (bytes 2–3) so
 * custom WADs with different rates also work.
 * Step = (src_rate << 16) / dst_rate
 */
static unsigned int sfx_step_for_rate(unsigned int src_rate) {
    /* (src_rate * 65536) / 48000 */
    return (unsigned int)(((unsigned long long)src_rate * 65536ULL) / SAMPLE_RATE);
}

/* ============================================================
 * Music — MUS format parser + square-wave synthesizer
 * ============================================================ */

/* MUS header (all little-endian) */
typedef struct {
    unsigned char  magic[4];   /* "MUS\x1a" */
    unsigned short score_len;
    unsigned short score_start;
    unsigned short pri_channels;
    unsigned short sec_channels;
    unsigned short num_instruments;
    unsigned short reserved;
} mus_header_t;

/* Per-MUS-channel state */
typedef struct {
    int volume;          /* last set volume for this channel, 0..127 */
} mus_chan_t;

/* Active synthesizer voice */
typedef struct {
    int          active;
    int          note;
    int          mus_channel;
    unsigned int phase;
    unsigned int phase_inc;  /* 16.16 Hz/sample */
    int          env;        /* amplitude 0..127 */
} music_voice_t;

typedef struct {
    unsigned char *data;
    int            len;
} song_t;

static music_voice_t  mus_voices[MUSIC_MAX_NOTES];
static mus_chan_t      mus_chans[16];
static unsigned char *mus_score      = 0;   /* pointer into song data */
static int            mus_score_len  = 0;
static int            mus_pos        = 0;   /* byte offset in score */
static int            mus_loop       = 0;
static int            mus_playing    = 0;
static int            mus_end        = 0;

/* Timing: MUS uses a fixed 70 Hz tick rate (same as Doom's game tic).
 * At 48000 Hz sample rate: 48000 / 70 = ~685.7 samples per tick.
 * We use a sample accumulator. */
#define MUS_TICKS_PER_SEC  70
static unsigned int mus_sample_acc  = 0;   /* accumulated samples × 1000 */
static unsigned int mus_samples_per_tick_x1000 =
    (SAMPLE_RATE * 1000) / MUS_TICKS_PER_SEC;   /* = 685714 */
static unsigned int mus_delay_ticks = 0;   /* ticks remaining before next event */

/* Phase increment table: note → 16.16 phase_inc at 48 kHz */
static unsigned int note_phase_inc[128];

static void init_music_tables(void) {
    /* A4 (note 69) = 440 Hz
     * phase_inc = freq * 65536 / SAMPLE_RATE  (16.16) */
    /* Build upward from A4 */
    /* 2^(1/12) in 16.16 = 69433 */
    const unsigned int SEMI = 69433;
    unsigned int f = (unsigned int)((440ULL * 65536ULL) / SAMPLE_RATE);
    note_phase_inc[69] = f;
    for (int i = 70; i < 128; i++) {
        f = (unsigned int)(((unsigned long long)f * SEMI) >> 16);
        note_phase_inc[i] = f;
    }
    f = (unsigned int)((440ULL * 65536ULL) / SAMPLE_RATE);
    for (int i = 68; i >= 0; i--) {
        f = (unsigned int)(((unsigned long long)f << 16) / SEMI);
        note_phase_inc[i] = f;
    }
}

static void voice_note_on(int ch, int note, int vol) {
    if (note < 0 || note > 127) return;
    if (ch == 9) return; /* skip percussion channel — no pitched sound */
    /* Find free slot; evict oldest if full */
    int slot = -1;
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (!mus_voices[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0;  /* evict slot 0 */
    mus_voices[slot].active      = 1;
    mus_voices[slot].note        = note;
    mus_voices[slot].mus_channel = ch;
    mus_voices[slot].phase       = 0;
    mus_voices[slot].phase_inc   = note_phase_inc[note];
    mus_voices[slot].env         = vol > 0 ? vol : 64;
}

static void voice_note_off(int ch, int note) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (mus_voices[i].active &&
            mus_voices[i].mus_channel == ch &&
            mus_voices[i].note == note) {
            mus_voices[i].active = 0;
        }
    }
}

static void voice_all_off(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        mus_voices[i].active = 0;
}

/* Read the delay field (variable-length, follows last event in a group) */
static void mus_read_delay(void) {
    mus_delay_ticks = 0;
    while (mus_pos < mus_score_len) {
        unsigned char b = mus_score[mus_pos++];
        mus_delay_ticks = (mus_delay_ticks << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
}

/* Process all events for the current tick */
static void mus_process_tick(void) {
    if (!mus_playing || mus_end) return;
    if (mus_delay_ticks > 0) { mus_delay_ticks--; return; }

    /* Process events until we hit a "last in group" event */
    for (;;) {
        if (mus_pos >= mus_score_len) { mus_end = 1; return; }

        unsigned char ev    = mus_score[mus_pos++];
        int           last  = (ev >> 7) & 1;
        int           type  = (ev >> 4) & 7;
        int           chan  = ev & 0x0F;

        switch (type) {
        case 0: { /* Release note */
            if (mus_pos >= mus_score_len) { mus_end = 1; return; }
            int note = mus_score[mus_pos++] & 0x7F;
            voice_note_off(chan, note);
            break;
        }
        case 1: { /* Play note */
            if (mus_pos >= mus_score_len) { mus_end = 1; return; }
            unsigned char b0 = mus_score[mus_pos++];
            int has_vol = (b0 >> 7) & 1;
            int note    = b0 & 0x7F;
            int vol     = mus_chans[chan].volume;
            if (has_vol) {
                if (mus_pos >= mus_score_len) { mus_end = 1; return; }
                vol = mus_score[mus_pos++] & 0x7F;
                mus_chans[chan].volume = vol;
            }
            voice_note_on(chan, note, vol);
            break;
        }
        case 2: /* Pitch wheel — skip 1 byte */
            if (mus_pos < mus_score_len) mus_pos++;
            break;
        case 3: /* System event — skip 1 byte */
            if (mus_pos < mus_score_len) mus_pos++;
            break;
        case 4: /* Change controller — skip 2 bytes */
            if (mus_pos + 1 < mus_score_len) mus_pos += 2;
            break;
        case 5: /* Unknown — skip 1 byte */
            if (mus_pos < mus_score_len) mus_pos++;
            break;
        case 6: /* Score end */
            mus_end = 1;
            return;
        case 7: /* Unused */
            break;
        }

        if (last) {
            /* Read the delay until the next event group */
            mus_read_delay();
            return;
        }
    }
}

/* Synthesize `frames` stereo S16 samples of music into buf (+=) */
static void music_render(short *buf, int frames) {
    if (!mus_playing) return;

    int vol = snd_musicvolume;
    if (vol < 0)  vol = 0;
    if (vol > 15) vol = 15;

    for (int i = 0; i < frames; i++) {
        /* Advance MUS clock */
        mus_sample_acc += 1000;
        while (mus_sample_acc >= mus_samples_per_tick_x1000) {
            mus_sample_acc -= mus_samples_per_tick_x1000;
            mus_process_tick();
            if (mus_end) {
                if (mus_loop) {
                    /* Rewind to start of score */
                    mus_pos   = 0;
                    mus_end   = 0;
                    mus_delay_ticks = 0;
                    mus_sample_acc  = 0;
                    voice_all_off();
                    /* Prime first delay */
                    mus_read_delay();
                } else {
                    mus_playing = 0;
                    voice_all_off();
                }
                break;
            }
        }
        if (!mus_playing) break;

        /* Sum active voices — square wave */
        int mix = 0;
        for (int j = 0; j < MUSIC_MAX_NOTES; j++) {
            music_voice_t *v = &mus_voices[j];
            if (!v->active) continue;
            /* Square: positive half = +env, negative half = -env */
            int s = (v->phase & 0x80000000u) ? -(int)v->env : (int)v->env;
            mix += s;
            v->phase += v->phase_inc;
        }

        /* Scale: 16 voices × 127 max = 2032.
         * ×16 → 32512.  Apply volume (0..15). */
        mix = (mix * 16 * vol) / 15;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;

        buf[i * 2]     += (short)mix;
        buf[i * 2 + 1] += (short)mix;
    }
}

/* ============================================================
 * Doom sound API — required symbols
 * ============================================================ */
void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
    init_music_tables();
    /* Tell Doom's S_Init a music device exists so it calls I_InitMusic */
    if (snd_musicdevice == 0) snd_musicdevice = 3;  /* General MIDI */
    ps_sound_log("I_InitSound: done\n");
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

/* --- Mix SFX + music → submit to audio ring ----------------------- */
void I_SubmitSound(void) {
    /* Zero mix buffer */
    for (int i = 0; i < MIXBUF * 2; i++) mixbuf[i] = 0;

    /* --- SFX --- */
    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        unsigned int pos = ch->pos;
        for (int i = 0; i < MIXBUF; i++) {
            unsigned int idx = pos >> 16;
            if ((int)idx >= ch->length) { ch->active = 0; break; }

            /* Doom SFX are unsigned 8-bit: 0=silence, 128=max negative */
            int sample = ((int)ch->data[idx] - 128) << 7; /* ±16256 */
            sample = sample * ch->volume / 127;

            /* Pan: sep 0=full left, 255=full right, 128=centre */
            int left  = sample * (255 - ch->pan) / 255;
            int right = sample * ch->pan        / 255;

            mixbuf[i * 2]     += (short)left;
            mixbuf[i * 2 + 1] += (short)right;

            pos += ch->step;
        }
        ch->pos = pos;
    }

    /* --- Music --- */
    music_render(mixbuf, MIXBUF);

    /* --- Clip --- */
    for (int i = 0; i < MIXBUF * 2; i++) {
        if (mixbuf[i] >  32767) mixbuf[i] =  32767;
        if (mixbuf[i] < -32768) mixbuf[i] = -32768;
    }

    dg_audio_callback(mixbuf, MIXBUF);
}

/* --- SFX start / stop / query ------------------------------------- */
int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= NCHANNELS || !sfxinfo) return -1;

    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) {
        lumpnum = I_GetSfxLumpNum(sfxinfo);
        sfxinfo->lumpnum = lumpnum;
    }
    if (lumpnum < 0) return -1;

    unsigned char *lump =
        (unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return -1;

    /* Doom SFX lump header:
     *   bytes 0–1: format (0x0003 = PC Speaker format raw)
     *   bytes 2–3: sample rate (LE u16)
     *   bytes 4–7: sample count (LE u32)
     *   bytes 8–23: padding / unused
     *   bytes 24+: sample data (unsigned 8-bit) */
    if (lump[0] != 0x03 || lump[1] != 0x00) return -1;

    unsigned int src_rate = (unsigned int)lump[2] | ((unsigned int)lump[3] << 8);
    if (src_rate == 0) src_rate = 11025;

    int length = (int)(lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24));
    if (length <= 0) return -1;

    unsigned char *samples = lump + 24;

    channel_t *ch = &channels[channel];
    ch->active = 1;
    ch->volume = vol;
    ch->pan    = sep;
    ch->step   = sfx_step_for_rate(src_rate);  /* BUG 2 FIX */
    ch->pos    = 0;
    ch->data   = samples;
    ch->length = length;

    if (sound_start_count < 20) {
        sound_start_count++;
        char b[80]; int p = 0;
        const char *m = "Snd: ";
        while (*m) b[p++] = *m++;
        const char *n = DEH_String(sfxinfo->name);
        for (int i = 0; n[i] && p < 60; i++) b[p++] = n[i];
        b[p++] = ' '; b[p++] = 'r'; b[p++] = 'a'; b[p++] = 't';
        b[p++] = 'e'; b[p++] = '=';
        unsigned int r = src_rate;
        if (r >= 10000) b[p++] = '0' + (r / 10000) % 10;
        if (r >=  1000) b[p++] = '0' + (r /  1000) % 10;
        if (r >=   100) b[p++] = '0' + (r /   100) % 10;
        if (r >=    10) b[p++] = '0' + (r /    10) % 10;
        b[p++] = '0' + r % 10;
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
    channels[channel].pan    = sep;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds; (void)num_sounds;
}

/* ============================================================
 * Music API
 * ============================================================ */
void I_InitMusic(void) {
    init_music_tables();
    ps_sound_log("I_InitMusic: done\n");
}

void I_ShutdownMusic(void) {
    mus_playing = 0;
    voice_all_off();
}

void I_SetMusicVolume(int volume) {
    snd_musicvolume = volume;
}

void I_PauseSong(void) {
    /* Zero all voice envelopes — notes stay allocated */
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        mus_voices[i].env = 0;
}

void I_ResumeSong(void) {
    /* Notes will be re-triggered by the next note-on event */
}

void *I_RegisterSong(void *data, int len) {
    if (!data || len < 16) {
        ps_sound_log("Music: RegisterSong bad data\n");
        return 0;
    }

    song_t *s = (song_t *)malloc(sizeof(song_t));
    if (!s) { ps_sound_log("Music: RegisterSong malloc fail\n"); return 0; }

    s->data = (unsigned char *)malloc((unsigned)len);
    if (!s->data) { ps_sound_log("Music: RegisterSong data malloc fail\n"); return 0; }

    memcpy(s->data, data, len);
    s->len = len;

    /* Validate — must be MUS\x1a */
    if (s->data[0] == 'M' && s->data[1] == 'U' &&
        s->data[2] == 'S' && s->data[3] == 0x1A) {
        ps_sound_log("Music: RegisterSong OK (MUS format)\n");
    } else {
        ps_sound_log("Music: RegisterSong WARN unknown format\n");
    }
    return s;
}

void I_UnRegisterSong(void *handle) {
    (void)handle; /* bump allocator — no free */
}

void I_PlaySong(void *handle, boolean looping) {
    song_t *s = (song_t *)handle;
    if (!s || !s->data || s->len < 16) {
        ps_sound_log("Music: PlaySong NULL\n");
        return;
    }

    unsigned char *d = s->data;

    /* Verify MUS magic */
    if (d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1A) {
        ps_sound_log("Music: PlaySong bad magic\n");
        return;
    }

    unsigned short score_len   = (unsigned short)(d[4]  | (d[5]  << 8));
    unsigned short score_start = (unsigned short)(d[6]  | (d[7]  << 8));

    if (score_start + score_len > (unsigned)s->len) {
        ps_sound_log("Music: PlaySong score out of bounds\n");
        return;
    }

    /* Point directly at score data */
    mus_score     = d + score_start;
    mus_score_len = score_len;
    mus_pos       = 0;
    mus_end       = 0;
    mus_loop      = looping ? 1 : 0;
    mus_sample_acc = 0;
    mus_delay_ticks = 0;

    /* Reset per-channel state */
    for (int i = 0; i < 16; i++) mus_chans[i].volume = 64;
    voice_all_off();

    /* Prime the first delay */
    mus_read_delay();

    mus_playing = 1;
    ps_sound_log(looping ? "Music: playing (loop)\n" : "Music: playing (once)\n");
}

void I_StopSong(void) {
    mus_playing = 0;
    voice_all_off();
}

boolean I_IsSongPlaying(void) {
    return mus_playing ? 1 : 0;
}

boolean I_MusicIsPlaying(void) {
    return mus_playing ? 1 : 0;
}
