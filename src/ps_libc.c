/*
 * ps_libc.c — minimal libc replacement for doom-ps.
 *
 * v3: DMEM pool with size cascade + diagnostic logging.
 *     Every pool attempt and the first malloc/fopen are logged to UDP.
 */

#include "core.h"
#include <stddef.h>

/* ===== one-time init ===== */
static void *__G, *__D;
static void *fn_mmap, *fn_munmap;
static void *fn_kopen, *fn_kread, *fn_kwrite, *fn_kclose, *fn_klseek, *fn_kmkdir;
static void *fn_alloc_dm, *fn_map_dm, *fn_dm_size;
static void *fn_sendto;

/* UDP log plumbing */
static s32 __log_fd = -1;
static u8  __log_sa[16];
static int __log_ready = 0;

static void ps_libc_log(const char *msg) {
    if (!__log_ready || __log_fd < 0 || !fn_sendto) return;
    u64 n = 0;
    while (msg[n]) n++;
    NC(__G, fn_sendto, (u64)__log_fd, (u64)msg, n, 0, (u64)__log_sa, 16);
}

void ps_libc_init(void *G, void *D, s32 log_fd, const u8 *log_sa) {
    __G = G; __D = D;
    fn_mmap     = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    fn_munmap   = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    fn_kopen    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    fn_kread    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    fn_kwrite   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    fn_kclose   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    fn_klseek   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLseek");
    fn_kmkdir   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
    fn_alloc_dm = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    fn_map_dm   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    fn_dm_size  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    fn_sendto   = SYM(G, D, LIBKERNEL_HANDLE, "sendto");

    __log_fd = log_fd;
    for (int i = 0; i < 16; i++) __log_sa[i] = log_sa[i];
    __log_ready = 1;
}

/* ===== memory pool ===== */
static unsigned char *__pool = 0;
static size_t __pool_size = 0;
static size_t __pool_used = 0;

static void log_hex(const char *prefix, u64 v) {
    char b[80]; int p = 0;
    while (*prefix && p < 60) b[p++] = *prefix++;
    b[p++] = '0'; b[p++] = 'x';
    const char h[] = "0123456789ABCDEF";
    for (int k = 0; k < 16; k++)
        b[p++] = h[(v >> ((15 - k) * 4)) & 0xF];
    b[p++] = '\n'; b[p] = 0;
    ps_libc_log(b);
}

static void *try_dmem_pool(u64 size) {
    if (!fn_alloc_dm || !fn_map_dm) return 0;
    u64 total = fn_dm_size ? NC(__G, fn_dm_size, 0,0,0,0,0,0)
                          : 0x300000000ULL;
    u64 phys = 0;
    s32 ret = (s32)NC(__G, fn_alloc_dm,
                      0, total, size, 0x200000, 3, (u64)&phys);
    if (ret != 0) {
        log_hex("ps_libc: DMEM alloc ret=", (u64)(u32)ret);
        return 0;
    }
    void *vmem = 0;
    ret = (s32)NC(__G, fn_map_dm,
                  (u64)&vmem, size, 3, 0, phys, 0x200000);
    if (ret != 0) {
        log_hex("ps_libc: DMEM map ret=", (u64)(u32)ret);
        return 0;
    }
    if (!vmem || (u64)vmem >= 0x8000000000000000ULL) {
        log_hex("ps_libc: DMEM vmem=", (u64)vmem);
        return 0;
    }
    log_hex("ps_libc: DMEM pool at ", (u64)vmem);
    return vmem;
}

static void *try_mmap_pool(u64 size) {
    if (!fn_mmap) return 0;
    void *p = (void *)NC(__G, fn_mmap, 0, size, 3, 0x1002, (u64)-1, 0);
    if (!p || (u64)p == (u64)-1 || (u64)p >= 0x8000000000000000ULL) {
        log_hex("ps_libc: mmap fail p=", (u64)p);
        return 0;
    }
    log_hex("ps_libc: mmap pool at ", (u64)p);
    return p;
}

static void pool_init(void) {
    static const u64 sizes[] = {
        64ULL*1024*1024,
        48ULL*1024*1024,
        32ULL*1024*1024,
        24ULL*1024*1024,
        16ULL*1024*1024,
        0
    };
    ps_libc_log("ps_libc: pool_init start\n");
    for (int i = 0; sizes[i]; i++) {
        log_hex("ps_libc: trying DMEM size ", sizes[i]);
        __pool = (unsigned char *)try_dmem_pool(sizes[i]);
        if (__pool) { __pool_size = sizes[i]; return; }
    }
    for (int i = 0; sizes[i]; i++) {
        log_hex("ps_libc: trying mmap size ", sizes[i]);
        __pool = (unsigned char *)try_mmap_pool(sizes[i]);
        if (__pool) { __pool_size = sizes[i]; return; }
    }
    ps_libc_log("ps_libc: POOL_INIT FAILED - no memory\n");
}

static int __first_malloc_logged = 0;

void *malloc(size_t size) {
    if (!__pool) {
        pool_init();
        if (!__pool) return 0;
    }
    if (!__first_malloc_logged) {
        __first_malloc_logged = 1;
        log_hex("ps_libc: first malloc size=", (u64)size);
        log_hex("ps_libc: pool size=", (u64)__pool_size);
    }
    size = (size + 15) & ~(size_t)15;
    if (__pool_used + size > __pool_size) {
        log_hex("ps_libc: OOM req=", (u64)size);
        log_hex("ps_libc: OOM used=", (u64)__pool_used);
        return 0;
    }
    void *p = __pool + __pool_used;
    __pool_used += size;
    return p;
}

void *calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = malloc(total);
    if (!p) return 0;
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}

void *realloc(void *p, size_t size) {
    if (!p) return malloc(size);
    if (!size) return p;
    void *np = malloc(size);
    if (!np) return 0;
    unsigned char *d = (unsigned char *)np;
    unsigned char *s = (unsigned char *)p;
    for (size_t i = 0; i < size; i++) d[i] = s[i];
    return np;
}

void free(void *p) { (void)p; }

/* ===== mem* ===== */
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

/* ===== string ===== */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
static int __lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && __lc((unsigned char)*a) == __lc((unsigned char)*b)) { a++; b++; }
    return __lc((unsigned char)*a) - __lc((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && __lc((unsigned char)*a) == __lc((unsigned char)*b)) { a++; b++; n--; }
    return n ? __lc((unsigned char)*a) - __lc((unsigned char)*b) : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (char)c == 0 ? (char *)s : 0;
}
char *strrchr(const char *s, int c) {
    const char *last = 0;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if ((char)c == 0) return (char *)s;
    return (char *)last;
}
char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) strcpy(p, s);
    return p;
}

/* ===== ctype ===== */
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c>='a'&&c<='f') || (c>='A'&&c<='F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isblank(int c) { return c == ' ' || c == '\t'; }

static const unsigned short __ctype_tbl[384]   = {0};
static const unsigned short *__ctype_tbl_p     = __ctype_tbl + 128;
static const int __toupper_tbl[384]            = {0};
static const int *__toupper_tbl_p              = __toupper_tbl + 128;
static const int __tolower_tbl[384]            = {0};
static const int *__tolower_tbl_p              = __tolower_tbl + 128;

const unsigned short **__ctype_b_loc(void)       { return &__ctype_tbl_p; }
const int **__ctype_toupper_loc(void)            { return &__toupper_tbl_p; }
const int **__ctype_tolower_loc(void)            { return &__tolower_tbl_p; }

/* ===== numeric ===== */
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
int atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
double atof(const char *s) { return (double)atoi(s); }
double fabs(double x) { return x < 0 ? -x : x; }

/* ===== errno ===== */
static int __errno_val = 0;
int *__errno_location(void) { return &__errno_val; }

/* ===== stdio ===== */
typedef struct _ps_file FILE;
struct _ps_file {
    int fd;
    int writable;
    long pos;
    long size;
    unsigned char *buf;
};

static FILE __null_file = { -1, 0, 0, 0, 0 };
FILE *stderr = &__null_file;
FILE *stdout = &__null_file;
FILE *stdin  = &__null_file;

#define MAX_FILES 64
static struct _ps_file __files[MAX_FILES];

static FILE *alloc_slot(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (__files[i].fd == 0 && __files[i].buf == 0 && &__files[i] != stderr)
            return &__files[i];
    }
    return 0;
}

static int __fopen_count = 0;

FILE *fopen(const char *path, const char *mode) {
    if (!fn_kopen) return 0;
    int writable = (mode[0] == 'w' || mode[0] == 'a');
    int flags = writable ? 0x601 : 0x0000;

    s32 fd = (s32)NC(__G, fn_kopen, (u64)path, flags, 0x1FF, 0, 0, 0);

    if (__fopen_count < 12) {
        __fopen_count++;
        char b[200]; int p = 0;
        const char *pre = "ps_libc: fopen ";
        while (*pre && p < 40) b[p++] = *pre++;
        int i = 0;
        while (path[i] && p < 130) b[p++] = path[i++];
        b[p++] = ' '; b[p++] = 'm'; b[p++] = '='; b[p++] = mode[0];
        b[p++] = ' '; b[p++] = 'f'; b[p++] = 'd'; b[p++] = '=';
        b[p++] = '0'; b[p++] = 'x';
        const char h[] = "0123456789ABCDEF";
        u32 v = (u32)fd;
        for (int k = 0; k < 8; k++) b[p++] = h[(v >> (28 - k*4)) & 0xF];
        b[p++] = '\n'; b[p] = 0;
        ps_libc_log(b);
    }

    if (fd < 0) return 0;

    FILE *f = alloc_slot();
    if (!f) { NC(__G, fn_kclose, (u64)fd, 0,0,0,0,0); return 0; }

    f->fd = fd;
    f->writable = writable;
    f->pos = 0;
    f->size = 0;
    f->buf = 0;

    if (!writable) {
        long sz = (long)NC(__G, fn_klseek, (u64)fd, 0, 2, 0,0,0);
        NC(__G, fn_klseek, (u64)fd, 0, 0, 0,0,0);
        if (sz < 0) sz = 0;
        if (sz > 0) {
            unsigned char *b = (unsigned char *)malloc((size_t)sz);
            if (b) {
                long total = 0;
                while (total < sz) {
                    s32 n = (s32)NC(__G, fn_kread, (u64)fd,
                                    (u64)(b + total),
                                    (u64)(sz - total), 0,0,0);
                    if (n <= 0) break;
                    total += n;
                }
                f->buf = b;
                f->size = total;
            } else {
                ps_libc_log("ps_libc: fopen READ malloc FAILED\n");
            }
        }
        NC(__G, fn_kclose, (u64)fd, 0,0,0,0,0);
        f->fd = -1;
    }
    return f;
}

int fclose(FILE *f) {
    if (!f || f == &__null_file) return 0;
    if (f->fd >= 0 && fn_kclose)
        NC(__G, fn_kclose, (u64)f->fd, 0,0,0,0,0);
    f->fd = 0; f->buf = 0; f->pos = 0; f->size = 0;
    return 0;
}

size_t fread(void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f || !f->buf || sz == 0) return 0;
    size_t want = sz * n;
    long avail = f->size - f->pos;
    if (avail <= 0) return 0;
    if ((long)want > avail) want = (size_t)avail;
    unsigned char *d = (unsigned char *)ptr;
    unsigned char *s = f->buf + f->pos;
    for (size_t i = 0; i < want; i++) d[i] = s[i];
    f->pos += (long)want;
    return want / sz;
}

size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f || f->fd < 0 || sz == 0) return 0;
    size_t want = sz * n;
    s32 w = (s32)NC(__G, fn_kwrite, (u64)f->fd, (u64)ptr, (u64)want, 0,0,0);
    if (w <= 0) return 0;
    f->pos += w;
    return (size_t)w / sz;
}

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    if (whence == 0)      f->pos = off;
    else if (whence == 1) f->pos += off;
    else if (whence == 2) f->pos = f->size + off;
    return 0;
}

long ftell(FILE *f)  { return f ? f->pos : -1; }
int  fflush(FILE *f) { (void)f; return 0; }
int  feof(FILE *f)   { return f ? (f->pos >= f->size) : 1; }

int remove(const char *path) { (void)path; return 0; }
int rename(const char *a, const char *b) { (void)a; (void)b; return 0; }

int mkdir(const char *path, unsigned int mode) {
    if (!fn_kmkdir) return -1;
    return (s32)NC(__G, fn_kmkdir, (u64)path, (u64)mode, 0,0,0,0);
}

int printf(const char *fmt, ...)                           { (void)fmt; return 0; }
int fprintf(FILE *f, const char *fmt, ...)                 { (void)f; (void)fmt; return 0; }
int vfprintf(FILE *f, const char *fmt, __builtin_va_list a) { (void)f; (void)fmt; (void)a; return 0; }
int sprintf(char *b, const char *fmt, ...)                 { if (b) b[0]=0; (void)fmt; return 0; }
int snprintf(char *b, size_t n, const char *fmt, ...)      { if (b && n) b[0]=0; (void)fmt; return 0; }
int vsnprintf(char *b, size_t n, const char *fmt, __builtin_va_list a)
                                                           { if (b && n) b[0]=0; (void)fmt; (void)a; return 0; }

int sscanf(const char *s, const char *fmt, ...) { (void)s; (void)fmt; return 0; }

int __isoc99_sscanf(const char *s, const char *fmt, ...)
    __asm__("__isoc99_sscanf");
int __isoc99_sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt;
    return 0;
}
int __isoc99_vsscanf(const char *s, const char *fmt, __builtin_va_list a)
    __asm__("__isoc99_vsscanf");
int __isoc99_vsscanf(const char *s, const char *fmt, __builtin_va_list a) {
    (void)s; (void)fmt; (void)a;
    return 0;
}

int puts(const char *s)          { (void)s; return 0; }
int putchar(int c)               { return c; }
int fputs(const char *s, FILE *f){ (void)s; (void)f; return 0; }
int fputc(int c, FILE *f)        { (void)f; return c; }
int fgetc(FILE *f)               { (void)f; return -1; }
int getc(FILE *f)                { (void)f; return -1; }
int ungetc(int c, FILE *f)       { (void)f; return c; }
char *fgets(char *b, int n, FILE *f) { if (b && n) b[0]=0; (void)f; return 0; }

void exit(int code) { (void)code; for (;;) {} }
int  system(const char *cmd) { (void)cmd; return -1; }
