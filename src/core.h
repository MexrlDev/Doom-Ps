#ifndef CORE_H
#define CORE_H

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

/* ---- Offsets from eboot_base (Star Wars Racer Revenge PS2-Classic) ---- */
#define GADGET_OFFSET    0x31AA9
#define LIBKERNEL_HANDLE 0x2001
#define EBOOT_GS_THREAD  0x057F89B0
#define EBOOT_VIDOUT     0x02d695d0

/* ---- Framebuffer / screen ---- */
#define SCR_W       1920
#define SCR_H       1080
#define FB_SIZE     (SCR_W * SCR_H * 4)
#define FB_ALIGNED  ((FB_SIZE + 0x1FFFFF) & ~0x1FFFFF)
#define FB_TOTAL    (FB_ALIGNED * 2)

/* ---- Doom native resolution ---- */
#define DOOM_W  320
#define DOOM_H  200
/* Integer scale: 320*6=1920 (exact), 200*5=1000 (40px letterbox top+bot) */
#define SCALE_X 6
#define SCALE_Y 5
#define OFF_X   0
#define OFF_Y   ((SCR_H - DOOM_H * SCALE_Y) / 2)

/* ---- Audio ----
 *
 * SAMPLES_PER_BUF must match MIXBUF in i_sound_ps.c.  If they differ,
 * sceAudioOutOpen configures the hardware for one size but the mixer
 * submits buffers of a different size — the hardware plays only the
 * first half, so everything runs at 2× real time.  This was the cause
 * of the "insanely fast" music bug.
 */
#define SAMPLE_RATE      48000
#define SAMPLES_PER_BUF  1024
#define AUDIO_S16_STEREO 1
#define RING_SLOTS       8
#define RING_BYTES       (SAMPLES_PER_BUF * 4)

/* ---- ext_args — matches nes.lua / Luac0re layout exactly ---- */
struct ext_args {
    s64 status;       /* 0x00 */
    s64 step;         /* 0x08 */
    u32 frame_count;  /* 0x10 */
    s32 log_fd;       /* 0x14 */
    u8  log_addr[16]; /* 0x18 */
    u64 dbg[8];       /* 0x28 .. 0x68 */
};

/* ---- native_call / resolve_sym (identical to EmuC0re) ---- */
__attribute__((naked))
static u64 native_call(void *gadget, void *fn,
                       u64 a1, u64 a2, u64 a3,
                       u64 a4, u64 a5, u64 a6)
{
    __asm__ volatile (
        "pushq %%rbx\n\t"
        "movq %%rsi, %%rbx\n\t"
        "movq %%rdi, %%rax\n\t"
        "movq %%rdx, %%rdi\n\t"
        "movq %%rcx, %%rsi\n\t"
        "movq %%r8,  %%rdx\n\t"
        "movq %%r9,  %%rcx\n\t"
        "movq 16(%%rsp), %%r8\n\t"
        "movq 24(%%rsp), %%r9\n\t"
        "callq *%%rax\n\t"
        "popq %%rbx\n\t"
        "retq" ::: "memory"
    );
}

static void *resolve_sym(void *gadget, void *dlsym_fn, s32 handle, const char *name) {
    void *addr = 0;
    native_call(gadget, dlsym_fn, (u64)handle, (u64)name, (u64)&addr, 0, 0, 0);
    return addr;
}

#define NC  native_call
#define SYM resolve_sym

#endif /* CORE_H */
