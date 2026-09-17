/*
 * i_sound_ps.c — Doom sound backend for PS4/PS5.
 *
 * v6: added MIDI music synthesizer.
 *     - I_RegisterSong / I_PlaySong / I_StopSong / I_UnRegisterSong
 *     - Square-wave note synthesis with per-note velocity
 *     - Parses MIDI (post mus2mid) events
 *     - Music mixed into the same buffer as SFX via I_SubmitSound
 *
 * The audio thread in main.c calls I_SubmitSound in a tight loop,
 * which mixes SFX + music and submits to libSceAudioOut.
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
 * Globals — must exist for linkage
 * ============================================================ */
int snd_musicdevice = 0;   /* set to 3 (General MIDI) in I_InitSound */
int snd_sfxdevice   = 0;
int snd_musicvolume = 8;   /* 0..15 */
int snd_sfxvolume   = 8;

#define NCHANNELS        16
#define MIXBUF           512
#define MUSIC_MAX_NOTES  16

/* ============================================================
 * SFX
 * ============================================================ */
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
static short     mixbuf[MIXBUF * 2];
static int       sound_start_count = 0;

/* ============================================================
 * MUSIC (MIDI synth)
 * ============================================================ */
typedef struct {
    int active;
    int note;
    unsigned int phase;
    unsigned int phase_inc;
    int env;              /* current amplitude 0..127 */
} music_note_t;

typedef struct {
    unsigned char *data;
    int len;
} song_t;

static music_note_t mus_notes[MUSIC_MAX_NOTES];
static unsigned char *mus_data = 0;
static int      mus_len = 0;
static int      mus_pos = 0;
static int      mus_track_start = 0;
static int      mus_track_end = 0;
static int      mus_loop = 0;
static int      mus_playing = 0;
static int      mus_end_of_track = 0;
static int      mus_us_per_tick = 5208;   /* default 120 BPM / 96 PPQN */
static int      mus_sample_acc = 0;
static int      mus_current_tick = 0;
static int      mus_next_event_tick = 0;
static unsigned char mus_running_status = 0;

static unsigned int note_phase_inc_table[128];

/* --- phase_inc table: freq * 65536 / SAMPLE_RATE --- */
static void init_music_tables(void) {
    /* A4 = note 69 = 440 Hz → 440 * 65536 / 48000 = 601 (16.16) */
    const unsigned int SEMITONE_RATIO = 69433;   /* 2^(1/12) * 65536 */
    unsigned int f = (unsigned int)((440ULL * 65536) / SAMPLE_RATE);
    note_phase_inc_table[69] = f;
    for (int i = 70; i < 128; i++) {
        f = (unsigned int)(((unsigned long long)f * SEMITONE_RATIO) >> 16);
        note_phase_inc_table[i] = f;
    }
    f = (unsigned int)((440ULL * 65536) / SAMPLE_RATE);
    for (int i = 68; i >= 0; i--) {
        f = (unsigned int)(((unsigned long long)f * 65536) / SEMITONE_RATIO);
        note_phase_inc_table[i] = f;
    }
}

/* --- variable-length quantity reader --- */
static int mus_read_varlen(void) {
    int v = 0;
    while (mus_pos < mus_track_end) {
        unsigned char b = mus_data[mus_pos++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

/* --- note management --- */
static void music_note_on(int note, int vel) {
    if (note < 0 || note > 127 || vel <= 0) return;
    int slot = -1;
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (!mus_notes[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    mus_notes[slot].active    = 1;
    mus_notes[slot].note      = note;
    mus_notes[slot].phase     = 0;
    mus_notes[slot].phase_inc = note_phase_inc_table[note];
    mus_notes[slot].env       = vel;   /* 0..127 */
}

static void music_note_off(int note) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++) {
        if (mus_notes[i].active && mus_notes[i].note == note)
            mus_notes[i].active = 0;
    }
}

static void music_all_notes_off(void) {
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        mus_notes[i].active = 0;
}

/* --- MIDI event decoder --- */
static void music_execute_event(void) {
    if (mus_pos >= mus_track_end) { mus_end_of_track = 1; return; }

    unsigned char status = mus_data[mus_pos];
    if (status < 0x80) {
        status = mus_running_status;
    } else {
        mus_pos++;
        mus_running_status = status;
    }

    unsigned char type = status & 0xF0;

    switch (type) {
    case 0x80:  /* Note off */
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        music_note_off(mus_data[mus_pos]);
        mus_pos += 2;
        break;

    case 0x90: { /* Note on */
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        int note = mus_data[mus_pos];
        int vel  = mus_data[mus_pos + 1];
        mus_pos += 2;
        if (vel == 0) music_note_off(note);
        else          music_note_on(note, vel);
        break;
    }

    case 0xA0:  /* Poly aftertouch */
    case 0xB0:  /* Control change */
    case 0xE0:  /* Pitch bend */
        if (mus_pos + 2 > mus_track_end) { mus_end_of_track = 1; return; }
        mus_pos += 2;
        break;

    case 0xC0:  /* Program change */
    case 0xD0:  /* Channel pressure */
        if (mus_pos + 1 > mus_track_end) { mus_end_of_track = 1; return; }
        mus_pos += 1;
        break;

    case 0xF0: {
        if (status == 0xFF) {
            /* Meta event: FF <type> <varlen> <data> */
            if (mus_pos + 1 > mus_track_end) { mus_end_of_track = 1; return; }
            unsigned char meta = mus_data[mus_pos++];
            int mlen = mus_read_varlen();
            if (meta == 0x2F) { mus_end_of_track = 1; return; }
            if (meta == 0x51 && mlen == 3 && mus_pos + 3 <= mus_track_end) {
                int tempo = (mus_data[mus_pos] << 16)
                          | (mus_data[mus_pos + 1] << 8)
                          | (mus_data[mus_pos + 2]);
                /* We don't store PPQN here; keep default */
                (void)tempo;
            }
            mus_pos += mlen;
            if (mus_pos > mus_track_end) mus_pos = mus_track_end;
        } else if (status == 0xF0 || status == 0xF7) {
            int len = mus_read_varlen();
            mus_pos += len;
            if (mus_pos > mus_track_end) mus_pos = mus_track_end;
        }
        break;
    }

    default:
        mus_end_of_track = 1;
        break;
    }
}

/* --- header parser: locate first MTrk chunk --- */
static void music_parse_header(void) {
    mus_end_of_track   = 0;
    mus_current_tick   = 0;
    mus_next_event_tick= 0;
    mus_running_status = 0;
    mus_sample_acc     = 0;
    music_all_notes_off();

    if (!mus_data || mus_len < 14) { mus_end_of_track = 1; return; }

    if (mus_data[0] != 'M' || mus_data[1] != 'T' ||
        mus_data[2] != 'h' || mus_data[3] != 'd') {
        ps_sound_log("Music: no MThd header");
        mus_end_of_track = 1;
        return;
    }

    int ppqn = (mus_data[12] << 8) | mus_data[13];
    if (ppqn == 0 || ppqn > 0x7FFF) ppqn = 96;

    /* Default tempo: 500000 us / quarter note (120 BPM) */
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
            return;
        }
        int clen = (mus_data[p+4] << 24) | (mus_data[p+5] << 16)
                 | (mus_data[p+6] << 8)  |  mus_data[p+7];
        p += 8 + clen;
    }

    ps_sound_log("Music: no MTrk chunk");
    mus_end_of_track = 1;
}

/* --- one MIDI tick --- */
static void music_process_tick(void) {
    if (!mus_playing) return;

    if (mus_end_of_track) {
        if (mus_loop) {
            mus_pos            = mus_track_start;
            mus_running_status = 0;
            mus_end_of_track   = 0;
            mus_current_tick   = 0;
            mus_sample_acc     = 0;
            music_all_notes_off();
            mus_next_event_tick = mus_read_varlen();
            return;
        } else {
            mus_playing = 0;
            music_all_notes_off();
            return;
        }
    }

    if (mus_current_tick >= mus_next_event_tick) {
        music_execute_event();
        if (!mus_end_of_track && mus_pos < mus_track_end) {
            mus_next_event_tick += mus_read_varlen();
        } else {
            mus_end_of_track = 1;
        }
    }
    mus_current_tick++;
}

/* --- mix active music notes into buf --- */
static void music_render(short *buf, int frames) {
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
                music_process_tick();
                if (!mus_playing) break;
            }
        }

        if (!mus_playing) break;

        /* Sum active notes */
        int mix = 0;
        for (int j = 0; j < MUSIC_MAX_NOTES; j++) {
            music_note_t *n = &mus_notes[j];
            if (!n->active) continue;

            /* Square wave: top bit of 32-bit phase */
            int sample = (n->phase & 0x80000000) ? -1 : 1;
            sample = sample * n->env;   /* ±127 max */
            mix += sample;

            n->phase += n->phase_inc;
        }

        /* Scale: max ~16 × 127 = 2032.  Target ±7000 for audible music. */
        mix = (mix * 4) * vol / 15;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;

        buf[i * 2]     += (short)mix;
        buf[i * 2 + 1] += (short)mix;
    }
}

/* ============================================================
 * Doom SFX API
 * ============================================================ */
void I_BindSoundVariables(void) {}

void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < NCHANNELS; i++) channels[i].active = 0;
    init_music_tables();

    /* Force music device to a non-zero value so Doom's S_Init
     * believes a music backend exists and calls I_InitMusic. */
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

/* --- mix SFX + music, submit --- */
void I_SubmitSound(void) {
    for (int i = 0; i < MIXBUF * 2; i++) mixbuf[i] = 0;

    /* SFX */
    for (int c = 0; c < NCHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->active) continue;

        int pos = ch->pos;
        for (int i = 0; i < MIXBUF; i++) {
            int idx = pos >> 16;
            if (idx >= ch->length) { ch->active = 0; break; }

            int sample = ((int)ch->data[idx] - 128) << 8;
            sample = sample * ch->volume / 80;

            int left  = sample * (255 - ch->pan) / 200;
            int right = sample * ch->pan / 200;

            mixbuf[i * 2]     += (short)left;
            mixbuf[i * 2 + 1] += (short)right;

            pos += ch->step;
        }
        ch->pos = pos;
    }

    /* Music */
    music_render(mixbuf, MIXBUF);

    /* Final clip */
    for (int i = 0; i < MIXBUF * 2; i++) {
        if (mixbuf[i] >  32767) mixbuf[i] =  32767;
        if (mixbuf[i] < -32768) mixbuf[i] = -32768;
    }

    dg_audio_callback(mixbuf, MIXBUF);
}

/* --- SFX start / stop / query --- */
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
    ch->pan    = sep;
    ch->step   = 1 << 16;
    ch->pos    = 0;
    ch->data   = samples;
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
        if (ch->volume >=  10) b[p++] = '0' + (ch->volume /  10) % 10;
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
    /* Silence by zeroing envelope; notes stay allocated */
    for (int i = 0; i < MUSIC_MAX_NOTES; i++)
        mus_notes[i].env = 0;
}

void I_ResumeSong(void) {
    /* Nothing — notes resume on next note-on */
}

void *I_RegisterSong(void *data, int len) {
    song_t *s = (song_t *)malloc(sizeof(song_t));
    if (!s) return 0;
    s->data = (unsigned char *)malloc((unsigned)len);
    if (!s->data) return 0;
    memcpy(s->data, data, len);
    s->len = len;

    ps_sound_log("Music: registered song");
    return s;
}

void I_UnRegisterSong(void *handle) {
    /* ps_libc malloc is bump-allocator, no free */
    (void)handle;
}

void I_PlaySong(void *handle, boolean looping) {
    song_t *s = (song_t *)handle;
    if (!s) {
        ps_sound_log("Music: PlaySong with NULL");
        return;
    }

    mus_data = s->data;
    mus_len  = s->len;
    mus_loop = looping ? 1 : 0;

    music_parse_header();
    if (mus_end_of_track) {
        ps_sound_log("Music: parse failed");
        return;
    }
    mus_playing         = 1;
    mus_current_tick    = 0;
    mus_next_event_tick = mus_read_varlen();

    ps_sound_log(looping ? "Music: playing (loop)" : "Music: playing (once)");
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
