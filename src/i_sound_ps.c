/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v17:
 *   - CRITICAL: SFX playback rate.  Doom's DMX lumps are 11025 Hz,
 *     but ch->step was 1<<16 = "1 input sample per output sample".
 *     At 48000 Hz output, that's a 4.35x speedup (chipmunk + fast).
 *     Now step = (11025 << 16) / SAMPLE_RATE  →  15052.
 *   - Music volume conversion: Doom sends 0-120 to I_SetMusicVolume
 *     (its menu value 0-15 is pre-multiplied by 8).  Was dividing by
 *     64; now /120 so default 8 stays 8/15 and slider max 120 → 15.
 *   - MUSIC_AMPL 32 → 48 (50% louder).
 *   - Event logging 40 → 100, note logging 30 → 200.
 *   - One-pole LPF, soft clip, T=6 spurious skip retained.
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

#define NCHANNELS           16
#define MIXBUF              512
#define MUSIC_MAX_NOTES     24
#define MUS_TICKS_PER_SEC   140
#define MUS_DEFAULT_TICK_US (1000000 / MUS_TICKS_PER_SEC)

/* SFX headroom: 4 → 4 concurrent full-volume sounds fit under ±32767. */
#define SFX_HEADROOM   4

/* Music: typical 4-voice chord at vel 64 → mix=256.  × 48 × vol/15. */
#define MUSIC_AMPL     48

/* DMX sample rate for Doom SFX lumps (dsXXX).  Output is 48000. */
#define DMX_SAMPLE_RATE 11025

#define FADE_MAX       1024
#define FADE_STEP_IN   6
#define FADE_STEP_OUT  3

static int dbg_submit = 0;
static int dbg_tick   = 0;
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

typedef struct {
    int active, note, channel, releasing;
    unsigned int phase, phase_inc;
    int env, env_target;
} music_note_t;

typedef struct { unsigned char *data; int len; int is_mus; } song_t;

static music_note_t mus_notes[MUSIC_MAX_NOTES];

static unsigned char *mus_data   = 0;
static int      mus_len          = 0;
static int      mus_pos          = 0;
static int      mus_track_start  = 0;
static int      mus_track_end    = 0;
static int      mus_loop         = 0;
static int      mus_playing      = 0;
static int      mus_end_of_track = 0;
static int      mus_is_mus       = 0;
static int      mus_us_per_tick  = MUS_DEFAULT_TICK_US;
static int      mus_sample_acc   = 0;
static unsigned mus_current_tick    = 0;
static unsigned mus_next_event_tick = 0;

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

static int mus_read_varlen(void) {
    int v = 0;
    for (int i = 0; i < 4 && mus_pos < mus_track_end; i++) {
        unsigned char b = mus_data[mus_pos++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

/* 64-byte hex dump.  Buffer must be >= 9 + 64*3 + 2 = 203 bytes. */
static void dump_score_bytes(void) {
    char b[320]; int p = 0;
    const char *pre = "M: hdr64:";
    while (*pre && p < 32) b[p++] = *pre++;
    const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < 64 && (mus_track_start + i) < mus_track_end; i++) {
        if (p >= 315) break;
        b[p++] = ' ';
        unsigned char c = mus_data[mus_track_start + i];
        b[p++] = h[(c >> 4) & 0xF];
        b[p++] = h[c & 0xF];
    }
    b[p++] = '\n'; b[p] = 0;
    ps_sound_log(b);
}

static void mus_process_tick(void) {
    if (!mus_playing) return;

    dbg_tick++;
    if (dbg_tick == 1 || dbg_tick == 1000 || dbg_tick == 5000) {
        log_num("M: tick #", dbg_tick, "");
    }

    if (mus_end_of_track) {
        if (mus_loop) {
            log_str("M: loop restart");
            mus_pos             = mus_track_start;
            mus_current_tick    = 0;
            mus_sample_acc      = 0;
            mus_end_of_track    = 0;
            music_all_notes_off();
            mus_next_event_tick = (unsigned)mus_read_varlen();
        } else {
            log_str("M: END (once)");
            mus_playing = 0;
            music_all_notes_off();
        }
        return;
    }

    while (!mus_end_of_track && mus_current_tick >= mus_next_event_tick) {
        if (mus_pos >= mus_track_end) {
            mus_end_of_track = 1;
            break;
        }

        unsigned char ev = mus_data[mus_pos++];
        int type = (ev >> 4) & 0x07;
        int chan = ev & 0x0F;

        dbg_event++;
        if (dbg_event <= 100) {
            log_num("M: ev #", dbg_event, "");
            log_num("M: ev T=", type, "");
            log_num("M: ev C=", chan, "");
        }

        switch (type) {
        case 0:
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            music_note_off_chan(chan, mus_data[mus_pos++]);
            break;
        case 1:
            if (mus_pos + 1 >= mus_track_end) { mus_end_of_track = 1; break; }
            {
                int note = mus_data[mus_pos++];
                int vel  = mus_data[mus_pos++];
                if (vel == 0) music_note_off_chan(chan, note);
                else          music_note_on(note, vel, chan);
            }
            break;
        case 2:
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            mus_pos++;
            break;
        case 3:
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            mus_pos++;
            break;
        case 4:
            if (mus_pos + 1 >= mus_track_end) { mus_end_of_track = 1; break; }
            mus_pos += 2;
            break;
        case 6:
            if (mus_pos + 100 < mus_track_end) {
                log_str("M: skip spurious T=6");
            } else {
                mus_end_of_track = 1;
            }
            break;
        default:
            mus_end_of_track = 1;
            break;
        }
        if (mus_end_of_track) break;

        if (mus_pos < mus_track_end) {
            int delay = mus_read_varlen();
            mus_next_event_tick = mus_current_tick + (unsigned)delay;
        } else {
            mus_end_of_track = 1;
        }
    }

    mus_current_tick++;
}

static void mus_parse_header(void) {
    mus_end_of_track    = 0;
    mus_current_tick    = 0;
    mus_next_event_tick = 0;
    mus_sample_acc      = 0;
    music_all_notes_off();

    if (!mus_data || mus_len < 16) { mus_end_of_track = 1; return; }
    if (mus_data[0] != 'M' || mus_data[1] != 'U' ||
        mus_data[2] != 'S' || mus_data[3] != 0x1A) {
        log_str("Music: bad MUS magic");
        mus_end_of_track = 1;
        return;
    }

    int score_len   = mus_data[4]  | (mus_data[5]  << 8);
    int score_start = mus_data[6]  | (mus_data[7]  << 8);
    log_num("M: lump_len=", mus_len, "");
    log_num("M: score_len=", score_len, "");
    log_num("M: score_start=", score_start, "");
    log_num("M: prim_ch=", mus_data[8] | (mus_data[9] << 8), "");
    log_num("M: sec_ch=", mus_data[10] | (mus_data[11] << 8), "");
    log_num("M: inst=", mus_data[12] | (mus_data[13] << 8), "");

    if (score_start < 16 || score_start >= mus_len) {
        log_str("Music: bad MUS score offset");
        mus_end_of_track = 1;
        return;
    }

    mus_track_start = score_start;
    mus_track_end   = mus_len;
    mus_pos         = score_start;
    mus_next_event_tick = (unsigned)mus_read_varlen();
    log_num("M: first delay=", (int)mus_next_event_tick, "");

    dump_score_bytes();
    log_str("Music: MUS loaded");
}

static void midi_process_tick(void) { mus_end_of_track = 1; }

static void music_render_accum(s32 *accum, int frames) {
    if (!mus_playing) return;

    int us_per_sample = 1000000 / SAMPLE_RATE;
    if (us_per_sample < 1) us_per_sample = 1;

    int vol = snd_musicvolume;
    if (vol < 0)   vol = 0;
    /* Doom menu value 0-15 is pre-multiplied by 8 before reaching
     * I_SetMusicVolume.  Normalize 0-120 → 0-15. */
    if (vol > 15)  vol = (vol * 15) / 120;
    if (vol > 15)  vol = 15;

    for (int i = 0; i < frames; i++) {
        if (mus_playing) {
            mus_sample_acc += us_per_sample;
            while (mus_sample_acc >= mus_us_per_tick) {
                mus_sample_acc -= mus_us_per_tick;
                if (mus_is_mus) mus_process_tick();
                else            midi_process_tick();
                if (!mus_playing) break;
            }
        }
        if (!mus_playing) break;

        int mix = 0;
        for (int j = 0; j < MUSIC_MAX_NOTES; j++) {
            music_note_t *n = &mus_notes[j];
            if (!n->active) continue;

            if (n->releasing) {
                n->env -= 6;
                if (n->env <= 0) { n->env = 0; n->active = 0; continue; }
            } else if (n->env < n->env_target) {
                n->env += 10;
                if (n->env > n->env_target) n->env = n->env_target;
            }

            int sample = (n->phase & 0x80000000) ? -1 : 1;
            sample *= n->env;
            mix += sample;
            n->phase += n->phase_inc;
        }

        mix = mix * MUSIC_AMPL * vol / 15;
        g_mus_lpf += (mix - g_mus_lpf) / 2;

        accum[i * 2]     += g_mus_lpf;
        accum[i * 2 + 1] += g_mus_lpf;
    }
}

static inline s16 soft_clip(s32 v) {
    if (v > 20000) v = 20000 + (v - 20000) / 4;
    if (v > 26000) v = 26000 + (v - 26000) / 8;
    if (v > 30000) v = 30000 + (v - 30000) / 32;
    if (v > 32767) v = 32767;
    if (v < -20000) v = -20000 + (v + 20000) / 4;
    if (v < -26000) v = -26000 + (v + 26000) / 8;
    if (v < -30000) v = -30000 + (v + 30000) / 32;
    if (v < -32768) v = -32768;
    return (s16)v;
}

void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++)
        channels[i].active = channels[i].releasing = channels[i].fade = 0;
    init_music_tables();
    g_mus_lpf = 0;
    if (snd_musicdevice == 0) snd_musicdevice = 3;
    log_num("I_InitSound: musicvol=", snd_musicvolume, "");
    log_num("I_InitSound: sfxvol=",   snd_sfxvolume,   "");
    log_str("I_InitSound done");
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

        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= length) { ch->active = 0; break; }

            if (releasing) {
                fade -= FADE_STEP_OUT;
                if (fade <= 0) { ch->active = 0; break; }
            } else if (fade < FADE_MAX) {
                fade += FADE_STEP_IN;
                if (fade > FADE_MAX) fade = FADE_MAX;
            }

            int sample = ((int)data[idx] - 128) << 8;
            sample = sample * fade / FADE_MAX;
            sample = sample * vol / 127;
            sample /= SFX_HEADROOM;

            int lgain = 255 - pan, rgain = pan;
            mix_accum[i * 2]     += sample * lgain / 255;
            mix_accum[i * 2 + 1] += sample * rgain / 255;
            pos += step;
        }
        ch->pos  = pos;
        ch->fade = fade;
    }

    music_render_accum(mix_accum, MIXBUF);

    for (int i = 0; i < MIXBUF * 2; i++) {
        mix_final[i] = soft_clip(mix_accum[i]);
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
    /* DMX lumps are 11025 Hz; output is SAMPLE_RATE Hz.  step is 16.16. */
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

    if (len >= 4) {
        char b[40]; int p = 0;
        const char *pre = "Music: fmt=";
        while (*pre) b[p++] = *pre++;
        const char h[] = "0123456789ABCDEF";
        for (int i = 0; i < 4; i++) {
            b[p++] = h[(s->data[i] >> 4) & 0xF];
            b[p++] = h[s->data[i] & 0xF];
            b[p++] = ' ';
        }
        b[p++] = '\n'; b[p] = 0;
        ps_sound_log(b);
    }

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
    mus_data = s->data; mus_len = s->len; mus_loop = looping ? 1 : 0;
    mus_is_mus = s->is_mus;
    if (mus_is_mus == -1) { log_str("Music: unsupported format"); return; }
    if (!mus_is_mus)      { log_str("Music: MIDI not supported"); return; }

    mus_parse_header();
    if (mus_end_of_track) { log_str("Music: parse failed"); return; }

    mus_playing = 1;
    log_str(looping ? "Music: playing (loop)" : "Music: playing once");
}

void I_StopSong(void) { mus_playing = 0; music_all_notes_off(); }
boolean I_IsSongPlaying(void)  { return mus_playing ? 1 : 0; }
boolean I_MusicIsPlaying(void) { return mus_playing ? 1 : 0; }
