/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v8:
 *   - SFX mixed in an int32 accumulator (was int16 per-sample,
 *     clipped badly with 3+ overlapping sounds → the "cutting"
 *     heard on rapid fire).
 *   - Soft-clip limiter on the final mix: loud passages compress
 *     smoothly instead of hard-clipping.
 *   - Per-channel fade envelope on SFX (256-sample attack + release,
 *     ~5 ms @ 48 kHz) so retriggered channels don't click.
 *   - MUS format support (Doom WAD lumps are MUS, not MIDI).
 *   - Diagnostic: log first 4 bytes of every registered song so we
 *     can prove the format is being detected.
 *
 * If your UDP log still shows "Music: registered song" after this
 * build, the linker did NOT pick up this file — run
 *     make clean && make
 * and re-deploy doom_ps.bin.
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
 * Globals — required for linkage
 * ============================================================ */
int snd_musicdevice = 0;
int snd_sfxdevice   = 0;
int snd_musicvolume = 8;   /* 0..15 */
int snd_sfxvolume   = 8;

#define NCHANNELS          16
#define MIXBUF             512
#define MUSIC_MAX_NOTES    24
#define MUS_TICKS_PER_SEC  140
#define MUS_DEFAULT_TICK_US (1000000 / MUS_TICKS_PER_SEC)   /* ~7143 us */

#define FADE_MAX           256      /* 256 samples @ 48 kHz ≈ 5.3 ms */
#define FADE_STEP          2

/* ============================================================
 * SFX channel
 * ============================================================ */
typedef struct {
    int active;
    int releasing;      /* 1 = fading out, will stop */
    int volume;         /* 0..127 */
    int pan;            /* 0..255, 128 = center */
    int step;           /* 16.16 sample-position increment */
    int pos;            /* 16.16 sample position */
    const unsigned char *data;
    int length;
    int fade;           /* 0..FADE_MAX amplitude envelope */
} channel_t;

static channel_t channels[NCHANNELS];

/* Int32 mix accumulator. All SFX + music summed here, then soft-clipped
 * into a short buffer at the end. This is the key fix for the clipping. */
static s32 mix_accum[MIXBUF * 2];
static short mix_final[MIXBUF * 2];

/* ============================================================
 * Music voice
 * ============================================================ */
typedef struct {
    int active;
    int note;               /* 0..127 */
    int channel;            /* 0..15 */
    int releasing;          /* 1 = fade out */
    unsigned int phase;     /* 32-bit, MSB = square state */
    unsigned int phase_inc;
    int env;                /* current amplitude 0..127 */
    int env_target;
} music_note_t;

typedef struct {
    unsigned char *data;
    int len;
    int is_mus;             /* 1 = MUS, 0 = MIDI, -1 = unknown */
} song_t;

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
static unsigned char mus_running_status = 0;
static unsigned char mus_channel_vol[16];
static unsigned char mus_channel_prog[16];

static unsigned int note_phase_inc_table[128];

/* 32-bit phase increment: freq * 2^32 / SAMPLE_RATE */
static void init_music_tables(void) {
    const double k    = 4294967296.0 / (double)SAMPLE_RATE;
    const double semi = 1.0594630943592953;
    for (int n = 0; n < 128; n++) {
        int diff = n - 69;
        double ratio = 1.0;
        if (diff >= 0) for (int i = 0; i <  diff; i++) ratio *= semi;
        else           for (int i = 0; i < -diff; i++) ratio /= semi;
        double freq = 440.0 * ratio;
        double inc  = freq * k;
        if (inc > 4294967295.0) inc = 4294967295.0;
        note_phase_inc_table[n] = (unsigned int)inc;
    }
}

/* ============================================================
 * Music voice allocation
 * ============================================================ */
static void music_note_off_chan(int chan, int note) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (mus_notes[i].active && !mus_notes[i].releasing &&
            mus_notes[i].channel == chan && mus_notes[i].note == note) {
            mus_notes[i].releasing = 1;
        }
    }
}

static void music_note_on(int note, int vel, int chan) {
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    if (vel <= 0) { music_note_off_chan(chan, note); return; }
    if (vel > 127)  vel = 127;

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
            /* steal the quietest voice */
            slot = 0;
            int best = 0x7FFFFFFF;
            for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
                if (mus_notes[i].env < best) {
                    best = mus_notes[i].env;
                    slot = i;
                }
            }
        }
    }

    mus_notes[slot].active     = 1;
    mus_notes[slot].note       = note;
    mus_notes[slot].channel    = chan;
    mus_notes[slot].releasing  = 0;
    mus_notes[slot].phase      = 0;
    mus_notes[slot].phase_inc  = note_phase_inc_table[note];
    mus_notes[slot].env        = 0;       /* fade-in from silence */
    mus_notes[slot].env_target = vel;
}

static void music_all_notes_off(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        mus_notes[i].active    = 0;
        mus_notes[i].releasing = 0;
    }
}

/* ============================================================
 * MUS parser
 * ============================================================ */
static int mus_read_varlen(void) {
    int v = 0;
    for (int i = 0; i < 4 && mus_pos < mus_track_end; i++) {
        unsigned char b = mus_data[mus_pos++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void mus_process_tick(void) {
    if (!mus_playing) return;

    if (mus_end_of_track) {
        if (mus_loop) {
            mus_pos             = mus_track_start;
            mus_current_tick    = 0;
            mus_sample_acc      = 0;
            mus_end_of_track    = 0;
            mus_running_status  = 0;
            music_all_notes_off();
            mus_next_event_tick = (unsigned)mus_read_varlen();
        } else {
            mus_playing = 0;
            music_all_notes_off();
        }
        return;
    }

    while (!mus_end_of_track && mus_current_tick >= mus_next_event_tick) {
        if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }

        unsigned char ev = mus_data[mus_pos++];
        int type = (ev >> 4) & 0x0F;
        int chan = ev & 0x0F;

        switch (type) {
        case 0:                         /* Release note */
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            music_note_off_chan(chan, mus_data[mus_pos++]);
            break;

        case 1: {                       /* Play note */
            if (mus_pos + 1 >= mus_track_end) {
                mus_end_of_track = 1; break;
            }
            int note = mus_data[mus_pos++];
            int vel  = mus_data[mus_pos++];
            if (vel == 0) music_note_off_chan(chan, note);
            else          music_note_on(note, vel, chan);
            break;
        }

        case 2:                         /* Pitch bend — ignored */
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            mus_pos++;
            break;

        case 3:                         /* System event — ignored */
            if (mus_pos >= mus_track_end) { mus_end_of_track = 1; break; }
            mus_pos++;
            break;

        case 4: {                       /* Controller change */
            if (mus_pos + 1 >= mus_track_end) {
                mus_end_of_track = 1; break;
            }
            int ctrl = mus_data[mus_pos++];
            int val  = mus_data[mus_pos++];
            if      (ctrl == 0) mus_channel_prog[chan] = (unsigned char)val;
            else if (ctrl == 3) mus_channel_vol[chan]  = (unsigned char)val;
            break;
        }

        case 6:                         /* Score end */
            mus_end_of_track = 1;
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
    mus_running_status  = 0;
    mus_sample_acc      = 0;
    music_all_notes_off();

    if (!mus_data || mus_len < 16) { mus_end_of_track = 1; return; }
    if (mus_data[0] != 'M' || mus_data[1] != 'U' ||
        mus_data[2] != 'S' || mus_data[3] != 0x1A) {
        ps_sound_log("Music: bad MUS magic");
        mus_end_of_track = 1;
        return;
    }

    int score_start = mus_data[6] | (mus_data[7] << 8);
    if (score_start < 16 || score_start >= mus_len) {
        ps_sound_log("Music: bad MUS score offset");
        mus_end_of_track = 1;
        return;
    }

    mus_track_start = score_start;
    mus_track_end   = mus_len;
    mus_pos         = score_start;
    mus_us_per_tick = MUS_DEFAULT_TICK_US;

    for (int i = 0; i < 16; i++) {
        mus_channel_vol[i]  = 127;
        mus_channel_prog[i] = 0;
    }

    mus_next_event_tick = (unsigned)mus_read_varlen();
    ps_sound_log("Music: MUS loaded");
}

/* ============================================================
 * MIDI fallback (custom WADs that ship real .mid data)
 * ============================================================ */
static int midi_read_varlen(void) {
    int v = 0;
    for (int i = 0; i < 4 && mus_pos < mus_track_end; i++) {
        unsigned char b = mus_data[mus_pos++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void midi_execute_event(void) {
    if (mus_pos >= mus_track_end) { mus_end_of_track = 1; return; }

    unsigned char status = mus_data[mus_pos];
    if (status < 0x80) {
        status = mus_running_status;
    } else {
        mus_pos++;
        mus_running_status = status;
    }
    unsigned char type = status & 0xF0;
    int chan = status & 0x0F;

    switch (type) {
    case 0x80:
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        music_note_off_chan(chan, mus_data[mus_pos]);
        mus_pos += 2;
        break;
    case 0x90: {
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        int note = mus_data[mus_pos];
        int vel  = mus_data[mus_pos + 1];
        mus_pos += 2;
        if (vel == 0) music_note_off_chan(chan, note);
        else          music_note_on(note, vel, chan);
        break;
    }
    case 0xA0: case 0xB0: case 0xE0:
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        mus_pos += 2;
        break;
    case 0xC0: case 0xD0:
        if (mus_pos + 1 > mus_track_end) { mus_end_of_track = 1; return; }
        mus_pos += 1;
        break;
    case 0xF0:
        if (status == 0xFF) {
            if (mus_pos + 1 > mus_track_end) { mus_end_of_track = 1; return; }
            unsigned char meta = mus_data[mus_pos++];
            int mlen = midi_read_varlen();
            if (meta == 0x2F) { mus_end_of_track = 1; return; }
            mus_pos += mlen;
            if (mus_pos > mus_track_end) mus_pos = mus_track_end;
        } else if (status == 0xF0 || status == 0xF7) {
            int len = midi_read_varlen();
            mus_pos += len;
            if (mus_pos > mus_track_end) mus_pos = mus_track_end;
        }
        break;
    default:
        mus_end_of_track = 1;
        break;
    }
}

static void midi_process_tick(void) {
    if (!mus_playing) return;

    if (mus_end_of_track) {
        if (mus_loop) {
            mus_pos             = mus_track_start;
            mus_current_tick    = 0;
            mus_sample_acc      = 0;
            mus_end_of_track    = 0;
            mus_running_status  = 0;
            music_all_notes_off();
            mus_next_event_tick = (unsigned)midi_read_varlen();
        } else {
            mus_playing = 0;
            music_all_notes_off();
        }
        return;
    }

    while (!mus_end_of_track && mus_current_tick >= mus_next_event_tick) {
        midi_execute_event();
        if (mus_end_of_track) break;
        if (mus_pos < mus_track_end) {
            int delay = midi_read_varlen();
            mus_next_event_tick = mus_current_tick + (unsigned)delay;
        } else {
            mus_end_of_track = 1;
        }
    }
    mus_current_tick++;
}

static void midi_parse_header(void) {
    mus_end_of_track    = 0;
    mus_current_tick    = 0;
    mus_next_event_tick = 0;
    mus_running_status  = 0;
    mus_sample_acc      = 0;
    music_all_notes_off();

    if (!mus_data || mus_len < 14) { mus_end_of_track = 1; return; }
    if (mus_data[0] != 'M' || mus_data[1] != 'T' ||
        mus_data[2] != 'h' || mus_data[3] != 'd') {
        ps_sound_log("Music: bad MIDI magic");
        mus_end_of_track = 1;
        return;
    }

    int ppqn = (mus_data[12] << 8) | mus_data[13];
    if (ppqn == 0 || ppqn > 0x7FFF) ppqn = 96;
    mus_us_per_tick = 500000 / ppqn;

    int hdr_len = (mus_data[4] << 24) | (mus_data[5] << 16)
                | (mus_data[6] << 8)  |  mus_data[7];
    int p = 8 + hdr_len;
    while (p + 8 <= mus_len) {
        if (mus_data[p] == 'M' && mus_data[p+1] == 'T' &&
            mus_data[p+2] == 'r' && mus_data[p+3] == 'k') {
            int tlen = (mus_data[p+4] << 24) | (mus_data[p+5] << 16)
                     | (mus_data[p+6] << 8)  |  mus_data[p+7];
            mus_track_start = p + 8;
            mus_track_end   = mus_track_start + tlen;
            if (mus_track_end > mus_len) mus_track_end = mus_len;
            mus_pos = mus_track_start;
            mus_next_event_tick = (unsigned)midi_read_varlen();
            ps_sound_log("Music: MIDI loaded");
            return;
        }
        int clen = (mus_data[p+4] << 24) | (mus_data[p+5] << 16)
                 | (mus_data[p+6] << 8)  |  mus_data[p+7];
        p += 8 + clen;
    }
    ps_sound_log("Music: no MTrk");
    mus_end_of_track = 1;
}

/* ============================================================
 * Music render — adds into the int32 accumulator
 * ============================================================ */
static void music_render_accum(s32 *accum, int frames) {
    if (!mus_playing) return;

    int us_per_sample = 1000000 / SAMPLE_RATE;
    if (us_per_sample < 1) us_per_sample = 1;

    int vol = snd_musicvolume;
    if (vol < 0)  vol = 0;
    if (vol > 15) vol = 15;

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
                n->env -= 4;
                if (n->env <= 0) {
                    n->env = 0;
                    n->active = 0;
                    continue;
                }
            } else if (n->env < n->env_target) {
                n->env += 8;
                if (n->env > n->env_target) n->env = n->env_target;
            }

            int sample = (n->phase & 0x80000000) ? -1 : 1;
            sample *= n->env;
            mix += sample;
            n->phase += n->phase_inc;
        }

        /* 24 voices × 127 = 3048 peak; scale by 4 → up to 12192 per
         * sample.  SFX then add on top; both go into int32 and the
         * soft-clip at the end handles overflow. */
        mix = mix * 4 * vol / 15;
        accum[i * 2]     += mix;
        accum[i * 2 + 1] += mix;
    }
}

/* ============================================================
 * Doom SFX API
 * ============================================================ */
void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++) {
        channels[i].active    = 0;
        channels[i].releasing = 0;
        channels[i].fade      = 0;
    }
    init_music_tables();
    if (snd_musicdevice == 0) snd_musicdevice = 3;   /* General MIDI */
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

/* ------------------------------------------------------------
 * The main mixing function.
 *
 * All SFX are summed into an int32 accumulator with per-channel
 * fade envelopes.  Music is then added on top.  Finally a
 * soft-clip limiter compresses loud passages instead of hard-
 * clipping them.  This is the fix for the "cutting" on rapid fire.
 * ------------------------------------------------------------ */
void I_SubmitSound(void) {
    for (int i = 0; i < MIXBUF * 2; i++) mix_accum[i] = 0;

    /* ---- SFX ---- */
    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        int pos       = ch->pos;
        int fade      = ch->fade;
        int releasing = ch->releasing;
        int vol       = ch->volume;
        int pan       = ch->pan;
        const unsigned char *data = ch->data;
        int length    = ch->length;
        int step      = ch->step;

        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= length) { ch->active = 0; break; }

            /* Envelope: fade up on attack, fade down on release.
             * A 256-sample window is ~5 ms — short enough to feel
             * instant, long enough to kill clicks. */
            if (releasing) {
                fade -= FADE_STEP;
                if (fade <= 0) {
                    ch->active = 0;
                    break;
                }
            } else if (fade < FADE_MAX) {
                fade += FADE_STEP;
                if (fade > FADE_MAX) fade = FADE_MAX;
            }

            int sample = ((int)data[idx] - 128) << 8;   /* ±32768 */
            sample = sample * fade / FADE_MAX;
            sample = sample * vol / 127;

            int lgain = 255 - pan;
            int rgain = pan;
            mix_accum[i * 2]     += sample * lgain / 255;
            mix_accum[i * 2 + 1] += sample * rgain / 255;

            pos += step;
        }
        ch->pos  = pos;
        ch->fade = fade;
    }

    /* ---- Music ---- */
    music_render_accum(mix_accum, MIXBUF);

    /* ---- Soft-clip limiter ----
     * Below 3/4 full-scale: linear passthrough (single sounds stay
     * loud and clean).  Above: 2:1 compression.  Anything still over
     * gets hard-clipped as a last resort. */
    for (int i = 0; i < MIXBUF * 2; i++) {
        int v = mix_accum[i];
        if (v > 24576) {
            v = 24576 + (v - 24576) / 2;
            if (v > 32767) v = 32767;
        } else if (v < -24576) {
            v = -24576 + (v + 24576) / 2;
            if (v < -32768) v = -32768;
        }
        mix_final[i] = (short)v;
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

    unsigned char *lump =
        (unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return -1;
    if (lump[0] != 0x03 || lump[1] != 0x00) return -1;

    int length = lump[4] | (lump[5] << 8) | (lump[6] << 16) | (lump[7] << 24);
    unsigned char *samples = lump + 24;

    channel_t *ch = &channels[channel];
    ch->active    = 1;
    ch->releasing = 0;
    ch->fade      = 0;              /* ramps up, hides retrigger click */
    ch->volume    = vol;
    ch->pan       = sep;
    ch->step      = 1 << 16;
    ch->pos       = 0;
    ch->data      = samples;
    ch->length    = length;
    return channel;
}

void I_StopSound(int channel) {
    if (channel < 0 || channel >= NCHANNELS) return;
    /* Do NOT zero `active` — let the mixer fade it out so there's no
     * click when Doom reuses the channel for the next gunshot. */
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
    channels[channel].pan    = sep;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

/* ============================================================
 * Music API
 * ============================================================ */
void I_InitMusic(void) {
    init_music_tables();
    ps_sound_log("I_InitMusic done");
}

void I_ShutdownMusic(void) {
    music_all_notes_off();
    mus_playing = 0;
}

void I_SetMusicVolume(int volume) {
    snd_musicvolume = volume;
}

void I_PauseSong(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (mus_notes[i].active) mus_notes[i].env = 0;
    }
}

void I_ResumeSong(void) {}

void *I_RegisterSong(void *data, int len) {
    song_t *s = (song_t *)malloc(sizeof(song_t));
    if (!s) return 0;
    s->data = (unsigned char *)malloc((unsigned)len);
    if (!s->data) return 0;
    memcpy(s->data, data, len);
    s->len = len;

    /* Diagnostic: log the first 4 magic bytes as hex.  If your log
     * shows "Music: fmt=4D 55 53 1A" then you're on this build and the
     * lump is MUS.  If it shows "Music: registered song" (no fmt line),
     * the linker did not pick up this file — `make clean && make`. */
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
    } else if (len >= 4 && s->data[0] == 'M' && s->data[1] == 'T' &&
               s->data[2] == 'h' && s->data[3] == 'd') {
        s->is_mus = 0;
        ps_sound_log("Music: registered MIDI");
    } else {
        s->is_mus = -1;
        ps_sound_log("Music: unknown format");
    }
    return s;
}

void I_UnRegisterSong(void *handle) {
    /* ps_libc malloc is a bump allocator — no free */
    (void)handle;
}

void I_PlaySong(void *handle, boolean looping) {
    song_t *s = (song_t *)handle;
    if (!s) { ps_sound_log("Music: PlaySong NULL"); return; }

    mus_data   = s->data;
    mus_len    = s->len;
    mus_loop   = looping ? 1 : 0;
    mus_is_mus = s->is_mus;

    if (mus_is_mus == -1) {
        ps_sound_log("Music: unsupported format");
        return;
    }

    if (mus_is_mus) mus_parse_header();
    else            midi_parse_header();

    if (mus_end_of_track) {
        ps_sound_log("Music: parse failed");
        return;
    }

    mus_playing = 1;
    ps_sound_log(looping ? "Music: playing (loop)"
                         : "Music: playing once");
}

void I_StopSong(void) {
    mus_playing = 0;
    music_all_notes_off();
}

boolean I_IsSongPlaying(void) {
    return mus_playing ? 1 : 0;
}

boolean I_MusicIsPlaying(void) {
    return mus_playing ? 1 : 0;
}
