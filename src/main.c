/*
 * doom-ps/src/main.c
 *
 * PS4/PS5 platform layer for doomgeneric, built on the EmuC0re pattern.
 *
 * DIAGNOSTIC BUILD: logs a UDP message after every step so we can see
 * exactly where the shellcode dies. Once the crash is localized, remove
 * the udp_log() calls between the SYMs — they add ~1 KB to the binary.
 */

#include "core.h"
#include "doomgeneric_ps.h"

/* ========================================================================
 * Minimal helpers (no libc in ffreestanding+nostdlib)
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
static u64 ps_strlen(const char *s) {
    u64 n = 0; while (s[n]) n++; return n;
}

/* ========================================================================
 * DualShock button bits
 * ======================================================================== */
#define DS_UP       0x00000010
#define DS_RIGHT    0x00000020
#define DS_DOWN     0x00000040
#define DS_LEFT     0x00000080
#define DS_L2       0x00000100
#define DS_R2       0x00000200
#define DS_L1       0x00000400
#define DS_R1       0x00000800
#define DS_TRIANGLE 0x00001000
#define DS_CIRCLE   0x00002000
#define DS_CROSS    0x00004000
#define DS_SQUARE   0x00008000
#define DS_OPTIONS  0x00000008
#define DS_SHARE    0x00000001
#define DS_L3       0x00000002
#define DS_R3       0x00000004

#define DG_KEY_RIGHTARROW  0xae
#define DG_KEY_LEFTARROW   0xac
#define DG_KEY_UPARROW     0xad
#define DG_KEY_DOWNARROW   0xaf
#define DG_KEY_FIRE        0xa0
#define DG_KEY_USE         0x20
#define DG_KEY_ESCAPE      27
#define DG_KEY_ENTER       13
#define DG_KEY_TAB         9
#define DG_KEY_SHIFT       0xa2
#define DG_KEY_ALT         0xa4
#define DG_KEY_F1          (0x80+0x3b)
#define DG_KEY_F2          (0x80+0x3c)
#define DG_KEY_1           '1'
#define DG_KEY_2           '2'
#define DG_KEY_3           '3'
#define DG_KEY_4           '4'
#define DG_KEY_5           '5'
#define DG_KEY_6           '6'
#define DG_KEY_7           '7'

/* ========================================================================
 * Global context
 * ======================================================================== */
#define KEY_QUEUE_SIZE 32

static struct ps_ctx {
    void *G, *D;

    void *usleep_fn;
    void *load_mod;
    void *alloc_dm;
    void *map_dm;
    void *dm_size;
    void *create_eq;
    void *wait_eq;
    void *delete_eq;
    void *mmap_fn;
    void *kopen;
    void *kwrite;
    void *kclose;
    void *clock_gettime;
    void *accept_fn;
    void *recv_fn;
    void *close_fn;
    void *sendto_fn;
    void *cancel;

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

    struct { u8 key; u8 pressed; } key_queue[KEY_QUEUE_SIZE];
    int   key_wp, key_rp;

    char  wad_path[128];

    s32   log_fd;
    u8    log_sa[16];

    struct ext_args *ext;
} g_ctx;

/* ========================================================================
 * UDP log — safe to call once sendto_fn and log_fd are set
 * ======================================================================== */
static void udp_log(const char *msg) {
    struct ps_ctx *c = &g_ctx;
    if (c->log_fd < 0 || !c->sendto_fn) return;
    NC(c->G, c->sendto_fn,
       (u64)c->log_fd, (u64)msg, (u64)ps_strlen(msg),
       0, (u64)c->log_sa, 16);
}

/* ========================================================================
 * Blit 320×200 RGBA → 1920×1080 BGRA
 * ======================================================================== */
static void blit_doom_frame(u32 *fb, const u32 *doom) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
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

/* ========================================================================
 * Key queue
 * ======================================================================== */
static void push_key(u8 key, u8 pressed) {
    struct ps_ctx *c = &g_ctx;
    int next = (c->key_wp + 1) & (KEY_QUEUE_SIZE - 1);
    if (next == c->key_rp) return;
    c->key_queue[c->key_wp].key     = key;
    c->key_queue[c->key_wp].pressed = pressed;
    c->key_wp = next;
}

static void translate_pad(u32 raw) {
    struct ps_ctx *c = &g_ctx;
    u32 changed = raw ^ c->pad_prev;
    c->pad_prev = raw;

#define MAP(ds_bit, doom_key) \
    if (changed & (ds_bit)) push_key((doom_key), (raw & (ds_bit)) ? 1 : 0)

    MAP(DS_UP,       DG_KEY_UPARROW);
    MAP(DS_DOWN,     DG_KEY_DOWNARROW);
    MAP(DS_LEFT,     DG_KEY_LEFTARROW);
    MAP(DS_RIGHT,    DG_KEY_RIGHTARROW);
    MAP(DS_CROSS,    DG_KEY_FIRE);
    MAP(DS_SQUARE,   DG_KEY_USE);
    MAP(DS_CIRCLE,   DG_KEY_ENTER);
    MAP(DS_OPTIONS,  DG_KEY_ESCAPE);
    MAP(DS_TRIANGLE, DG_KEY_TAB);
    MAP(DS_L1,       DG_KEY_SHIFT);
    MAP(DS_R1,       DG_KEY_ALT);
    MAP(DS_L2,       DG_KEY_F1);
    MAP(DS_R2,       DG_KEY_F2);
    if ((raw & DS_SHARE) && (changed & DS_CROSS))    push_key(DG_KEY_1, (raw & DS_CROSS)    ? 1 : 0);
    if ((raw & DS_SHARE) && (changed & DS_SQUARE))   push_key(DG_KEY_2, (raw & DS_SQUARE)   ? 1 : 0);
    if ((raw & DS_SHARE) && (changed & DS_CIRCLE))   push_key(DG_KEY_3, (raw & DS_CIRCLE)   ? 1 : 0);
    if ((raw & DS_SHARE) && (changed & DS_TRIANGLE)) push_key(DG_KEY_4, (raw & DS_TRIANGLE) ? 1 : 0);
    if ((raw & DS_L3)    && (changed & DS_CROSS))    push_key(DG_KEY_5, (raw & DS_CROSS)    ? 1 : 0);
    if ((raw & DS_L3)    && (changed & DS_SQUARE))   push_key(DG_KEY_6, (raw & DS_SQUARE)   ? 1 : 0);
    if ((raw & DS_L3)    && (changed & DS_CIRCLE))   push_key(DG_KEY_7, (raw & DS_CIRCLE)   ? 1 : 0);
#undef MAP
}

/* ========================================================================
 * Audio drain
 * ======================================================================== */
static void audio_drain(void) {
    struct ps_ctx *c = &g_ctx;
    if (c->audio_h < 0 || !c->aud_out || !c->ring) return;
    while (c->ring_count > 0) {
        u8 *slot = c->ring + c->ring_read * RING_BYTES;
        NC(c->G, c->aud_out, (u64)c->audio_h, (u64)slot, 0, 0, 0, 0);
        c->ring_read = (c->ring_read + 1) & (RING_SLOTS - 1);
        c->ring_count--;
    }
}

/* ========================================================================
 * WAD receive
 * ======================================================================== */
#define WAD_CHUNK 4096
static int recv_wad(s32 listen_fd) {
    struct ps_ctx *c = &g_ctx;
    if (listen_fd < 0) return -1;

    udp_log("DoomPS: waiting for WAD on TCP...\n");

    u8 peer[16]; s32 plen = 16;
    s32 client = (s32)NC(c->G, c->accept_fn,
                         (u64)listen_fd, (u64)peer, (u64)&plen, 0, 0, 0);
    if (client < 0) { udp_log("DoomPS: accept failed\n"); return -1; }

    u8 hdr[8]; s32 got = 0;
    while (got < 8) {
        s32 n = (s32)NC(c->G, c->recv_fn,
                        (u64)client, (u64)(hdr + got), (u64)(8 - got), 0, 0, 0);
        if (n <= 0) { NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0); return -1; }
        got += n;
    }
    u64 wad_size = 0;
    for (int i = 0; i < 8; i++) wad_size |= ((u64)hdr[i] << (i * 8));

    udp_log("DoomPS: receiving WAD...\n");

    const char *out = "/savedata0/doom.wad";
    s32 fd = (s32)NC(c->G, c->kopen, (u64)out, 0x601, 0x1FF, 0, 0, 0);
    if (fd < 0) {
        udp_log("DoomPS: cannot open output WAD\n");
        NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0);
        return -1;
    }

    u8 chunk[WAD_CHUNK];
    u64 remaining = wad_size;
    while (remaining > 0) {
        u64 want = remaining < WAD_CHUNK ? remaining : WAD_CHUNK;
        s32 n = (s32)NC(c->G, c->recv_fn,
                        (u64)client, (u64)chunk, (u64)want, 0, 0, 0);
        if (n <= 0) break;
        NC(c->G, c->kwrite, (u64)fd, (u64)chunk, (u64)n, 0, 0, 0);
        remaining -= (u64)n;
    }

    NC(c->G, c->kclose, (u64)fd, 0,0,0,0,0);
    NC(c->G, c->close_fn, (u64)client, 0,0,0,0,0);

    int i = 0;
    while (out[i] && i < 127) { g_ctx.wad_path[i] = out[i]; i++; }
    g_ctx.wad_path[i] = 0;

    udp_log("DoomPS: WAD written\n");
    return 0;
}

/* ========================================================================
 * doomgeneric callbacks
 * ======================================================================== */
extern u32 *DG_ScreenBuffer;

void DG_Init(void) { }

void DG_DrawFrame(void) {
    struct ps_ctx *c = &g_ctx;

    blit_doom_frame((u32 *)c->fbs[c->active_fb], DG_ScreenBuffer);

    NC(c->G, c->vid_flip,
       (u64)c->video_h, (u64)c->active_fb, 1, (u64)c->total_frames, 0, 0);

    if (c->eq && c->wait_eq) {
        u8 evt[64]; s32 cnt = 0;
        NC(c->G, c->wait_eq, c->eq, (u64)evt, 1, (u64)&cnt, 0, 0);
    }

    c->active_fb ^= 1;
    c->total_frames++;
    if (c->ext) c->ext->frame_count = c->total_frames;

    audio_drain();

    if (c->pad_h >= 0 && c->pad_read) {
        u8 pad_buf[128];
        ps_memset(pad_buf, 0, 128);
        s32 n = (s32)NC(c->G, c->pad_read,
                        (u64)c->pad_h, (u64)pad_buf, 1, 0, 0, 0);
        if (n > 0 && (u32)n < 0x80000000) {
            u32 raw = *(u32 *)pad_buf;
            if (!(raw & 0x80000000))
                translate_pad(raw & 0x001FFFFF);
        }
    }
}

void DG_SleepMs(unsigned int ms) {
    struct ps_ctx *c = &g_ctx;
    if (c->usleep_fn)
        NC(c->G, c->usleep_fn, (u64)ms * 1000ULL, 0, 0, 0, 0, 0);
}

unsigned int DG_GetTicksMs(void) {
    struct ps_ctx *c = &g_ctx;
    if (!c->clock_gettime) return c->total_frames * 16;
    u64 ts[2] = {0, 0};
    NC(c->G, c->clock_gettime, 4, (u64)ts, 0, 0, 0, 0);
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

void DG_SetWindowTitle(const char *title) { (void)title; }

void dg_audio_callback(const short *pcm, int sample_count) {
    struct ps_ctx *c = &g_ctx;
    if (!c->ring) return;
    int frames = sample_count, offset = 0;
    while (frames > 0 && c->ring_count < RING_SLOTS) {
        u8 *slot = c->ring + c->ring_write * RING_BYTES;
        int copy = frames < SAMPLES_PER_BUF ? frames : SAMPLES_PER_BUF;
        ps_memcpy(slot, pcm + offset * 2, (u64)(copy * 4));
        if (copy < SAMPLES_PER_BUF)
            ps_memset(slot + copy * 4, 0, (u64)((SAMPLES_PER_BUF - copy) * 4));
        c->ring_write = (c->ring_write + 1) & (RING_SLOTS - 1);
        c->ring_count++;
        offset += copy;
        frames -= copy;
    }
}

extern void doomgeneric_Create(int argc, char **argv);
extern void doomgeneric_Tick(void);

/* ========================================================================
 * _start — Luac0re entry point
 * ======================================================================== */
__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {

    /* ----- IMMEDIATE: mark step 1 before doing anything ----- */
    ext->step = 1;

    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;

    /* ----- Provide libc shims to doomgeneric FIRST ----- */
    extern void ps_libc_init(void *G, void *D);
    ps_libc_init(G, D);
    ext->step = 2;

    struct ps_ctx *c = &g_ctx;
    ps_memset(c, 0, sizeof(*c));
    c->video_h = -1;
    c->audio_h = -1;
    c->pad_h   = -1;
    c->log_fd  = -1;
    c->ext     = ext;

    c->G = G;
    c->D = D;

    /* Copy log socket details from ext IMMEDIATELY */
    c->log_fd = ext->log_fd;
    for (int i = 0; i < 16; i++) c->log_sa[i] = ext->log_addr[i];

    /* Resolve sendto FIRST so we can log everything after */
    c->sendto_fn = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sendto");
    ext->step = 3;
    udp_log("DoomPS: [3] sendto resolved\n");

    /* Args from Lua */
    s32 tcp_listen_fd = (s32)ext->dbg[0];
    s32 userId        = (s32)ext->dbg[2];
    (void)userId;

    /* ----- Resolve remaining libkernel symbols ----- */
    c->usleep_fn     = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    ext->step = 4; udp_log("DoomPS: [4] usleep resolved\n");

    c->cancel        = SYM(c->G, c->D, LIBKERNEL_HANDLE, "scePthreadCancel");
    c->load_mod      = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    ext->step = 5; udp_log("DoomPS: [5] cancel + load_mod resolved\n");

    c->alloc_dm      = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    c->map_dm        = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    c->dm_size       = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    ext->step = 6; udp_log("DoomPS: [6] dmem trio resolved\n");

    c->create_eq     = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    c->wait_eq       = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    c->delete_eq     = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    ext->step = 7; udp_log("DoomPS: [7] equeue trio resolved\n");

    c->mmap_fn       = SYM(c->G, c->D, LIBKERNEL_HANDLE, "mmap");
    c->kopen         = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelOpen");
    c->kwrite        = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelWrite");
    c->kclose        = SYM(c->G, c->D, LIBKERNEL_HANDLE, "sceKernelClose");
    c->clock_gettime = SYM(c->G, c->D, LIBKERNEL_HANDLE, "clock_gettime");
    ext->step = 8; udp_log("DoomPS: [8] file/clock resolved\n");

    c->accept_fn     = SYM(c->G, c->D, LIBKERNEL_HANDLE, "accept");
    c->recv_fn       = SYM(c->G, c->D, LIBKERNEL_HANDLE, "recv");
    c->close_fn      = SYM(c->G, c->D, LIBKERNEL_HANDLE, "close");
    ext->step = 9; udp_log("DoomPS: [9] socket fns resolved\n");

    if (!c->usleep_fn || !c->load_mod) {
        ext->status = -1; ext->step = 10; return;
    }
    ext->step = 11;
    udp_log("DoomPS: symbols OK\n");

    /* ----- Load sprx modules ----- */
    s32 vid_mod = (s32)NC(c->G, c->load_mod, (u64)"libSceVideoOut.sprx",  0,0,0,0,0);
    ext->step = 12; udp_log("DoomPS: [12] VideoOut.sprx loaded\n");

    s32 aud_mod = (s32)NC(c->G, c->load_mod, (u64)"libSceAudioOut.sprx",  0,0,0,0,0);
    ext->step = 13; udp_log("DoomPS: [13] AudioOut.sprx loaded\n");

    s32 pad_mod = (s32)NC(c->G, c->load_mod, (u64)"libScePad.sprx",       0,0,0,0,0);
    ext->step = 14; udp_log("DoomPS: [14] Pad.sprx loaded\n");

    NC(c->G, c->load_mod, (u64)"libSceUserService.sprx", 0,0,0,0,0);
    ext->step = 15; udp_log("DoomPS: [15] UserService.sprx loaded\n");

    c->vid_open  = SYM(c->G, c->D, vid_mod, "sceVideoOutOpen");
    c->vid_close = SYM(c->G, c->D, vid_mod, "sceVideoOutClose");
    c->vid_reg   = SYM(c->G, c->D, vid_mod, "sceVideoOutRegisterBuffers");
    c->vid_flip  = SYM(c->G, c->D, vid_mod, "sceVideoOutSubmitFlip");
    c->vid_rate  = SYM(c->G, c->D, vid_mod, "sceVideoOutSetFlipRate");
    c->vid_evt   = SYM(c->G, c->D, vid_mod, "sceVideoOutAddFlipEvent");
    ext->step = 16; udp_log("DoomPS: [16] VideoOut fns resolved\n");

    c->aud_open  = SYM(c->G, c->D, aud_mod, "sceAudioOutOpen");
    c->aud_out   = SYM(c->G, c->D, aud_mod, "sceAudioOutOutput");
    c->aud_close = SYM(c->G, c->D, aud_mod, "sceAudioOutClose");
    ext->step = 17; udp_log("DoomPS: [17] AudioOut fns resolved\n");

    c->pad_init_fn = SYM(c->G, c->D, pad_mod, "scePadInit");
    c->pad_geth    = SYM(c->G, c->D, pad_mod, "scePadGetHandle");
    c->pad_read    = SYM(c->G, c->D, pad_mod, "scePadRead");
    ext->step = 18; udp_log("DoomPS: [18] Pad fns resolved\n");

    /* ----- Kill existing game renderer ----- */
    ext->step = 19; udp_log("DoomPS: [19] killing GS thread\n");
    if (c->cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(c->G, c->cancel, gs, 0,0,0,0,0);
    }
    NC(c->G, c->usleep_fn, 300000, 0,0,0,0,0);
    ext->step = 20; udp_log("DoomPS: [20] GS thread cancelled\n");

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (c->vid_close && emu_vid >= 0)
        NC(c->G, c->vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(c->G, c->usleep_fn, 100000, 0,0,0,0,0);
    ext->step = 21; udp_log("DoomPS: [21] old VideoOut closed\n");

    /* ----- Open VideoOut ----- */
    c->video_h = (s32)NC(c->G, c->vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (c->video_h < 0) {
        ext->status = -10; ext->step = 22;
        udp_log("DoomPS: [22] VideoOut open FAILED\n");
        return;
    }
    ext->step = 23; udp_log("DoomPS: [23] VideoOut open OK\n");

    /* ----- Equeue ----- */
    if (c->create_eq)
        NC(c->G, c->create_eq, (u64)&c->eq, (u64)"doomq", 0,0,0,0);
    if (c->vid_evt && c->eq)
        NC(c->G, c->vid_evt, c->eq, (u64)c->video_h, 0,0,0,0);
    ext->step = 24; udp_log("DoomPS: [24] equeue created\n");

    /* ----- Direct memory for framebuffers ----- */
    u64 mem_total = c->dm_size
        ? NC(c->G, c->dm_size, 0,0,0,0,0,0)
        : 0x300000000ULL;
    u64 phys = 0;
    NC(c->G, c->alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    ext->step = 25; udp_log("DoomPS: [25] dmem allocated\n");

    c->vmem = 0;
    NC(c->G, c->map_dm, (u64)&c->vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!c->vmem) {
        ext->status = -21; ext->step = 26;
        udp_log("DoomPS: [26] dmem map FAILED\n");
        return;
    }
    ext->step = 27; udp_log("DoomPS: [27] dmem mapped\n");

    c->fbs[0] = c->vmem;
    c->fbs[1] = (u8 *)c->vmem + FB_ALIGNED;

    for (int i = 0; i < SCR_W * SCR_H; i++) {
        ((u32 *)c->fbs[0])[i] = 0xFF000000;
        ((u32 *)c->fbs[1])[i] = 0xFF000000;
    }
    ext->step = 28; udp_log("DoomPS: [28] FBs cleared\n");

    /* ----- Register framebuffers ----- */
    u8 attr[64];
    ps_memset(attr, 0, 64);
    *(u32 *)(attr +  0) = 0x80000000;
    *(u32 *)(attr +  4) = 1;
    *(u32 *)(attr + 12) = SCR_W;
    *(u32 *)(attr + 16) = SCR_H;
    *(u32 *)(attr + 20) = SCR_W;

    if (NC(c->G, c->vid_reg,
           (u64)c->video_h, 0, (u64)c->fbs, 2, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30;
        udp_log("DoomPS: [30] RegisterBuffers FAILED\n");
        return;
    }
    ext->step = 31; udp_log("DoomPS: [31] FBs registered\n");

    if (c->vid_rate) NC(c->G, c->vid_rate, (u64)c->video_h, 0, 0,0,0,0);

    /* ----- Audio ----- */
    if (c->aud_close)
        for (int h = 0; h < 8; h++) NC(c->G, c->aud_close, (u64)h, 0,0,0,0,0);
    if (c->aud_open)
        c->audio_h = (s32)NC(c->G, c->aud_open,
                              0xFF, 0, 0, SAMPLES_PER_BUF, SAMPLE_RATE,
                              AUDIO_S16_STEREO);
    ext->step = 32; udp_log("DoomPS: [32] audio opened\n");

    if (c->mmap_fn) {
        c->ring = (u8 *)NC(c->G, c->mmap_fn,
                           0, (u64)(RING_SLOTS * RING_BYTES), 3, 0x1002,
                           (u64)-1, 0);
        if ((s64)c->ring == -1) c->ring = 0;
    }
    ext->step = 33;
    udp_log(c->audio_h >= 0 ? "DoomPS: [33] audio up\n"
                            : "DoomPS: [33] audio N/A\n");

    /* ----- Pad ----- */
    if (c->pad_init_fn) NC(c->G, c->pad_init_fn, 0,0,0,0,0,0);
    if (c->pad_geth)
        c->pad_h = (s32)NC(c->G, c->pad_geth, 0, 0, 0, 0, 0, 0);
    ext->step = 34;
    udp_log(c->pad_h >= 0 ? "DoomPS: [34] pad up\n"
                          : "DoomPS: [34] pad N/A\n");

    /* ----- Receive WAD ----- */
    ext->step = 35; udp_log("DoomPS: [35] calling recv_wad\n");
    if (recv_wad(tcp_listen_fd) != 0) {
        const char *fb = "/savedata0/doom.wad";
        int i = 0;
        while (fb[i] && i < 127) { c->wad_path[i] = fb[i]; i++; }
        c->wad_path[i] = 0;
        udp_log("DoomPS: WAD recv failed, trying /savedata0/doom.wad\n");
    }
    ext->step = 36; udp_log("DoomPS: [36] WAD phase complete\n");

    /* ----- Build argv ----- */
    static const char arg0[] = "doom";
    static const char arg1[] = "-iwad";
    const char *argv[4];
    argv[0] = arg0;
    argv[1] = arg1;
    argv[2] = c->wad_path;
    argv[3] = (char *)0;

    ext->step = 37; udp_log("DoomPS: [37] calling doomgeneric_Create\n");
    doomgeneric_Create(3, (char **)argv);

    ext->step = 38; udp_log("DoomPS: [38] entering main loop\n");
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
