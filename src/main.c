/*
 * doom-ps/src/main.c — v14
 *
 * Fixes over v13:
 *   - O_CREAT/O_TRUNC use PS4/PS5 values (0x100 / 0x1000)
 *   - WAD open attempts unrolled, no nested-function stack frames
 *   - Single static diag buffer (no stack recursion in native_call)
 *   - "By MexrlDev" credit line under "doomgeneric on Luac0re"
 *   - Error screen says "Reboot game to recover"
 */

#include "core.h"
#include "doomgeneric_ps.h"
#include "font.h"

/* ========================================================================
 * Minimal helpers
 * ======================================================================== */
static void ps_memset(void *dst, u8 val, u64 len) {
    u8 *d = (u8 *)dst;
    while (len--) *d++ = val;
}
static void ps_memcpy(void *dst, const void *src, u64 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (len--) *d++ = *s++;
}
static u64 ps_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }

/* ========================================================================
 * DualShock bits
 * ======================================================================== */
#define DS_UP 0x10
#define DS_RIGHT 0x20
#define DS_DOWN 0x40
#define DS_LEFT 0x80
#define DS_L2 0x100
#define DS_R2 0x200
#define DS_L1 0x400
#define DS_R1 0x800
#define DS_TRIANGLE 0x1000
#define DS_CIRCLE 0x2000
#define DS_CROSS 0x4000
#define DS_SQUARE 0x8000
#define DS_OPTIONS 0x08
#define DS_SHARE 0x01
#define DS_L3 0x02

#define DG_KEY_RIGHTARROW 0xae
#define DG_KEY_LEFTARROW  0xac
#define DG_KEY_UPARROW    0xad
#define DG_KEY_DOWNARROW  0xaf
#define DG_KEY_FIRE       0xa0
#define DG_KEY_USE        0x20
#define DG_KEY_ESCAPE     27
#define DG_KEY_ENTER      13
#define DG_KEY_TAB        9
#define DG_KEY_SHIFT      0xa2
#define DG_KEY_ALT        0xa4
#define DG_KEY_F1         (0x80+0x3b)
#define DG_KEY_F2         (0x80+0x3c)
#define DG_KEY_1 '1'
#define DG_KEY_2 '2'
#define DG_KEY_3 '3'
#define DG_KEY_4 '4'
#define DG_KEY_5 '5'
#define DG_KEY_6 '6'
#define DG_KEY_7 '7'

/* ========================================================================
 * PS4/PS5 file open flags — CORRECTED for libkernel
 * ======================================================================== */
#define O_RDONLY_  0x0000
#define O_WRONLY_  0x0001
#define O_RDWR_    0x0002
#define O_CREAT_   0x0100   /* PS4/PS5 value (not FreeBSD desktop) */
#define O_TRUNC_   0x1000   /* PS4/PS5 value */

/* ========================================================================
 * Global context
 * ======================================================================== */
#define KEY_QUEUE_SIZE 32

static struct ps_ctx {
    void *G, *D;
    void *usleep_fn, *load_mod, *alloc_dm, *map_dm, *dm_size;
    void *create_eq, *wait_eq, *delete_eq, *mmap_fn;
    void *kopen, *kwrite, *kclose, *clock_gettime;
    void *accept_fn, *recv_fn, *close_fn, *sendto_fn, *setsockopt_fn, *cancel;

    void *vid_open, *vid_close, *vid_reg, *vid_flip, *vid_rate, *vid_evt;
    s32   video_h;
    void *vmem;
    void *fbs[2];
    int   active_fb;
    u64   eq;
    u32   total_frames;

    void *aud_open, *aud_out, *aud_close;
    s32   audio_h;
    u8   *ring;
    int   ring_write, ring_read, ring_count;

    void *pad_init_fn, *pad_geth, *pad_read;
    s32   pad_h;
    u32   pad_prev;
    s32   user_id;

    struct { u8 key; u8 pressed; } key_queue[KEY_QUEUE_SIZE];
    int   key_wp, key_rp;

    char  wad_path[128];
    s32   log_fd;
    u8    log_sa[16];
    struct ext_args *ext;
} g_ctx;

/* Single static diag buffer — no per-call stack allocation */
static char g_diag[256];

/* ========================================================================
 * UDP log
 * ======================================================================== */
static void udp_log(const char *msg) {
    struct ps_ctx *c = &g_ctx;
    if (c->log_fd < 0 || !c->sendto_fn) return;
    NC(c->G, c->sendto_fn,
       (u64)c->log_fd, (u64)msg, (u64)ps_strlen(msg),
       0, (u64)c->log_sa, 16);
}

/* Log "DoomPS: <prefix><path> fd=0xNNNNNNNN\n" */
static void diag_kopen(const char *prefix, const char *path, s32 fd) {
    int p = 0;
    const char *m = prefix;
    while (*m && p < 40) g_diag[p++] = *m++;
    int i = 0;
    while (path[i] && p < 110) g_diag[p++] = path[i++];
    g_diag[p++] = ' ';
    g_diag[p++] = 'f'; g_diag[p++] = 'd'; g_diag[p++] = '=';
    g_diag[p++] = '0'; g_diag[p++] = 'x';
    const char h[] = "0123456789ABCDEF";
    u32 v = (u32)fd;
    for (int k = 0; k < 8; k++)
        g_diag[p++] = h[(v >> (28 - k*4)) & 0xF];
    g_diag[p++] = '\n';
    g_diag[p] = 0;
    udp_log(g_diag);
}

/* ========================================================================
 * Shared present helper
 * ======================================================================== */
static void present(struct ps_ctx *c) {
    if (c->video_h < 0 || !c->vid_flip) return;
    NC(c->G, c->vid_flip, (u64)c->video_h, (u64)c->active_fb, 1,
       (u64)c->total_frames, 0, 0);
    if (c->eq && c->wait_eq) {
        u8 evt[64]; s32 cnt = 0;
        NC(c->G, c->wait_eq, c->eq, (u64)evt, 1, (u64)&cnt, 0, 0);
    }
    c->active_fb ^= 1;
    c->total_frames++;
}

/* ========================================================================
 * On-screen LOADING / progress / error
 * ======================================================================== */
static void show_loading(struct ps_ctx *c, int dots, const char *status) {
    if (c->video_h < 0 || !c->fbs[c->active_fb]) return;
    u32 *fb = (u32 *)c->fbs[c->active_fb];
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF101018;

    ps_draw_str_center(fb, 280, "DOOM-PS", 0xFFFFAA00, 8);
    ps_draw_str_center(fb, 400, "doomgeneric on Luac0re",
                       0xFF808080, 3);
    /* By MexrlDev — 3px below the previous line */
    ps_draw_str_center(fb, 427, "By MexrlDev",
                       0xFF606060, 2);

    char buf[32]; int p = 0;
    const char *base = "LOADING";
    while (base[p]) { buf[p] = base[p]; p++; }
    for (int i = 0; i < dots; i++) buf[p++] = '.';
    buf[p] = 0;
    ps_draw_str_center(fb, 540, buf, 0xFFFFFFFF, 5);

    if (status) ps_draw_str_center(fb, 700, status, 0xFFA0A0A0, 3);

    present(c);
}

static void show_wad_progress(struct ps_ctx *c, u64 got, u64 total) {
    if (c->video_h < 0 || !c->fbs[c->active_fb]) return;
    u32 *fb = (u32 *)c->fbs[c->active_fb];
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF101018;

    ps_draw_str_center(fb, 280, "DOOM-PS", 0xFFFFAA00, 8);
    ps_draw_str_center(fb, 400, "doomgeneric on Luac0re",
                       0xFF808080, 3);
    ps_draw_str_center(fb, 427, "By MexrlDev",
                       0xFF606060, 2);

    int pct = (total > 0) ? (int)(got * 100 / total) : 0;
    char buf[40]; int p = 0;
    const char *pre = "RECEIVING WAD ";
    while (pre[p]) { buf[p] = pre[p]; p++; }
    if (pct >= 100) buf[p++] = '1';
    if (pct >= 10)  buf[p++] = '0' + ((pct / 10) % 10);
    buf[p++] = '0' + (pct % 10);
    buf[p++] = '%'; buf[p] = 0;
    ps_draw_str_center(fb, 540, buf, 0xFFFFFFFF, 5);

    int bar_x = 200, bar_y = 680, bar_w = SCR_W - 400, bar_h = 40;
    int filled = (total > 0) ? (int)((u64)bar_w * got / total) : 0;
    if (filled > bar_w) filled = bar_w;
    ps_fill_rect(fb, bar_x, bar_y, bar_w, bar_h, 0xFF303030);
    ps_fill_rect(fb, bar_x, bar_y, filled, bar_h, 0xFF00FF00);

    present(c);
}

static void show_error_and_hang(struct ps_ctx *c, const char *line1,
                                const char *line2) {
    for (;;) {
        if (c->video_h < 0 || !c->fbs[c->active_fb]) continue;
        u32 *fb = (u32 *)c->fbs[c->active_fb];
        for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF200000;

        ps_draw_str_center(fb, 300, "DOOM-PS ERROR", 0xFFFF4040, 6);
        if (line1) ps_draw_str_center(fb, 500, line1, 0xFFFFFFFF, 4);
        if (line2) ps_draw_str_center(fb, 600, line2, 0xFFA0A0A0, 3);
        ps_draw_str_center(fb, 900, "Reboot game to recover",
                           0xFF808080, 3);

        present(c);
        if (c->usleep_fn) NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    }
}

/* ========================================================================
 * Blit / key / audio
 * ======================================================================== */
static void blit_doom_frame(u32 *fb, const u32 *doom) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
    if (!doom) return;
    for (int dy = 0; dy < DOOM_H; dy++) {
        const u32 *row = doom + dy * DOOM_W;
        for (int dx = 0; dx < DOOM_W; dx++) {
            u32 rgba = row[dx];
            u8 r = rgba & 0xFF;
            u8 g = (rgba >> 8) & 0xFF;
            u8 b = (rgba >> 16) & 0xFF;
            u32 bgra = 0xFF000000 | ((u32)r << 16) | ((u32)g << 8) | b;
            int fy = OFF_Y + dy * SCALE_Y;
            int fx = OFF_X + dx * SCALE_X;
            for (int sy = 0; sy < SCALE_Y; sy++) {
                u32 *dst = fb + (fy + sy) * SCR_W + fx;
                for (int sx = 0; sx < SCALE_X; sx++) dst[sx] = bgra;
            }
        }
    }
}

static void push_key(u8 key, u8 pressed) {
    struct ps_ctx *c = &g_ctx;
    int next = (c->key_wp + 1) & (KEY_QUEUE_SIZE - 1);
    if (next == c->key_rp) return;
    c->key_queue[c->key_wp].key = key;
    c->key_queue[c->key_wp].pressed = pressed;
    c->key_wp = next;
}
static void translate_pad(u32 raw) {
    struct ps_ctx *c = &g_ctx;
    u32 ch = raw ^ c->pad_prev; c->pad_prev = raw;
#define MAP(b,k) if (ch & (b)) push_key((k), (raw & (b)) ? 1 : 0)
    MAP(DS_UP,DG_KEY_UPARROW);   MAP(DS_DOWN,DG_KEY_DOWNARROW);
    MAP(DS_LEFT,DG_KEY_LEFTARROW); MAP(DS_RIGHT,DG_KEY_RIGHTARROW);
    MAP(DS_CROSS,DG_KEY_FIRE);   MAP(DS_SQUARE,DG_KEY_USE);
    MAP(DS_CIRCLE,DG_KEY_ENTER); MAP(DS_OPTIONS,DG_KEY_ESCAPE);
    MAP(DS_TRIANGLE,DG_KEY_TAB); MAP(DS_L1,DG_KEY_SHIFT);
    MAP(DS_R1,DG_KEY_ALT);       MAP(DS_L2,DG_KEY_F1);
    MAP(DS_R2,DG_KEY_F2);
    if ((raw&DS_SHARE)&&(ch&DS_CROSS))    push_key(DG_KEY_1,(raw&DS_CROSS)?1:0);
    if ((raw&DS_SHARE)&&(ch&DS_SQUARE))   push_key(DG_KEY_2,(raw&DS_SQUARE)?1:0);
    if ((raw&DS_SHARE)&&(ch&DS_CIRCLE))   push_key(DG_KEY_3,(raw&DS_CIRCLE)?1:0);
    if ((raw&DS_SHARE)&&(ch&DS_TRIANGLE)) push_key(DG_KEY_4,(raw&DS_TRIANGLE)?1:0);
    if ((raw&DS_L3)&&(ch&DS_CROSS))       push_key(DG_KEY_5,(raw&DS_CROSS)?1:0);
    if ((raw&DS_L3)&&(ch&DS_SQUARE))      push_key(DG_KEY_6,(raw&DS_SQUARE)?1:0);
    if ((raw&DS_L3)&&(ch&DS_CIRCLE))      push_key(DG_KEY_7,(raw&DS_CIRCLE)?1:0);
#undef MAP
}
static void audio_drain(void) {
    struct ps_ctx *c = &g_ctx;
    if (c->audio_h < 0 || !c->aud_out || !c->ring) return;
    while (c->ring_count > 0) {
        u8 *slot = c->ring + c->ring_read * RING_BYTES;
        NC(c->G, c->aud_out, (u64)c->audio_h, (u64)slot, 0,0,0,0);
        c->ring_read = (c->ring_read + 1) & (RING_SLOTS - 1);
        c->ring_count--;
    }
}

/* ========================================================================
 * WAD receive — unrolled open attempts, checkpoints at each step
 * ======================================================================== */
#define WAD_CHUNK 4096
static int recv_wad(s32 listen_fd) {
    struct ps_ctx *c = &g_ctx;
    if (listen_fd < 0) {
        udp_log("DoomPS: listen_fd < 0\n");
        return -1;
    }

    udp_log("DoomPS: waiting for WAD on TCP...\n");

    if (c->setsockopt_fn) {
        u8 tv[16] = {0};
        *(u64 *)(tv + 8) = 500000;
        s32 so_ret = (s32)NC(c->G, c->setsockopt_fn,
                             (u64)listen_fd, 0xFFFF, 0x1006,
                             (u64)tv, 16, 0);
        if (so_ret != 0)
            udp_log("DoomPS: WARN SO_RCVTIMEO failed\n");
    }

    int dots = 0;
    s32 client = -1;
    for (int attempt = 0; attempt < 600; attempt++) {
        u8 peer[16]; s32 plen = 16;
        client = (s32)NC(c->G, c->accept_fn,
                         (u64)listen_fd, (u64)peer, (u64)&plen, 0,0,0);
        if (client >= 0) break;
        show_loading(c, dots, "Waiting for WAD upload...");
        dots = (dots + 1) & 3;
    }
    if (client < 0) {
        udp_log("DoomPS: accept failed\n");
        return -1;
    }

    show_loading(c, 0, "WAD connected, receiving...");
    udp_log("DoomPS: receiving WAD...\n");

    u8 hdr[8]; s32 got = 0;
    while (got < 8) {
        s32 n = (s32)NC(c->G, c->recv_fn,
                        (u64)client, (u64)(hdr + got),
                        (u64)(8 - got), 0,0,0);
        if (n <= 0) {
            NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0);
            udp_log("DoomPS: hdr read failed\n");
            return -1;
        }
        got += n;
    }
    u64 wad_size = 0;
    for (int i = 0; i < 8; i++) wad_size |= ((u64)hdr[i] << (i * 8));

    /* Print WAD size */
    {
        int p = 0;
        const char *m = "DoomPS: WAD size = ";
        while (*m) g_diag[p++] = *m++;
        u64 v = wad_size;
        char tmp[24]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t) g_diag[p++] = tmp[--t];
        g_diag[p++] = '\n'; g_diag[p] = 0;
        udp_log(g_diag);
    }

    /* ================================================================
     * Unrolled open attempts — no nested functions, single static buffer,
     * correct PS4/PS5 O_CREAT/O_TRUNC values.
     * ================================================================ */
    const char *wad_out = (const char *)0;
    s32 fd = -1;

    udp_log("DoomPS: [W1] kopen /av_contents/content_tmp/doom.wad 0x1101\n");
    fd = (s32)NC(c->G, c->kopen,
                 (u64)"/av_contents/content_tmp/doom.wad",
                 (u64)0x1101, 0x1FF, 0,0,0);
    diag_kopen("DoomPS: [W1r] ", "/av_contents/content_tmp/doom.wad", fd);
    if (fd >= 0) wad_out = "/av_contents/content_tmp/doom.wad";

    if (fd < 0) {
        udp_log("DoomPS: [W2] kopen /tmp/doom.wad 0x1101\n");
        fd = (s32)NC(c->G, c->kopen,
                     (u64)"/tmp/doom.wad",
                     (u64)0x1101, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W2r] ", "/tmp/doom.wad", fd);
        if (fd >= 0) wad_out = "/tmp/doom.wad";
    }

    if (fd < 0) {
        udp_log("DoomPS: [W3] kopen /savedata0/doom.wad 0x1101\n");
        fd = (s32)NC(c->G, c->kopen,
                     (u64)"/savedata0/doom.wad",
                     (u64)0x1101, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W3r] ", "/savedata0/doom.wad", fd);
        if (fd >= 0) wad_out = "/savedata0/doom.wad";
    }

    if (fd < 0) {
        udp_log("DoomPS: [W4] kopen /temp0/doom.wad 0x1101\n");
        fd = (s32)NC(c->G, c->kopen,
                     (u64)"/temp0/doom.wad",
                     (u64)0x1101, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W4r] ", "/temp0/doom.wad", fd);
        if (fd >= 0) wad_out = "/temp0/doom.wad";
    }

    if (fd < 0) {
        udp_log("DoomPS: [W5] kopen doom.wad 0x1101\n");
        fd = (s32)NC(c->G, c->kopen,
                     (u64)"doom.wad",
                     (u64)0x1101, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W5r] ", "doom.wad", fd);
        if (fd >= 0) wad_out = "doom.wad";
    }

    /* Fallback: try RDWR instead of WRONLY */
    if (fd < 0) {
        udp_log("DoomPS: [W6] kopen /av_contents/content_tmp/doom.wad 0x1102\n");
        fd = (s32)NC(c->G, c->kopen,
                     (u64)"/av_contents/content_tmp/doom.wad",
                     (u64)0x1102, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W6r] ", "/av_contents/content_tmp/doom.wad", fd);
        if (fd >= 0) wad_out = "/av_contents/content_tmp/doom.wad";
    }

    if (fd < 0 || !wad_out) {
        udp_log("DoomPS: all WAD open attempts failed\n");
        NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0);
        return -1;
    }

    /* Save chosen path */
    {
        int i = 0;
        while (wad_out[i] && i < 127) { c->wad_path[i] = wad_out[i]; i++; }
        c->wad_path[i] = 0;
    }
    {
        int p = 0;
        const char *m = "DoomPS: using ";
        while (*m) g_diag[p++] = *m++;
        int i = 0;
        while (c->wad_path[i] && p < 250) g_diag[p++] = c->wad_path[i++];
        g_diag[p++] = '\n'; g_diag[p] = 0;
        udp_log(g_diag);
    }

    /* Stream the WAD data */
    u8 chunk[WAD_CHUNK];
    u64 remaining = wad_size;
    u64 total = wad_size;
    int last_pct = -1;
    while (remaining > 0) {
        u64 want = remaining < WAD_CHUNK ? remaining : WAD_CHUNK;
        s32 n = (s32)NC(c->G, c->recv_fn,
                        (u64)client, (u64)chunk, (u64)want, 0,0,0);
        if (n <= 0) break;
        NC(c->G, c->kwrite, (u64)fd, (u64)chunk, (u64)n, 0,0,0);
        remaining -= (u64)n;
        int pct = (total > 0) ? (int)((total - remaining) * 100 / total) : 0;
        if (pct != last_pct) {
            show_wad_progress(c, total - remaining, total);
            last_pct = pct;
        }
    }

    NC(c->G, c->kclose, (u64)fd, 0,0,0,0,0);
    NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0);

    if (remaining > 0) {
        udp_log("DoomPS: WAD truncated\n");
        return -1;
    }

    udp_log("DoomPS: WAD written OK\n");
    show_loading(c, 3, "WAD ready, launching Doom...");
    return 0;
}

/* ========================================================================
 * doomgeneric callbacks
 * ======================================================================== */
extern u32 *DG_ScreenBuffer;
extern void doomgeneric_Create(int argc, char **argv);
extern void doomgeneric_Tick(void);

void DG_Init(void) {
    if (!DG_ScreenBuffer) {
        DG_ScreenBuffer = (u32 *)malloc(DOOM_W * DOOM_H * 4);
        if (!DG_ScreenBuffer)
            udp_log("DoomPS: DG_ScreenBuffer alloc FAILED\n");
    }
}

void DG_DrawFrame(void) {
    struct ps_ctx *c = &g_ctx;
    blit_doom_frame((u32 *)c->fbs[c->active_fb], DG_ScreenBuffer);
    present(c);
    if (c->ext) c->ext->frame_count = c->total_frames;
    audio_drain();
    if (c->pad_h >= 0 && c->pad_read) {
        u8 pad_buf[128]; ps_memset(pad_buf, 0, 128);
        s32 n = (s32)NC(c->G, c->pad_read,
                        (u64)c->pad_h, (u64)pad_buf, 1, 0, 0, 0);
        if (n > 0 && (u32)n < 0x80000000) {
            u32 raw = *(u32 *)pad_buf;
            if (!(raw & 0x80000000)) translate_pad(raw & 0x001FFFFF);
        }
    }
}
void DG_SleepMs(unsigned int ms) {
    struct ps_ctx *c = &g_ctx;
    if (c->usleep_fn) NC(c->G, c->usleep_fn, (u64)ms * 1000ULL, 0,0,0,0,0);
}
unsigned int DG_GetTicksMs(void) {
    struct ps_ctx *c = &g_ctx;
    if (!c->clock_gettime) return c->total_frames * 16;
    u64 ts[2] = {0,0};
    s32 rc = (s32)NC(c->G, c->clock_gettime, 4, (u64)ts, 0,0,0,0);
    if (rc != 0) return c->total_frames * 16;
    return (unsigned int)(ts[0] * 1000ULL + ts[1] / 1000000ULL);
}
int DG_GetKey(int *pressed, unsigned char *doomKey) {
    struct ps_ctx *c = &g_ctx;
    if (c->key_rp == c->key_wp) return 0;
    *doomKey = c->key_queue[c->key_rp].key;
    *pressed = c->key_queue[c->key_rp].pressed;
    c->key_rp = (c->key_rp + 1) & (KEY_QUEUE_SIZE - 1);
    return 1;
}
void DG_SetWindowTitle(const char *t) { (void)t; }

void dg_audio_callback(const short *pcm, int sample_count) {
    struct ps_ctx *c = &g_ctx;
    if (!c->ring) return;
    int frames = sample_count, off = 0;
    while (frames > 0 && c->ring_count < RING_SLOTS) {
        u8 *slot = c->ring + c->ring_write * RING_BYTES;
        int cp = frames < SAMPLES_PER_BUF ? frames : SAMPLES_PER_BUF;
        ps_memcpy(slot, pcm + off * 2, (u64)(cp * 4));
        if (cp < SAMPLES_PER_BUF)
            ps_memset(slot + cp * 4, 0,
                      (u64)((SAMPLES_PER_BUF - cp) * 4));
        c->ring_write = (c->ring_write + 1) & (RING_SLOTS - 1);
        c->ring_count++; off += cp; frames -= cp;
    }
}

/* ========================================================================
 * _start
 * ======================================================================== */
__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    ext->step = 1;
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    struct ps_ctx *c = &g_ctx;

    ps_memset(c, 0, sizeof(*c));
    c->video_h = -1; c->audio_h = -1; c->pad_h = -1; c->log_fd = -1;
    c->ext = ext; c->G = G; c->D = D;
    c->log_fd = ext->log_fd;
    for (int i = 0; i < 16; i++) c->log_sa[i] = ext->log_addr[i];

    ext->step = 2;

    c->sendto_fn = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    udp_log("DoomPS: [3] sendto resolved\n");
    ext->step = 3;

    extern void ps_libc_init(void *G, void *D);
    ps_libc_init(G, D);
    udp_log("DoomPS: [4] ps_libc_init OK\n");
    ext->step = 4;

    c->usleep_fn = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    udp_log("DoomPS: [5] usleep\n");
    ext->step = 5;

    c->cancel   = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    c->load_mod = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    udp_log("DoomPS: [6] cancel+load_mod\n");
    ext->step = 6;

    c->alloc_dm = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    c->map_dm   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    c->dm_size  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    udp_log("DoomPS: [7] dmem\n");
    ext->step = 7;

    c->create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    c->wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    c->delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    udp_log("DoomPS: [8] equeue\n");
    ext->step = 8;

    c->mmap_fn       = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    c->kopen         = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    c->kwrite        = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    c->kclose        = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    c->clock_gettime = SYM(G, D, LIBKERNEL_HANDLE, "clock_gettime");
    udp_log("DoomPS: [9] file/clock\n");
    ext->step = 9;

    c->accept_fn     = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    c->recv_fn       = SYM(G, D, LIBKERNEL_HANDLE, "recv");
    c->close_fn      = SYM(G, D, LIBKERNEL_HANDLE, "close");
    c->setsockopt_fn = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    udp_log("DoomPS: [10] sockets\n");
    ext->step = 10;

    if (!c->usleep_fn || !c->load_mod) {
        ext->status = -1;
        udp_log("DoomPS: [11] symbol check failed\n");
        ext->step = 11;
        return;
    }
    udp_log("DoomPS: [12] symbols OK\n");
    ext->step = 12;

    s32 vid_mod = (s32)NC(c->G, c->load_mod,
                          (u64)"libSceVideoOut.sprx",0,0,0,0,0);
    udp_log("DoomPS: [13] VideoOut.sprx\n");
    ext->step = 13;
    s32 aud_mod = (s32)NC(c->G, c->load_mod,
                          (u64)"libSceAudioOut.sprx",0,0,0,0,0);
    udp_log("DoomPS: [14] AudioOut.sprx\n");
    ext->step = 14;
    s32 pad_mod = (s32)NC(c->G, c->load_mod,
                          (u64)"libScePad.sprx",0,0,0,0,0);
    udp_log("DoomPS: [15] Pad.sprx\n");
    ext->step = 15;
    s32 usr_mod = (s32)NC(c->G, c->load_mod,
                          (u64)"libSceUserService.sprx",0,0,0,0,0);
    udp_log("DoomPS: [16] UserService.sprx\n");
    ext->step = 16;

    c->user_id = 0;
    if (usr_mod > 0) {
        void *get_user = SYM(c->G, c->D, usr_mod,
                             "sceUserServiceGetInitialUser");
        if (get_user) {
            u32 uid = 0;
            s32 rc = (s32)NC(c->G, get_user, (u64)&uid, 0,0,0,0,0);
            if (rc == 0 && uid != 0) c->user_id = (s32)uid;
        }
    }
    if (c->user_id == 0) c->user_id = 1;

    c->vid_open  = SYM(c->G, c->D, vid_mod, "sceVideoOutOpen");
    c->vid_close = SYM(c->G, c->D, vid_mod, "sceVideoOutClose");
    c->vid_reg   = SYM(c->G, c->D, vid_mod, "sceVideoOutRegisterBuffers");
    c->vid_flip  = SYM(c->G, c->D, vid_mod, "sceVideoOutSubmitFlip");
    c->vid_rate  = SYM(c->G, c->D, vid_mod, "sceVideoOutSetFlipRate");
    c->vid_evt   = SYM(c->G, c->D, vid_mod, "sceVideoOutAddFlipEvent");
    udp_log("DoomPS: [17] VideoOut fns\n");
    ext->step = 17;

    c->aud_open  = SYM(c->G, c->D, aud_mod, "sceAudioOutOpen");
    c->aud_out   = SYM(c->G, c->D, aud_mod, "sceAudioOutOutput");
    c->aud_close = SYM(c->G, c->D, aud_mod, "sceAudioOutClose");
    udp_log("DoomPS: [18] AudioOut fns\n");
    ext->step = 18;

    c->pad_init_fn = SYM(c->G, c->D, pad_mod, "scePadInit");
    c->pad_geth    = SYM(c->G, c->D, pad_mod, "scePadGetHandle");
    c->pad_read    = SYM(c->G, c->D, pad_mod, "scePadRead");
    udp_log("DoomPS: [19] Pad fns\n");
    ext->step = 19;

    if (c->cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(c->G, c->cancel, gs, 0,0,0,0,0);
    }
    NC(c->G, c->usleep_fn, 300000, 0,0,0,0,0);
    udp_log("DoomPS: [20] GS killed\n");
    ext->step = 20;

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (c->vid_close && emu_vid >= 0)
        NC(c->G, c->vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    udp_log("DoomPS: [21] old VO closed\n");
    ext->step = 21;

    c->video_h = (s32)NC(c->G, c->vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (c->video_h < 0) {
        ext->status = -10;
        udp_log("DoomPS: [22] VideoOut open FAILED\n");
        ext->step = 22;
        return;
    }
    udp_log("DoomPS: [23] VideoOut open OK\n");
    ext->step = 23;

    if (c->create_eq)
        NC(c->G, c->create_eq, (u64)&c->eq, (u64)"doomq",0,0,0,0);
    if (c->vid_evt && c->eq)
        NC(c->G, c->vid_evt, c->eq, (u64)c->video_h,0,0,0,0);
    udp_log("DoomPS: [24] equeue\n");
    ext->step = 24;

    u64 mem_total = c->dm_size
                  ? NC(c->G, c->dm_size, 0,0,0,0,0,0)
                  : 0x300000000ULL;
    u64 phys = 0;
    NC(c->G, c->alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    udp_log("DoomPS: [25] dmem alloc\n");
    ext->step = 25;

    c->vmem = 0;
    NC(c->G, c->map_dm, (u64)&c->vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!c->vmem) {
        ext->status = -21;
        udp_log("DoomPS: [26] dmem map FAILED\n");
        ext->step = 26;
        return;
    }
    udp_log("DoomPS: [27] dmem mapped\n");
    ext->step = 27;

    c->fbs[0] = c->vmem;
    c->fbs[1] = (u8 *)c->vmem + FB_ALIGNED;
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        ((u32 *)c->fbs[0])[i] = 0xFF000000;
        ((u32 *)c->fbs[1])[i] = 0xFF000000;
    }
    udp_log("DoomPS: [28] FBs cleared\n");
    ext->step = 28;

    u8 attr[64]; ps_memset(attr, 0, 64);
    *(u32*)(attr+0)  = 0x80000000;
    *(u32*)(attr+4)  = 1;
    *(u32*)(attr+12) = SCR_W;
    *(u32*)(attr+16) = SCR_H;
    *(u32*)(attr+20) = SCR_W;

    if (NC(c->G, c->vid_reg, (u64)c->video_h, 0, (u64)c->fbs, 2,
           (u64)attr, 0) != 0) {
        ext->status = -30;
        udp_log("DoomPS: [30] RegisterBuffers FAILED\n");
        ext->step = 30;
        return;
    }
    udp_log("DoomPS: [31] FBs registered\n");
    ext->step = 31;
    if (c->vid_rate)
        NC(c->G, c->vid_rate, (u64)c->video_h, 0,0,0,0,0);

    show_loading(c, 0, "Doom-PS starting up");
    udp_log("DoomPS: [32] first frame shown\n");
    ext->step = 32;

    if (c->aud_close)
        for (int h = 0; h < 8; h++)
            NC(c->G, c->aud_close, (u64)h,0,0,0,0,0);
    if (c->aud_open)
        c->audio_h = (s32)NC(c->G, c->aud_open, 0xFF, 0, 0,
                             SAMPLES_PER_BUF, SAMPLE_RATE,
                             AUDIO_S16_STEREO);
    if (c->mmap_fn) {
        c->ring = (u8 *)NC(c->G, c->mmap_fn, 0,
                           (u64)(RING_SLOTS * RING_BYTES), 3, 0x1002,
                           (u64)-1, 0);
        if ((s64)c->ring == -1) c->ring = 0;
    }
    udp_log(c->audio_h >= 0 ? "DoomPS: [33] audio up\n"
                            : "DoomPS: [33] audio N/A\n");
    ext->step = 33;

    if (c->pad_init_fn) NC(c->G, c->pad_init_fn, 0,0,0,0,0,0);
    if (c->pad_geth)
        c->pad_h = (s32)NC(c->G, c->pad_geth,
                           (u64)c->user_id, 0, 0, 0, 0, 0);
    {
        char b[80]; int p = 0;
        const char *m = "DoomPS: [34] pad_h="; while (*m) b[p++]=*m++;
        int v = c->pad_h;
        if (v < 0) { b[p++]='-'; v=-v; }
        if (v>=100) b[p++]='0'+(v/100)%10;
        if (v>=10)  b[p++]='0'+(v/10)%10;
        b[p++]='0'+v%10;
        b[p++]=' '; b[p++]='u'; b[p++]='i'; b[p++]='d'; b[p++]='=';
        v = c->user_id;
        if (v>=100) b[p++]='0'+(v/100)%10;
        if (v>=10)  b[p++]='0'+(v/10)%10;
        b[p++]='0'+v%10;
        b[p++]='\n'; b[p]=0;
        udp_log(b);
    }
    ext->step = 34;

    s32 tcp_listen_fd = (s32)ext->dbg[0];
    udp_log("DoomPS: [35] entering recv_wad\n");
    ext->step = 35;

    int wad_ok = (recv_wad(tcp_listen_fd) == 0);

    if (!wad_ok) {
        udp_log("DoomPS: WAD recv failed\n");
        show_error_and_hang(c, "WAD transfer failed",
                            "Check PC->console TCP connectivity");
    }

    {
        s32 check = (s32)NC(c->G, c->kopen, (u64)c->wad_path,
                            (u64)0x0000, 0, 0, 0, 0);
        if (check < 0) {
            udp_log("DoomPS: WAD verify failed\n");
            show_error_and_hang(c, "WAD missing after transfer",
                                c->wad_path);
        }
        NC(c->G, c->kclose, (u64)check, 0,0,0,0,0);
    }

    udp_log("DoomPS: [36] WAD phase complete\n");
    ext->step = 36;

    static const char arg0[] = "doom";
    static const char arg1[] = "-iwad";
    const char *argv[4];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = c->wad_path;
    argv[3] = (char *)0;

    udp_log("DoomPS: [37] doomgeneric_Create\n");
    ext->step = 37;
    doomgeneric_Create(3, (char **)argv);

    udp_log("DoomPS: [38] main loop\n");
    ext->step = 38;
    while (1) {
        doomgeneric_Tick();
        c->ext->frame_count = c->total_frames;
    }

    udp_log("DoomPS: exit\n");
    if (c->aud_close && c->audio_h >= 0)
        NC(c->G, c->aud_close, (u64)c->audio_h, 0,0,0,0,0);
    if (c->vid_close && c->video_h >= 0)
        NC(c->G, c->vid_close, (u64)c->video_h, 0,0,0,0,0);
    if (c->delete_eq && c->eq)
        NC(c->G, c->delete_eq, c->eq, 0,0,0,0,0);
    ext->status = 0;
    ext->step   = 99;
}
