/*
 * i_sound_ps.c — Doom sound. V27
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

#define SFX_HEADROOM   2
#define MUSIC_AMPL     6

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
static int      mus_delay_us       = 0;
static int      mus_tick_acc       = 0;

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

    if (dbg_note < 200) {
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
            mus_delay_us = 0;
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
    if (dbg_event <= 100) {
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
        mus_delay_us = (delay * 1000000) / 140;
    } else {
        mus_delay_us = 0;
    }
}

static void mus_parse_header(void) {
    mus_pos = mus_track_start;
    music_all_notes_off();
    mus_delay_us = 0;
    mus_tick_acc = 0;

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
    mus_delay_us    = 0;
    log_str("Music: MUS loaded");
}

static void music_render_accum(s32 *accum, int frames) {
    if (!mus_playing) return;

    int us_per_sample = 1000000 / SAMPLE_RATE;
    if (us_per_sample < 1) us_per_sample = 1;

    int vol = snd_musicvolume;
    if (vol < 0)   vol = 0;
    if (vol > 15)  vol = (vol * 15) / 120;
    if (vol > 15)  vol = 15;

    int music_scale_fp = (MUSIC_AMPL * vol * 65536) / 15;

    for (int i = 0; i < frames; i++) {
        mus_tick_acc += us_per_sample;

        int safety = 0;
        while (mus_playing && mus_tick_acc >= mus_delay_us && safety < 200) {
            safety++;
            mus_tick_acc -= mus_delay_us;
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

        mix = ((long long)mix * music_scale_fp) >> 16;
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
    /* Explicit zero — belt-and-suspenders on top of the BSS wipe, because
     * a session-2 init should always start from a clean slate even if some
     * compiler optimisation or linker-section quirk left stale bytes. */
    for (int i = 0; i < NCHANNELS; i++) {
        channels[i].active    = 0;
        channels[i].releasing = 0;
        channels[i].fade      = 0;
        channels[i].data      = 0;
        channels[i].pos       = 0;
        channels[i].volume    = 0;
        channels[i].pan       = 0;
        channels[i].step      = 0;
        channels[i].length    = 0;
    }
    init_music_tables();
    g_mus_lpf = 0; g_out_lpf_l = 0; g_out_lpf_r = 0;
    dbg_submit = 0;  /* reset counters so session 2 gets fresh heartbeat logs */
    dbg_event  = 0;
    dbg_note   = 0;
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
            sample >>= 1;

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
    g_mus_lpf    = 0;
    mus_data     = 0;
    mus_len      = 0;
    mus_pos      = 0;
    mus_track_start = 0;
    mus_track_end   = 0;
    mus_loop     = 0;
    mus_playing  = 0;
    mus_is_mus   = 0;
    mus_delay_us = 0;
    mus_tick_acc = 0;
    music_all_notes_off();
    log_str("I_InitMusic reached");
    log_str("I_InitMusic done");
}

void I_ShutdownMusic(void) { music_all_notes_off(); mus_playing = 0; }

void I_SetMusicVolume(int volume) {
    snd_musicvolume = volume;
    log_num("M:vol=", volume, "");
}

void I_PauseSong(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        if (mus_notes[i].active) mus_notes[i].env = 0;
}

void I_ResumeSong(void) {}

/* v25: log the first 8 bytes of the incoming song so we can see exactly
 * what Doom is passing when music fails on session 2. */
static void log_song_bytes(const unsigned char *p, int len) {
    char b[128]; int bp = 0;
    const char *hex = "0123456789ABCDEF";
    const char *pre = "Music: len=";
    while (*pre) b[bp++] = *pre++;
    /* decimal len */
    char tmp[16]; int t = 0; int v = len;
    if (v == 0) tmp[t++] = '0';
    else { while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; } }
    while (t) b[bp++] = tmp[--t];
    const char *mid = " bytes=";
    while (*mid) b[bp++] = *mid++;
    int n = len < 8 ? len : 8;
    for (int i = 0; i < n; i++) {
        b[bp++] = hex[(p[i] >> 4) & 0xF];
        b[bp++] = hex[p[i] & 0xF];
        b[bp++] = ' ';
    }
    b[bp++] = '\n'; b[bp] = 0;
    ps_sound_log(b);
}

void *I_RegisterSong(void *data, int len) {
    song_t *s = (song_t *)malloc(sizeof(song_t));
    if (!s) { ps_sound_log("Music: song_t malloc failed"); return 0; }
    s->data = (unsigned char *)malloc((unsigned)len);
    if (!s->data) {
        ps_sound_log("Music: data malloc failed");
        return 0;
    }
    memcpy(s->data, data, len);
    s->len = len;

    log_song_bytes(s->data, len);

    if (len >= 4 && s->data[0] == 'M' && s->data[1] == 'U' &&
        s->data[2] == 'S' && s->data[3] == 0x1A) {
        s->is_mus = 1;
        ps_sound_log("Music: registered MUS");
    } else {
        s->is_mus = -1;
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
