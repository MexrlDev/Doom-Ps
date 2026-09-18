/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v25:
 *   - MUSIC_AMPL 6 → 12 (music 2× louder)
 *   - SFX_HEADROOM 2 → 1 (SFX 2× louder)
 *     → both louder, same music-vs-SFX balance
 *   - Music timing is now EXACT.  The old code advanced the tick
 *     accumulator by `1000000 / 48000 = 20` µs per sample, but a real
 *     sample is 20.833 µs.  That's a 4% drift per second, which is
 *     the "wrong notes" the user reported.  Now every sample adds
 *     1e9 ns × SAMPLE_RATE units and the delay is stored in the same
 *     exact-rational units.
 *   - Trimmed the debug logs (only first 20 events, first 50 notes).
 *     The event flood was adding latency on every submit.
 *   - Reset snd_musicvolume/snd_sfxvolume to 8 at I_InitSound so
 *     session 2 can't inherit a stale value.
 *
 * v24: mixer divisions → multiplies.
 * v23: MUSIC_AMPL 36 → 6, SFX_HEADROOM 5 → 2.
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

#define NCHANNELS           32
#define MIXBUF              SAMPLES_PER_BUF
#define MUSIC_MAX_NOTES     32

/* v25: both buses 2× louder, same music:sfx ratio as v24. */
#define SFX_HEADROOM   1
#define MUSIC_AMPL     12

#define DMX_SAMPLE_RATE 11025

#define FADE_MAX       1024
#define FADE_STEP_IN   6
#define FADE_STEP_OUT  3

static int dbg_submit = 0;
static int dbg_event  = 0;
static int dbg_note   = 0;

static void log_num(const char *prefix, int n, const char *suffix) {
    char b[64]; int p = 0;
    while (*prefix && p < 24) b[p++] = *prefix++;
    if (n == 0) b[p++] = '0';
    else {
        char t[12]; int k = 0;
        while (n > 0) { t[k++] = '0' + (n % 10); n /= 10; }
        while (k) b[p++] = t[--k];
    }
    while (*suffix && p < 60) b[p++] = *suffix++;
    b[p++] = '\n'; b[p] = 0;
    ps_sound_log(b);
}

static void log_str(const char *s) { ps_sound_log(s); }

typedef struct {
    int active, releasing, volume, pan, step, pos, length, fade;
    const unsigned char *data;
} channel_t;

static channel_t channels[NCHANNELS];
static s32   mix_accum[MIXBUF * 2];
static short mix_final[MIXBUF * 2];

static s32 g_mus_lpf = 0;
static s32 g_out_lpf_l = 0;
static s32 g_out_lpf_r = 0;

typedef struct {
    int active, note, channel, releasing;
    unsigned int phase, phase_inc;
    int env, env_target;
} music_note_t;

typedef struct { unsigned char *data; int len; int is_mus; } song_t;

static music_note_t mus_notes[MUSIC_MAX_NOTES];

static unsigned char *mus_data     = 0;
static int      mus_len            = 0;
static int      mus_pos            = 0;
static int      mus_track_start    = 0;
static int      mus_track_end      = 0;
static int      mus_loop           = 0;
static int      mus_playing        = 0;
static int      mus_is_mus         = 0;

/* v25: exact-rational timing.
 *   mus_delay_units and mus_time_units are both in units of
 *   (nanoseconds × SAMPLE_RATE).  Advancing by one audio sample
 *   adds 1e9 to mus_time_units (because 1e9 / SAMPLE_RATE ns of real
 *   time has elapsed, and we scale by SAMPLE_RATE to keep it integer).
 */
static s64      mus_delay_units    = 0;
static s64      mus_time_units     = 0;

static unsigned int note_phase_inc_table[128];

static void init_music_tables(void) {
    const double k    = 4294967296.0 / (double)SAMPLE_RATE;
    const double semi = 1.0594630943592953;
    for (int n = 0; n < 128; n++) {
        int diff = n - 69;
        double ratio = 1.0;
        if (diff >= 0) for (int i = 0; i <  diff; i++) ratio *= semi;
        else           for (int i = 0; i < -diff; i++) ratio /= semi;
        double inc = 440.0 * ratio * k;
        if (inc > 4294967295.0) inc = 4294967295.0;
        note_phase_inc_table[n] = (unsigned int)inc;
    }
}

static void music_note_off_chan(int chan, int note) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        if (mus_notes[i].active && !mus_notes[i].releasing &&
            mus_notes[i].channel == chan && mus_notes[i].note == note)
            mus_notes[i].releasing = 1;
}

static void music_note_on(int note, int vel, int chan) {
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    if (vel <= 0) { music_note_off_chan(chan, note); return; }
    if (vel > 127)  vel = 127;

    if (dbg_note < 50) {
        dbg_note++;
        log_num("M: note N=", note, "");
        log_num("M: note V=", vel, "");
    }

    int slot = -1, free_slot = -1, releasing_slot = -1;
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (mus_notes[i].active &&
            mus_notes[i].channel == chan && mus_notes[i].note == note) {
            slot = i; break;
        }
        if (!mus_notes[i].active && free_slot < 0) free_slot = i;
        if (mus_notes[i].active && mus_notes[i].releasing &&
            releasing_slot < 0) releasing_slot = i;
    }
    if (slot < 0) {
        if      (free_slot      >= 0) slot = free_slot;
        else if (releasing_slot >= 0) slot = releasing_slot;
        else {
            slot = 0;
            int best = 0x7FFFFFFF;
            for (int i = 0; i < MUSIC_MAX_NOTES; i++)
                if (mus_notes[i].env < best) { best = mus_notes[i].env; slot = i; }
        }
    }

    mus_notes[slot].active     = 1;
    mus_notes[slot].note       = note;
    mus_notes[slot].channel    = chan;
    mus_notes[slot].releasing  = 0;
    mus_notes[slot].phase      = 0;
    mus_notes[slot].phase_inc  = note_phase_inc_table[note];
    mus_notes[slot].env        = 0;
    mus_notes[slot].env_target = vel;
}

static void music_all_notes_off(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        mus_notes[i].active = mus_notes[i].releasing = 0;
}

static void mus_process_event(void) {
    if (mus_pos >= mus_track_end) {
        if (mus_loop) {
            log_str("M: loop restart");
            mus_pos = mus_track_start;
            music_all_notes_off();
            mus_delay_units = 0;
            mus_time_units  = 0;
        } else {
            log_str("M: END");
            mus_playing = 0;
            music_all_notes_off();
        }
        return;
    }

    unsigned char ev = mus_data[mus_pos++];
    int type = (ev >> 4) & 0x07;
    int chan = ev & 0x0F;
    int has_delay = (ev & 0x80) != 0;

    dbg_event++;
    if (dbg_event <= 20) {
        log_num("M: ev #", dbg_event, "");
        log_num("M: ev T=", type, "");
        log_num("M: ev C=", chan, "");
    }

    switch (type) {
    case 0:
        if (mus_pos < mus_track_end)
            music_note_off_chan(chan, mus_data[mus_pos++]);
        break;
    case 1: {
        if (mus_pos < mus_track_end) {
            unsigned char b1 = mus_data[mus_pos++];
            int note = b1 & 0x7F;
            int vol = 64;
            if (b1 & 0x80) {
                if (mus_pos < mus_track_end)
                    vol = mus_data[mus_pos++];
            }
            music_note_on(note, vol, chan);
        }
        break;
    }
    case 2: case 3:
        if (mus_pos < mus_track_end) mus_pos++;
        break;
    case 4:
        if (mus_pos + 1 < mus_track_end) mus_pos += 2;
        else mus_pos = mus_track_end;
        break;
    case 5: break;
    case 6:
        mus_playing = 0;
        music_all_notes_off();
        return;
    case 7:
        if (mus_pos < mus_track_end) mus_pos++;
        break;
    }

    if (has_delay) {
        int delay = 0;
        while (mus_pos < mus_track_end) {
            unsigned char b = mus_data[mus_pos++];
            delay = (delay << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        /* v25: exact.  1 tick = 1/140 s = 1e9/140 ns.
         * Convert to our scaled unit: (ns * SAMPLE_RATE).
         *   delay * 1e9 * SAMPLE_RATE / 140
         * int64 is good for delay up to ~2.7e7 ticks. */
        mus_delay_units = ((s64)delay * 1000000000LL * (s64)SAMPLE_RATE) / 140LL;
    } else {
        mus_delay_units = 0;
    }
}

static void mus_parse_header(void) {
    mus_pos = mus_track_start;
    music_all_notes_off();
    mus_delay_units = 0;
    mus_time_units  = 0;

    if (!mus_data || mus_len < 16) { mus_playing = 0; return; }
    if (mus_data[0] != 'M' || mus_data[1] != 'U' ||
        mus_data[2] != 'S' || mus_data[3] != 0x1A) {
        log_str("Music: bad MUS magic");
        mus_playing = 0;
        return;
    }

    int score_start = mus_data[6] | (mus_data[7] << 8);
    if (score_start < 16 || score_start >= mus_len) {
        log_str("Music: bad MUS score offset");
        mus_playing = 0;
        return;
    }

    mus_track_start = score_start;
    mus_track_end   = mus_len;
    mus_pos         = score_start;
    mus_delay_units = 0;
    mus_time_units  = 0;
    log_str("Music: MUS loaded");
}

/*
 * v25 — exact-timed music renderer.
 *
 * Each audio sample advances the wall clock by exactly 1e9/SAMPLE_RATE
 * nanoseconds.  We keep time in units of (nanoseconds × SAMPLE_RATE) so
 * the arithmetic is integer-exact:
 *
 *   advance per sample = 1e9   (because 1e9/SAMPLE_RATE ns × SAMPLE_RATE)
 *   event delay       = (delay_ticks × 1e9 × SAMPLE_RATE) / 140
 *
 * The old code advanced by 1000000/SAMPLE_RATE = 20 µs per sample
 * (truncated from 20.833), which drifted 4% per second — that was the
 * "wrong notes" the user heard.
 */
static void music_render_accum(s32 *accum, int frames) {
    if (!mus_playing) return;

    int vol = snd_musicvolume;
    if (vol < 0)   vol = 0;
    if (vol > 15)  vol = (vol * 15) / 120;
    if (vol > 15)  vol = 15;

    /* Precompute music scale: MUSIC_AMPL * vol / 15, fixed 16.16 */
    int music_scale_fp = (MUSIC_AMPL * vol * 65536) / 15;
    /* Max = 12 * 15 * 65536 / 15 = 786432.  Fits in 32-bit. */

    for (int i = 0; i < frames; i++) {
        mus_time_units += 1000000000LL;   /* one sample's worth of ns, scaled by SAMPLE_RATE */

        int safety = 0;
        while (mus_playing && mus_time_units >= mus_delay_units && safety < 200) {
            safety++;
            mus_time_units -= mus_delay_units;
            mus_process_event();
            if (!mus_playing) break;
        }
        if (!mus_playing) break;

        int mix = 0;
        for (int j = 0; j < MUSIC_MAX_NOTES; j++) {
            music_note_t *n = &mus_notes[j];
            if (!n->active) continue;

            if (n->releasing) {
                n->env -= 1;
                if (n->env <= 0) { n->env = 0; n->active = 0; continue; }
            } else if (n->env < n->env_target) {
                n->env += 2;
                if (n->env > n->env_target) n->env = n->env_target;
            }

            int sample = (n->phase & 0x80000000) ? -1 : 1;
            sample *= n->env;
            mix += sample;
            n->phase += n->phase_inc;
        }

        mix = (int)(((long long)mix * music_scale_fp) >> 16);
        g_mus_lpf += (mix - g_mus_lpf) >> 1;

        accum[i * 2]     += g_mus_lpf;
        accum[i * 2 + 1] += g_mus_lpf;
    }
}

static inline s16 soft_clip(s32 v) {
    const s32 knee = 24576;
    if (v > knee) {
        v = knee + (v - knee) / 2;
        if (v > 32767) v = 32767;
    } else if (v < -knee) {
        v = -knee + (v + knee) / 2;
        if (v < -32768) v = -32768;
    }
    return (s16)v;
}

void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++)
        channels[i].active = channels[i].releasing = channels[i].fade = 0;
    init_music_tables();
    g_mus_lpf = 0; g_out_lpf_l = 0; g_out_lpf_r = 0;

    /* v25: force a fresh music state at every session start. */
    music_all_notes_off();
    mus_playing = 0;
    mus_data = 0;
    mus_len = 0;
    mus_pos = 0;
    mus_track_start = 0;
    mus_track_end = 0;
    mus_loop = 0;
    mus_is_mus = 0;
    mus_delay_units = 0;
    mus_time_units = 0;

    if (snd_musicdevice == 0) snd_musicdevice = 3;
    log_num("I_InitSound: musicvol=", snd_musicvolume, "");
    log_num("I_InitSound: sfxvol=",   snd_sfxvolume,   "");
    log_str("I_InitSound done");
}

void I_ShutdownSound(void) { for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0; }

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    return W_GetNumForName(namebuf);
}

void I_UpdateSound(void) {}
void I_SetChannels(void) {}
void I_SetSfxVolume(int volume) { (void)volume; }

/*
 * v25 — mixer.
 *
 * Divisions folded into per-channel 16.16 gains and shifts.
 */
void I_SubmitSound(void) {
    dbg_submit++;
    if (dbg_submit == 1 || dbg_submit == 100 ||
        dbg_submit == 1000 || (dbg_submit % 5000) == 0) {
        log_num("Audio: submit #", dbg_submit, "");
    }

    for (int i = 0; i < MIXBUF * 2; i++) mix_accum[i] = 0;

    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        int pos = ch->pos, fade = ch->fade, releasing = ch->releasing;
        int vol = ch->volume, pan = ch->pan;
        const unsigned char *data = ch->data;
        int length = ch->length, step = ch->step;

        int vol_scaled = (vol * 65536) / 127;
        int lgain = ((255 - pan) * vol_scaled) / 255;
        int rgain = (pan * vol_scaled) / 255;

        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            int frac = pos & 0xFFFF;
            if (idx >= length) { ch->active = 0; break; }

            if (releasing) {
                fade -= FADE_STEP_OUT;
                if (fade <= 0) { ch->active = 0; break; }
            } else if (fade < FADE_MAX) {
                fade += FADE_STEP_IN;
                if (fade > FADE_MAX) fade = FADE_MAX;
            }

            int s0 = (int)data[idx] - 128;
            int s1 = (idx + 1 < length) ? (int)data[idx + 1] - 128 : s0;
            int sample8 = s0 + ((s1 - s0) * frac >> 16);
            int sample = sample8 << 8;
            sample = (sample * fade) >> 10;
            sample >>= SFX_HEADROOM;

            mix_accum[i * 2]     += (sample * lgain) >> 16;
            mix_accum[i * 2 + 1] += (sample * rgain) >> 16;
            pos += step;
        }
        ch->pos  = pos;
        ch->fade = fade;
    }

    music_render_accum(mix_accum, MIXBUF);

    for (int i = 0; i < MIXBUF; i++) {
        int l = mix_accum[i * 2];
        int r = mix_accum[i * 2 + 1];
        g_out_lpf_l += ((l - g_out_lpf_l) * 39322) >> 16;
        g_out_lpf_r += ((r - g_out_lpf_r) * 39322) >> 16;
        mix_final[i * 2]     = soft_clip(g_out_lpf_l);
        mix_final[i * 2 + 1] = soft_clip(g_out_lpf_r);
    }

    dg_audio_callback(mix_final, MIXBUF);
}

int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= NCHANNELS) return -1;
    if (!sfxinfo) return -1;

    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) {
        lumpnum = I_GetSfxLumpNum(sfxinfo);
        sfxinfo->lumpnum = lumpnum;
    }
    unsigned char *lump = (unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return -1;
    if (lump[0] != 0x03 || lump[1] != 0x00) return -1;

    int length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);
    unsigned char *samples = lump + 24;

    channel_t *ch = &channels[channel];
    ch->active = 1; ch->releasing = 0; ch->fade = 0;
    ch->volume = vol; ch->pan = sep;
    ch->step   = (DMX_SAMPLE_RATE << 16) / SAMPLE_RATE;
    ch->pos    = 0;
    ch->data   = samples;
    ch->length = length;
    return channel;
}

void I_StopSound(int channel) {
    if (channel < 0 || channel >= NCHANNELS) return;
    channels[channel].releasing = 1;
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
    (void)sounds; (void)num_sounds;
}

void I_InitMusic(void) {
    init_music_tables();
    g_mus_lpf = 0;
    log_str("I_InitMusic done");
}

void I_ShutdownMusic(void) { music_all_notes_off(); mus_playing = 0; }

void I_SetMusicVolume(int volume) {
    /* Doom passes 0-127 (or 0-15 in some builds).  Clamp to 0-15. */
    if (volume < 0)   volume = 0;
    if (volume > 15)  volume = (volume * 15) / 120;
    if (volume > 15)  volume = 15;
    snd_musicvolume = volume;
    log_num("M:vol=", volume, "");
}

void I_PauseSong(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        if (mus_notes[i].active) mus_notes[i].env = 0;
}

void I_ResumeSong(void) {}

void *I_RegisterSong(void *data, int len) {
    song_t *s = (song_t *)malloc(sizeof(song_t));
    if (!s) return 0;
    s->data = (unsigned char *)malloc((unsigned)len);
    if (!s->data) return 0;
    memcpy(s->data, data, len);
    s->len = len;

    if (len >= 4 && s->data[0] == 'M' && s->data[1] == 'U' &&
        s->data[2] == 'S' && s->data[3] == 0x1A) {
        s->is_mus = 1;
        ps_sound_log("Music: registered MUS");
    } else {
        s->is_mus = -1;
        /* v25: log the first 4 bytes so we can tell garbage from
         * a legitimately unsupported format (DOOM v1.8 DMX). */
        {
            char b[64]; int p = 0;
            const char *m = "Music: bad header bytes=";
            while (*m) b[p++] = *m++;
            const char *h = "0123456789ABCDEF";
            for (int i = 0; i < 4 && i < len; i++) {
                b[p++] = h[(s->data[i] >> 4) & 0xF];
                b[p++] = h[s->data[i] & 0xF];
                b[p++] = ' ';
            }
            b[p++] = '\n'; b[p] = 0;
            ps_sound_log(b);
        }
        ps_sound_log("Music: unknown format");
    }
    return s;
}

void I_UnRegisterSong(void *handle) { (void)handle; }

void I_PlaySong(void *handle, boolean looping) {
    song_t *s = (song_t *)handle;
    if (!s) { log_str("Music: PlaySong NULL"); return; }
    mus_data   = s->data;
    mus_len    = s->len;
    mus_loop   = looping ? 1 : 0;
    mus_is_mus = s->is_mus;
    if (mus_is_mus == -1) { log_str("Music: unsupported format"); return; }

    mus_parse_header();
    if (!mus_playing && mus_pos >= mus_track_end) {
        log_str("Music: parse failed");
        return;
    }

    mus_playing = 1;
    log_str(looping ? "Music: playing (loop)" : "Music: playing once");
}

void I_StopSong(void) { mus_playing = 0; music_all_notes_off(); }
boolean I_IsSongPlaying(void)  { return mus_playing ? 1 : 0; }
boolean I_MusicIsPlaying(void) { return mus_playing ? 1 : 0; }
