/*
 * doom-ps/src/main.c — v44
 *
 * v44:
 *   - Reset key queue + pad_prev at the end of run_doom() so a held
 *     button from the previous Doom session doesn't ghost-press into
 *     the WAD menu (e.g. Circle-to-quit instantly selects the first
 *     WAD when the menu redraws).  Cosmetic — no crash fix.
 *
 * v43:
 *   - FIXED: WAD path was missing the trailing slash. "/av_contents/
 *     content_tmp/" is 25 characters (indices 0..24), but the code
 *     started writing the filename at index 24, overwriting the slash.
 *     The resulting path /av_contents/content_tmpDOOM.WAD failed to
 *     open with EACCES, so the launcher appeared "stuck at loading".
 *     Changed pi = 24 → pi = 25.
 */

#include "core.h"
#include "doomgeneric_ps.h"
#include "font.h"

extern char __bss_start[];
extern char __bss_end[];

extern void *malloc(unsigned long size);
extern void  free(void *p);
extern void  ps_libc_set_error_cb(void (*cb)(const char *msg));
extern void  I_SubmitSound(void);
extern int   mkdir(const char *path, unsigned int mode);

extern u32 *DG_ScreenBuffer;
extern void doomgeneric_Create(int argc, char **argv);
extern void doomgeneric_Tick(void);

#define DOOMFB_W  320
#define DOOMFB_H  200

#define O_WRONLY_  0x0001
#define O_RDWR_    0x0002
#define O_CREAT_   0x0200
#define O_TRUNC_   0x0400
#define O_WR_CREAT_TRUNC  (O_WRONLY_ | O_CREAT_ | O_TRUNC_)

#define MAX_WADS 32

typedef struct { u64 r_offset; u64 r_info; s64 r_addend; } Elf64_Rela;
#define ELF64_R_TYPE(i) ((u32)((i) & 0xffffffffU))
#define R_X86_64_RELATIVE 8

static int do_relocations(u64 load_base) {
    u64 rs, re;
    __asm__ volatile("lea __rela_start(%%rip), %0" : "=r"(rs));
    __asm__ volatile("lea __rela_end(%%rip), %0"   : "=r"(re));
    int count = 0;
    Elf64_Rela *r = (Elf64_Rela *)rs;
    Elf64_Rela *e = (Elf64_Rela *)re;
    while (r < e) {
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
            *(u64 *)(load_base + r->r_offset) = load_base + r->r_addend;
            count++;
        }
        r++;
    }
    return count;
}

static void ps_memset(void *dst, u8 val, u64 len) { u8 *d = (u8 *)dst; while (len--) *d++ = val; }
static void ps_memcpy(void *dst, const void *src, u64 len) { u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src; while (len--) *d++ = *s++; }
static u64 ps_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void ps_strcpy(char *d, const char *s) { while ((*d++ = *s++)); }

#define DS_SHARE     0x00000001
#define DS_L3        0x00000002
#define DS_R3        0x00000004
#define DS_OPTIONS   0x00000008
#define DS_UP        0x00000010
#define DS_RIGHT     0x00000020
#define DS_DOWN      0x00000040
#define DS_LEFT      0x00000080
#define DS_L2        0x00000100
#define DS_R2        0x00000200
#define DS_L1        0x00000400
#define DS_R1        0x00000800
#define DS_TRIANGLE  0x00001000
#define DS_CIRCLE    0x00002000
#define DS_CROSS     0x00004000
#define DS_SQUARE    0x00008000
#define DS_TOUCHPAD  0x00100000
#define DS_PAD_MASK  0x001FFFFF

#define DOOM_KEY_ESCAPE     0x1b
#define DOOM_KEY_ENTER      0x0d
#define DOOM_KEY_SPACE      0x20
#define DOOM_KEY_TAB        0x09
#define DOOM_KEY_LEFT       0xac
#define DOOM_KEY_UP         0xad
#define DOOM_KEY_RIGHT      0xae
#define DOOM_KEY_DOWN       0xaf
#define DOOM_KEY_LBRACKET   0x5b
#define DOOM_KEY_RBRACKET   0x5d
#define DOOM_KEY_COMMA      0x2c
#define DOOM_KEY_PERIOD     0x2e
#define DOOM_KEY_USE        0xa2
#define DOOM_KEY_FIRE       0xa3
#define DOOM_KEY_RCTRL      0x9d
#define DOOM_KEY_RSHIFT     0xb6
#define DOOM_KEY_F2         0xbc
#define DOOM_KEY_F3         0xbd
#define DOOM_KEY_Y          0x79
#define DOOM_KEY_N          0x6e

#define KEY_QUEUE_SIZE 32

struct wad_entry {
    char path[128];
    char name[64];
};

static struct wad_entry g_wads[MAX_WADS];
static int g_wad_count = 0;
static int g_wad_cursor = 0;
static int g_wad_scroll = 0;

static struct ps_ctx {
    void *G, *D;
    void *usleep_fn, *load_mod, *alloc_dm, *map_dm, *dm_size;
    void *create_eq, *wait_eq, *delete_eq, *mmap_fn;
    void *kopen, *kwrite, *kclose, *clock_gettime;
    void *accept_fn, *recv_fn, *close_fn, *sendto_fn, *setsockopt_fn, *cancel;
    void *unlink_fn;
    void *vid_open, *vid_close, *vid_reg, *vid_flip, *vid_rate, *vid_evt;
    s32   video_h;
    void *vmem;
    void *fbs[2];
    int   active_fb;
    u64   eq;
    u32   total_frames;
    void *aud_open, *aud_out, *aud_close;
    s32   audio_h;
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

static char g_diag[256];
static volatile int g_audio_thread_running = 0;

void *g_jmp_buf[8];
static int   g_exit_requested = 0;

__attribute__((naked)) static int ps_setjmp(void) {
    __asm__ volatile (
        "leaq g_jmp_buf(%rip), %rax\n\t"
        "movq %rbx, 0(%rax)\n\t"
        "movq %rbp, 8(%rax)\n\t"
        "movq %rsp, 16(%rax)\n\t"
        "movq %r12, 24(%rax)\n\t"
        "movq %r13, 32(%rax)\n\t"
        "movq %r14, 40(%rax)\n\t"
        "movq %r15, 48(%rax)\n\t"
        "movq (%rsp), %rcx\n\t"
        "movq %rcx, 56(%rax)\n\t"
        "xorl %eax, %eax\n\t"
        "retq"
    );
}

__attribute__((naked)) static void ps_longjmp(void) {
    __asm__ volatile (
        "leaq g_jmp_buf(%rip), %rax\n\t"
        "movq 0(%rax), %rbx\n\t"
        "movq 8(%rax), %rbp\n\t"
        "movq 16(%rax), %rsp\n\t"
        "addq $8, %rsp\n\t"
        "movq 24(%rax), %r12\n\t"
        "movq 32(%rax), %r13\n\t"
        "movq 40(%rax), %r14\n\t"
        "movq 48(%rax), %r15\n\t"
        "movq 56(%rax), %rcx\n\t"
        "movl $1, %eax\n\t"
        "jmpq *%rcx"
    );
}

void ps_doom_exit_now(void) {
    g_exit_requested = 1;
    ps_longjmp();
    for (;;) {}
}

static void udp_log(const char *msg) {
    struct ps_ctx *c = &g_ctx;
    if (c->log_fd < 0 || !c->sendto_fn) return;
    NC(c->G, c->sendto_fn, (u64)c->log_fd, (u64)msg, (u64)ps_strlen(msg),
       0, (u64)c->log_sa, 16);
}

void ps_sound_log(const char *msg) { udp_log(msg); }

static void diag_kopen(const char *prefix, const char *path, s32 fd) {
    int p = 0;
    const char *m = prefix;
    while (*m && p < 40) g_diag[p++] = *m++;
    int i = 0;
    while (path[i] && p < 110) g_diag[p++] = path[i++];
    g_diag[p++] = ' '; g_diag[p++] = 'f'; g_diag[p++] = 'd'; g_diag[p++] = '=';
    g_diag[p++] = '0'; g_diag[p++] = 'x';
    const char h[] = "0123456789ABCDEF";
    u32 v = (u32)fd;
    for (int k = 0; k < 8; k++) g_diag[p++] = h[(v >> (28 - k*4)) & 0xF];
    g_diag[p++] = '\n'; g_diag[p] = 0;
    udp_log(g_diag);
}

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

static void show_loading(struct ps_ctx *c, int dots, const char *status) {
    if (c->video_h < 0 || !c->fbs[c->active_fb]) return;
    u32 *fb = (u32 *)c->fbs[c->active_fb];
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF101018;
    ps_draw_str_center(fb, 280, "DOOM-PS", 0xFFFFAA00, 8);
    ps_draw_str_center(fb, 400, "doomgeneric on Luac0re", 0xFF808080, 3);
    ps_draw_str_center(fb, 445, "By MexrlDev", 0xFF909090, 3);
    char buf[32]; int p = 0;
    const char *base = "LOADING";
    while (base[p]) { buf[p] = base[p]; p++; }
    for (int i = 0; i < dots; i++) buf[p++] = '.';
    buf[p] = 0;
    ps_draw_str_center(fb, 540, buf, 0xFFFFFFFF, 5);
    if (status) ps_draw_str_center(fb, 700, status, 0xFFA0A0A0, 3);
    present(c);
}

static void show_wad_progress(struct ps_ctx *c, const char *name, u64 got, u64 total) {
    if (c->video_h < 0 || !c->fbs[c->active_fb]) return;
    u32 *fb = (u32 *)c->fbs[c->active_fb];
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF101018;
    ps_draw_str_center(fb, 240, "RECEIVING WAD", 0xFFFFAA00, 5);
    if (name) ps_draw_str_center(fb, 400, name, 0xFF8080FF, 4);
    int pct = (total > 0) ? (int)(got * 100 / total) : 0;
    char buf[40]; int p = 0;
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

static void show_error_and_hang(struct ps_ctx *c, const char *l1, const char *l2) {
    for (;;) {
        if (c->video_h < 0) continue;
        u32 *fb = (u32 *)c->fbs[c->active_fb];
        for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF200000;
        ps_draw_str_center(fb, 300, "DOOM-PS ERROR", 0xFFFF4040, 6);
        if (l1) ps_draw_str_center(fb, 500, l1, 0xFFFFFFFF, 4);
        if (l2) ps_draw_str_center(fb, 600, l2, 0xFFA0A0A0, 3);
        ps_draw_str_center(fb, 900, "Reboot game to recover", 0xFF808080, 3);
        present(c);
        if (c->usleep_fn) NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    }
}

static void ps_error_display(const char *msg) {
    struct ps_ctx *c = &g_ctx;
    if (c->video_h < 0) return;
    for (;;) {
        u32 *fb = (u32 *)c->fbs[c->active_fb];
        for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF200000;
        ps_draw_str_center(fb, 180, "DOOM INTERNAL ERROR", 0xFFFF4040, 6);
        if (msg && msg[0]) ps_draw_str_center(fb, 400, msg, 0xFFFFFFFF, 3);
        else ps_draw_str_center(fb, 400, "(no message)", 0xFFA0A0A0, 3);
        ps_draw_str_center(fb, 900, "Reboot game to recover", 0xFF808080, 3);
        present(c);
        if (c->usleep_fn) NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    }
}

static void blit_doom_frame(u32 *fb, const u32 *doom) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
    if (!doom) return;
    for (int dy = 0; dy < DOOM_H; dy++) {
        const u32 *row = doom + dy * DOOMFB_W;
        for (int dx = 0; dx < DOOM_W; dx++) {
            u32 out = row[dx] | 0xFF000000;
            int fy = OFF_Y + dy * SCALE_Y;
            int fx = OFF_X + dx * SCALE_X;
            for (int sy = 0; sy < SCALE_Y; sy++) {
                u32 *dst = fb + (fy + sy) * SCR_W + fx;
                for (int sx = 0; sx < SCALE_X; sx++) dst[sx] = out;
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
    u32 ch = raw ^ c->pad_prev;
    c->pad_prev = raw;
#define MAP(b,k) if (ch & (b)) push_key((k), (raw & (b)) ? 1 : 0)
    MAP(DS_UP, DOOM_KEY_UP); MAP(DS_DOWN, DOOM_KEY_DOWN);
    MAP(DS_LEFT, DOOM_KEY_LEFT); MAP(DS_RIGHT, DOOM_KEY_RIGHT);
    MAP(DS_CROSS, DOOM_KEY_FIRE); MAP(DS_SQUARE, DOOM_KEY_USE);
    MAP(DS_TRIANGLE, DOOM_KEY_TAB); MAP(DS_CIRCLE, DOOM_KEY_ENTER);
    if (ch & DS_CIRCLE) push_key(DOOM_KEY_Y, (raw & DS_CIRCLE) ? 1 : 0);
    if (ch & DS_CROSS)  push_key(DOOM_KEY_N, (raw & DS_CROSS)  ? 1 : 0);
    MAP(DS_OPTIONS, DOOM_KEY_ESCAPE);
    MAP(DS_R1, DOOM_KEY_F2); MAP(DS_L1, DOOM_KEY_F3);
    MAP(DS_R2, DOOM_KEY_RBRACKET); MAP(DS_L2, DOOM_KEY_LBRACKET);
    MAP(DS_L3, DOOM_KEY_COMMA); MAP(DS_R3, DOOM_KEY_PERIOD);
    MAP(DS_TOUCHPAD, DOOM_KEY_TAB);
#undef MAP
}

void dg_audio_callback(const short *pcm, int sample_count) {
    struct ps_ctx *c = &g_ctx;
    if (c->audio_h < 0 || !c->aud_out) return;
    (void)sample_count;
    NC(c->G, c->aud_out, (u64)c->audio_h, (u64)pcm, 0,0,0,0);
}

static void *audio_thread_fn(void *arg) {
    (void)arg;
    ps_sound_log("Audio: thread started\n");
    while (g_audio_thread_running) I_SubmitSound();
    ps_sound_log("Audio: thread exiting\n");
    return 0;
}

/* ============================================================
 * doomgeneric callbacks
 * ============================================================ */

void DG_Init(void) {
    udp_log("DoomPS: DG_Init entered\n");
    if (!DG_ScreenBuffer) udp_log("DoomPS: WARN DG_ScreenBuffer is NULL\n");
    else                  udp_log("DoomPS: DG_ScreenBuffer preserved\n");
}

void DG_DrawFrame(void) {
    static int draw_count = 0;
    draw_count++;
    struct ps_ctx *c = &g_ctx;
    if (draw_count == 1 || draw_count == 2 || draw_count == 3) {
        char b[64]; int p = 0;
        const char *m = "DoomPS: DG_DrawFrame #";
        while (*m) b[p++] = *m++;
        b[p++] = '0' + draw_count;
        b[p++] = '\n'; b[p] = 0;
        udp_log(b);
    } else if ((draw_count % 60) == 0) {
        char b[64]; int p = 0;
        const char *m = "DoomPS: DG_DrawFrame #";
        while (*m) b[p++] = *m++;
        int v = draw_count;
        char tmp[16]; int t = 0;
        while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t) b[p++] = tmp[--t];
        b[p++] = '\n'; b[p] = 0;
        udp_log(b);
    }
    blit_doom_frame((u32 *)c->fbs[c->active_fb], DG_ScreenBuffer);
    present(c);
    if (c->ext) c->ext->frame_count = c->total_frames;
    if (c->pad_h >= 0 && c->pad_read) {
        u8 pad_buf[128]; ps_memset(pad_buf, 0, 128);
        s32 n = (s32)NC(c->G, c->pad_read,
                        (u64)c->pad_h, (u64)pad_buf, 1, 0, 0, 0);
        if (n > 0 && (u32)n < 0x80000000) {
            u32 raw = *(u32 *)pad_buf;
            if (!(raw & 0x80000000)) translate_pad(raw & DS_PAD_MASK);
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

/* ============================================================
 * WAD management
 * ============================================================ */

static void delete_all_wads(void) {
    struct ps_ctx *c = &g_ctx;
    for (int i = 0; i < g_wad_count; i++) {
        if (c->unlink_fn)
            NC(c->G, c->unlink_fn, (u64)g_wads[i].path, 0,0,0,0,0);
    }
    g_wad_count = 0;
    g_wad_cursor = 0;
    g_wad_scroll = 0;
}

#define WAD_CHUNK 4096

static int recv_wads(s32 listen_fd) {
    struct ps_ctx *c = &g_ctx;
    if (listen_fd < 0) return -1;
    udp_log("DoomPS: waiting for WADs\n");

    delete_all_wads();
    show_loading(c, 0, "Waiting for WADs...");

    if (c->setsockopt_fn) {
        u8 tv[16] = {0};
        *(u64 *)(tv + 0) = 0;
        *(u64 *)(tv + 8) = 500000;
        (void)NC(c->G, c->setsockopt_fn,
                 (u64)listen_fd, 0xFFFF, 0x1006, (u64)tv, 16, 0);
    }

    for (;;) {
        if (g_wad_count >= MAX_WADS) break;

        s32 client = -1;
        for (int attempt = 0; attempt < 12; attempt++) {
            u8 peer[16]; s32 plen = 16;
            client = (s32)NC(c->G, c->accept_fn,
                             (u64)listen_fd, (u64)peer, (u64)&plen, 0,0,0);
            if (client >= 0) break;
            if (c->usleep_fn) NC(c->G, c->usleep_fn, 500000, 0,0,0,0,0);
        }
        if (client < 0) break;

        if (c->setsockopt_fn) {
            u8 tv[16] = {0};
            *(u64 *)(tv + 0) = 30;
            *(u64 *)(tv + 8) = 0;
            (void)NC(c->G, c->setsockopt_fn,
                     (u64)client, 0xFFFF, 0x1006, (u64)tv, 16, 0);
        }

        u8 hdr[10]; s32 got = 0;
        while (got < 10) {
            s32 n = (s32)NC(c->G, c->recv_fn, (u64)client,
                            (u64)(hdr + got), (u64)(10 - got), 0,0,0);
            if (n <= 0) { NC(c->G, c->close_fn, (u64)client,0,0,0,0,0); goto done; }
            got += n;
        }
        u64 wad_size = 0;
        for (int i = 0; i < 8; i++) wad_size |= ((u64)hdr[i] << (i * 8));
        u16 namelen = (u16)hdr[8] | ((u16)hdr[9] << 8);
        if (namelen > 63) namelen = 63;

        char name[64];
        got = 0;
        while (got < namelen) {
            s32 n = (s32)NC(c->G, c->recv_fn, (u64)client,
                            (u64)(name + got), (u64)(namelen - got), 0,0,0);
            if (n <= 0) { NC(c->G, c->close_fn, (u64)client,0,0,0,0,0); goto done; }
            got += n;
        }
        name[namelen] = 0;
        if (name[0] == 0) ps_strcpy(name, "UNKNOWN.WAD");

        /* Build "/av_contents/content_tmp/" + name.
         * The prefix is 25 characters (indices 0..24), the trailing
         * slash is at index 24, so the name starts at index 25. */
        char path[128];
        ps_strcpy(path, "/av_contents/content_tmp/");
        int pi = 25;
        for (int i = 0; name[i] && pi < 126; i++) path[pi++] = name[i];
        path[pi] = 0;

        s32 stale = (s32)NC(c->G, c->kopen, (u64)path, 0,0,0,0,0);
        if (stale >= 0) NC(c->G, c->kclose, (u64)stale, 0,0,0,0,0);

        s32 fd = (s32)NC(c->G, c->kopen, (u64)path,
                         (u64)O_WR_CREAT_TRUNC, 0x1FF, 0,0,0);
        diag_kopen("DoomPS: [W] ", path, fd);
        if (fd < 0) {
            NC(c->G, c->close_fn, (u64)client,0,0,0,0,0);
            continue;
        }

        u8 chunk[WAD_CHUNK];
        u64 remaining = wad_size;
        int last_pct = -1;
        show_wad_progress(c, name, 0, wad_size);
        int retries = 0;
        while (remaining > 0) {
            u64 want = remaining < WAD_CHUNK ? remaining : WAD_CHUNK;
            s32 n = (s32)NC(c->G, c->recv_fn, (u64)client,
                            (u64)chunk, (u64)want, 0,0,0);
            if (n > 0) {
                NC(c->G, c->kwrite, (u64)fd, (u64)chunk, (u64)n, 0,0,0);
                remaining -= (u64)n;
                retries = 0;
                int pct = (wad_size > 0)
                        ? (int)((wad_size - remaining) * 100 / wad_size) : 0;
                if (pct != last_pct) {
                    show_wad_progress(c, name, wad_size - remaining, wad_size);
                    last_pct = pct;
                }
            } else if (n == 0) break;
            else {
                retries++;
                if (retries > 30) break;
            }
        }
        NC(c->G, c->kclose, (u64)fd, 0,0,0,0,0);
        NC(c->G, c->close_fn, (u64)client,0,0,0,0,0);

        if (remaining > 0) {
            udp_log("DoomPS: WAD truncated, skipping\n");
            continue;
        }

        ps_strcpy(g_wads[g_wad_count].name, name);
        ps_strcpy(g_wads[g_wad_count].path, path);
        g_wad_count++;
        udp_log("DoomPS: got WAD\n");
    }

done:
    udp_log("DoomPS: no more WADs\n");
    show_loading(c, 3, "WADs ready");
    return g_wad_count;
}

#define MENU_VISIBLE 3

static int show_wad_menu(struct ps_ctx *c) {
    int prev_btn = 0;

    for (;;) {
        u32 raw = 0;
        if (c->pad_h >= 0 && c->pad_read) {
            u8 buf[128]; ps_memset(buf, 0, 128);
            s32 n = (s32)NC(c->G, c->pad_read,
                            (u64)c->pad_h, (u64)buf, 1, 0, 0, 0);
            if (n > 0 && (u32)n < 0x80000000) {
                u32 r = *(u32 *)buf;
                if (!(r & 0x80000000)) raw = r & DS_PAD_MASK;
            }
        }
        u32 ch = raw ^ (u32)prev_btn;
        prev_btn = raw;

        if (ch & DS_UP) {
            g_wad_cursor--;
            if (g_wad_cursor < 0) g_wad_cursor = g_wad_count - 1;
        }
        if (ch & DS_DOWN) {
            g_wad_cursor++;
            if (g_wad_cursor >= g_wad_count) g_wad_cursor = 0;
        }

        if (g_wad_cursor < g_wad_scroll) g_wad_scroll = g_wad_cursor;
        if (g_wad_cursor >= g_wad_scroll + MENU_VISIBLE)
            g_wad_scroll = g_wad_cursor - MENU_VISIBLE + 1;
        if (g_wad_scroll < 0) g_wad_scroll = 0;

        if (ch & DS_CIRCLE) return -1;
        if (ch & DS_CROSS)  return g_wad_cursor;

        u32 *fb = (u32 *)c->fbs[c->active_fb];
        for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF101018;

        ps_draw_str_center(fb, 60, "DOOM-PS", 0xFFFFAA00, 6);
        ps_draw_str_center(fb, 140, "SELECT WAD", 0xFF808080, 4);
        int list_y = 260;
        int row_h = 100;
        for (int i = 0; i < MENU_VISIBLE; i++) {
            int idx = g_wad_scroll + i;
            if (idx >= g_wad_count) break;
            int y = list_y + i * row_h;
            int sel = (idx == g_wad_cursor);
            if (sel) {
                ps_fill_rect(fb, 120, y - 20, SCR_W - 240, 80, 0xFFFFAA00);
                ps_draw_str_center(fb, y, g_wads[idx].name, 0xFF000000, 5);
            } else {
                ps_draw_str_center(fb, y, g_wads[idx].name, 0xFFFFFFFF, 5);
            }
        }

        char counter[24]; int cp = 0;
        if (g_wad_cursor + 1 >= 10) counter[cp++] = '0' + (g_wad_cursor + 1) / 10;
        counter[cp++] = '0' + (g_wad_cursor + 1) % 10;
        counter[cp++] = '/';
        if (g_wad_count >= 10) counter[cp++] = '0' + g_wad_count / 10;
        counter[cp++] = '0' + g_wad_count % 10;
        counter[cp] = 0;
        ps_draw_str_center(fb, SCR_H - 180, counter, 0xFF808080, 3);

        ps_draw_str_center(fb, SCR_H - 100, "X = SELECT   O = QUIT", 0xFF909090, 3);

        present(c);
        if (c->usleep_fn) NC(c->G, c->usleep_fn, 16000, 0,0,0,0,0);
    }
}

static void run_doom(struct ps_ctx *c, int wad_idx) {
    static const char arg0[] = "doom";
    static const char arg1[] = "-iwad";
    const char *argv[4];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = g_wads[wad_idx].path;
    argv[3] = (char *)0;

    ps_strcpy(c->wad_path, g_wads[wad_idx].path);
    udp_log("DoomPS: launching doom\n");

    if (ps_setjmp() == 0) {
        doomgeneric_Create(3, (char **)argv);
        while (1) {
            doomgeneric_Tick();
            c->ext->frame_count = c->total_frames;
            if (g_exit_requested) break;
        }
    }

    g_exit_requested = 0;
    g_audio_thread_running = 0;
    if (c->usleep_fn) NC(c->G, c->usleep_fn, 200000, 0,0,0,0,0);

    /* v44: wipe input state so a held Circle/Cross from the previous
     * Doom session doesn't ghost-press into the WAD menu. */
    c->key_wp   = 0;
    c->key_rp   = 0;
    c->pad_prev = 0;

    if (c->aud_close && c->audio_h >= 0)
        NC(c->G, c->aud_close, (u64)c->audio_h, 0,0,0,0,0);
    if (c->aud_open)
        c->audio_h = (s32)NC(c->G, c->aud_open, 0xFF, 0, 0,
                             SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);
    if (c->pad_init_fn) NC(c->G, c->pad_init_fn, 0,0,0,0,0,0);
    if (c->pad_geth)
        c->pad_h = (s32)NC(c->G, c->pad_geth,
                           (u64)c->user_id, 0, 0, 0, 0, 0);
}

__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    u64 load_base = (u64)&_start;
    do_relocations(load_base);
    { volatile char *p = __bss_start; while (p < __bss_end) *p++ = 0; }

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
    udp_log("DoomPS: [3] sendto\n"); ext->step = 3;

    extern void ps_libc_init(void *G, void *D, s32 log_fd, const u8 *log_sa);
    ps_libc_init(G, D, c->log_fd, c->log_sa);
    udp_log("DoomPS: [4] libc\n"); ext->step = 4;

    c->usleep_fn = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    ext->step = 5;
    c->cancel   = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    c->load_mod = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    ext->step = 6;
    c->alloc_dm = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    c->map_dm   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    c->dm_size  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    ext->step = 7;
    c->create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    c->wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    c->delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    ext->step = 8;
    c->mmap_fn       = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    c->kopen         = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    c->kwrite        = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    c->kclose        = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    c->clock_gettime = SYM(G, D, LIBKERNEL_HANDLE, "clock_gettime");
    c->unlink_fn     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUnlink");
    if (!c->unlink_fn) c->unlink_fn = SYM(G, D, LIBKERNEL_HANDLE, "unlink");
    ext->step = 9;
    c->accept_fn     = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    c->recv_fn       = SYM(G, D, LIBKERNEL_HANDLE, "recv");
    c->close_fn      = SYM(G, D, LIBKERNEL_HANDLE, "close");
    c->setsockopt_fn = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    udp_log("DoomPS: [10] sockets\n"); ext->step = 10;

    mkdir("./.savegame", 0777);
    mkdir("/av_contents/content_tmp/.savegame", 0777);
    mkdir("/savedata0/.savegame", 0777);
    udp_log("DoomPS: [10b] mkdir\n"); ext->step = 11;

    if (!c->usleep_fn || !c->load_mod) { ext->status = -1; return; }
    ext->step = 12;

    s32 vid_mod = (s32)NC(c->G, c->load_mod, (u64)"libSceVideoOut.sprx",0,0,0,0,0);
    s32 aud_mod = (s32)NC(c->G, c->load_mod, (u64)"libSceAudioOut.sprx",0,0,0,0,0);
    s32 pad_mod = (s32)NC(c->G, c->load_mod, (u64)"libScePad.sprx",0,0,0,0,0);
    s32 usr_mod = (s32)NC(c->G, c->load_mod, (u64)"libSceUserService.sprx",0,0,0,0,0);
    ext->step = 16;

    c->user_id = 0;
    if (usr_mod > 0) {
        void *get_user = SYM(c->G, c->D, usr_mod, "sceUserServiceGetInitialUser");
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
    c->aud_open  = SYM(c->G, c->D, aud_mod, "sceAudioOutOpen");
    c->aud_out   = SYM(c->G, c->D, aud_mod, "sceAudioOutOutput");
    c->aud_close = SYM(c->G, c->D, aud_mod, "sceAudioOutClose");
    c->pad_init_fn = SYM(c->G, c->D, pad_mod, "scePadInit");
    c->pad_geth    = SYM(c->G, c->D, pad_mod, "scePadGetHandle");
    c->pad_read    = SYM(c->G, c->D, pad_mod, "scePadRead");
    ext->step = 19;

    if (c->cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(c->G, c->cancel, gs, 0,0,0,0,0);
    }
    NC(c->G, c->usleep_fn, 300000, 0,0,0,0,0);
    ext->step = 20;

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (c->vid_close && emu_vid >= 0)
        NC(c->G, c->vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    ext->step = 21;

    c->video_h = (s32)NC(c->G, c->vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (c->video_h < 0) { ext->status = -10; ext->step = 22; return; }
    ext->step = 23;

    if (c->create_eq) NC(c->G, c->create_eq, (u64)&c->eq, (u64)"doomq",0,0,0,0);
    if (c->vid_evt && c->eq)
        NC(c->G, c->vid_evt, c->eq, (u64)c->video_h,0,0,0,0);
    ext->step = 24;

    u64 mem_total = c->dm_size ? NC(c->G, c->dm_size,0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(c->G, c->alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    ext->step = 25;
    c->vmem = 0;
    NC(c->G, c->map_dm, (u64)&c->vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!c->vmem) { ext->status = -21; ext->step = 26; return; }
    ext->step = 27;

    c->fbs[0] = c->vmem;
    c->fbs[1] = (u8 *)c->vmem + FB_ALIGNED;
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        ((u32 *)c->fbs[0])[i] = 0xFF000000;
        ((u32 *)c->fbs[1])[i] = 0xFF000000;
    }
    ext->step = 28;

    u8 attr[64]; ps_memset(attr, 0, 64);
    *(u32*)(attr+0) = 0x80000000; *(u32*)(attr+4) = 1;
    *(u32*)(attr+12) = SCR_W; *(u32*)(attr+16) = SCR_H; *(u32*)(attr+20) = SCR_W;
    if (NC(c->G, c->vid_reg, (u64)c->video_h, 0, (u64)c->fbs, 2, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30; return;
    }
    ext->step = 31;
    if (c->vid_rate) NC(c->G, c->vid_rate, (u64)c->video_h, 0,0,0,0,0);

    show_loading(c, 0, "Doom-PS starting up");
    ps_libc_set_error_cb(ps_error_display);
    ext->step = 33;

    if (c->aud_close)
        for (int h = 0; h < 8; h++)
            NC(c->G, c->aud_close, (u64)h,0,0,0,0,0);
    if (c->aud_open)
        c->audio_h = (s32)NC(c->G, c->aud_open, 0xFF, 0, 0,
                             SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);
    ext->step = 34;
    if (c->pad_init_fn) NC(c->G, c->pad_init_fn, 0,0,0,0,0,0);
    if (c->pad_geth)
        c->pad_h = (s32)NC(c->G, c->pad_geth, (u64)c->user_id, 0,0,0,0,0);
    ext->step = 35;

    s32 tcp_listen_fd = (s32)ext->dbg[0];
    ext->step = 36;

    recv_wads(tcp_listen_fd);
    ext->step = 37;

    if (g_wad_count == 0) {
        show_error_and_hang(c, "No WAD files received",
                            "Send .wad files from the PC launcher");
    }

    if (c->audio_h >= 0 && c->aud_out) {
        void *pthread_create = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCreate");
        if (pthread_create) {
            u64 th = 0;
            g_audio_thread_running = 1;
            NC(c->G, pthread_create, (u64)&th, 0,
               (u64)audio_thread_fn, 0, (u64)"doom_audio", 0);
        }
    }

    ext->step = 38;

    while (1) {
        int sel = show_wad_menu(c);
        if (sel < 0) break;
        run_doom(c, sel);
    }

    delete_all_wads();

    if (c->usleep_fn) NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    if (c->aud_close && c->audio_h >= 0)
        NC(c->G, c->aud_close, (u64)c->audio_h, 0,0,0,0,0);
    if (c->vid_close && c->video_h >= 0)
        NC(c->G, c->vid_close, (u64)c->video_h, 0,0,0,0,0);
    if (c->delete_eq && c->eq)
        NC(c->G, c->delete_eq, c->eq, 0,0,0,0,0);
    ext->status = 0;
    ext->step = 99;
    udp_log("DoomPS: cleanup done, returning to Lua\n");
}
