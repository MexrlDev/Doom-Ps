/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v21 — MUS TIMING + SFX LIMITER FIX
 *
 *   MUS timing:
 *     The MUS format encodes a variable-length delay AFTER each event
 *     (high-bit continuation, 140 Hz ticks).  Previous versions used a
 *     fixed 6-tick gap and never read the delay bytes at all, which
 *     flattened all the rhythm and made fast passages sound frantic.
 *     Now the parser reads the delay properly and schedules the next
 *     event at the exact microsecond the file specifies.
 *
 *   SFX limiter:
 *     The old soft_clip divided the excess by 4 above 24000, then by
 *     16 above 30000.  With 3–4 overlapping gunshots, the sum easily
 *     exceeded 24000, and the -12 dB drop sounded like a cut.  The
 *     new limiter compresses the excess by 2:1 (gentle) and only
 *     hard-limits as a final safety net.
 *
 *   Volume:
 *     MUSIC_AMPL 80 → 48 (music lowered by about 4 dB).
 *     SFX_HEADROOM kept at 5 (SFX volume unchanged).
 *
 *   Buffer:
 *     SAMPLES_PER_BUF 512 → 1024 to give the audio thread more time
 *     to mix and submit before the hardware buffer underruns.
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
#define MIXBUF              1024
#define MUSIC_MAX_NOTES     32

#define SFX_HEADROOM   5
#define MUSIC_AMPL     48

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
static int      mus_delay_us       = 0;  /* microseconds until next event */
static int      mus_tick_acc       = 0;  /* accumulator for delay timing */

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

/* ----------------------------------------------------------------
 * MUS event parser.
 *
 * Event byte layout:
 *   bit 7: has-delay flag
 *   bits 6-4: type (0-7)
 *   bits 3-0: channel (0-15)
 *
 * Data bytes per type:
 *   0 = release note   : 1 byte
 *   1 = play note      : 1 byte, plus a second volume byte if bit 7
 *                        of the first data byte is set
 *   2 = pitch bend     : 1 byte
 *   3 = system event   : 1 byte
 *   4 = controller     : 2 bytes
 *   5 = end of measure : 0 bytes
 *   6 = finish         : 0 bytes (end of song data)
 *   7 = unused         : 1 byte
 *
 * After the event data, if the has-delay flag was set, a variable-
 * length delay follows.  Each byte contributes 7 bits; bit 7 marks
 * continuation.  Delay is in 140 Hz ticks (1 tick = 7142.857 µs).
 * ---------------------------------------------------------------- */
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
    case 0: /* Release note */
        if (mus_pos < mus_track_end)
            music_note_off_chan(chan, mus_data[mus_pos++]);
        break;

    case 1: { /* Play note */
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

    case 2: /* Pitch bend */
        if (mus_pos < mus_track_end) mus_pos++;
        break;

    case 3: /* System event */
        if (mus_pos < mus_track_end) mus_pos++;
        break;

    case 4: /* Controller */
        if (mus_pos + 1 < mus_track_end) mus_pos += 2;
        else mus_pos = mus_track_end;
        break;

    case 5: /* End of measure — no data bytes */
        break;

    case 6: /* Finish */
        mus_playing = 0;
        music_all_notes_off();
        return;

    case 7: /* Unused — 1 byte */
        if (mus_pos < mus_track_end) mus_pos++;
        break;

    default:
        mus_pos++;
        break;
    }

    /* Read the variable-length delay. */
    if (has_delay) {
        int delay = 0;
        while (mus_pos < mus_track_end) {
            unsigned char b = mus_data[mus_pos++];
            delay = (delay << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        /* 1 tick = 1/140 s = 7142.857 µs */
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
    log_num("M: score_start=", score_start, "");
    log_num("M: len=", mus_len, "");

    if (score_start < 16 || score_start >= mus_len) {
        log_str("Music: bad MUS score offset");
        mus_playing = 0;
        return;
    }

    mus_track_start = score_start;
    mus_track_end   = mus_len;
    mus_pos         = score_start;

    /* Fire the first event immediately. */
    mus_delay_us = 0;

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

    for (int i = 0; i < frames; i++) {
        mus_tick_acc += us_per_sample;

        /* Fire events whose delay has expired. */
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

        mix = mix * MUSIC_AMPL * vol / 15;
        g_mus_lpf += (mix - g_mus_lpf) / 2;

        accum[i * 2]     += g_mus_lpf;
        accum[i * 2 + 1] += g_mus_lpf;
    }
}

/* ----------------------------------------------------------------
 * Smooth limiter.  Linear below 0.75 FS; above that, excess is
 * compressed by 2:1.  Only pathological sums touch the hard limit.
 * This is what fixes the "cutting" on rapid fire — 3–4 overlapping
 * gunshots no longer trigger a -12 dB step.
 * ---------------------------------------------------------------- */
static inline s16 soft_clip(s32 v) {
    const s32 knee = 24576;  /* 0.75 × 32768 */
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
    g_mus_lpf = 0;
    g_out_lpf_l = 0;
    g_out_lpf_r = 0;
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

    for (int i = 0; i < MIXBUF; i++) {
        int l = mix_accum[i * 2];
        int r = mix_accum[i * 2 + 1];
        g_out_lpf_l += (l - g_out_lpf_l) * 3 / 5;
        g_out_lpf_r += (r - g_out_lpf_r) * 3 / 5;
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
