/*
 * ps_libc.c — minimal libc replacement for doom-ps.
 *
 * doomgeneric calls a huge amount of libc.  Because we link with
 * -nostdlib -nostartfiles (required for Luac0re shellcode), we have
 * to provide every one of those symbols ourselves.
 *
 * Nothing here is fast or complete.  It exists to make doom link and
 * run on PS4/PS5.  File I/O is routed through libkernel's
 * sceKernelOpen/Read/Write/Close/Lseek, memory through mmap.
 */

#include "core.h"
#include <stddef.h>

/* ===== one-time init, called from _start ===== */
static void *__G, *__D;
static void *fn_mmap, *fn_munmap;
static void *fn_kopen, *fn_kread, *fn_kwrite, *fn_kclose, *fn_klseek, *fn_kmkdir;

void ps_libc_init(void *G, void *D) {
    __G = G; __D = D;
    fn_mmap   = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    fn_munmap = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    fn_kopen  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    fn_kread  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    fn_kwrite = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    fn_kclose = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    fn_klseek = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLseek");
    fn_kmkdir = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
}

/* ===== memory (bump allocator, 256 MB pool) ===== */
#define POOL_SIZE (256u * 1024u * 1024u)
static unsigned char *__pool = 0;
static size_t __pool_used = 0;

void *malloc(size_t size) {
    if (!__pool) {
        if (!fn_mmap) return 0;
        __pool = (unsigned char *)NC(__G, fn_mmap,
                    0, POOL_SIZE, 3, 0x1002, (u64)-1, 0);
        if ((long)__pool == -1) { __pool = 0; return 0; }
    }
    size = (size + 15) & ~(size_t)15;
    if (__pool_used + size > POOL_SIZE) return 0;
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

/* glibc's ctype internals — some code paths still reference these. */
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
    int fd;             /* >=0 for write mode; -1 for read mode (buffered) */
    int writable;
    long pos;
    long size;
    unsigned char *buf; /* read mode only */
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

FILE *fopen(const char *path, const char *mode) {
    if (!fn_kopen) return 0;
    int writable = (mode[0] == 'w' || mode[0] == 'a');
    /* O_RDONLY=0 ; O_WRONLY|O_CREAT|O_TRUNC = 0x601 (FreeBSD) */
    int flags = writable ? 0x601 : 0x0000;

    s32 fd = (s32)NC(__G, fn_kopen, (u64)path, flags, 0x1FF, 0, 0, 0);
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
        f->size = sz;
        if (sz > 0 && fn_mmap) {
            unsigned char *b = (unsigned char *)NC(__G, fn_mmap,
                0, (u64)sz, 3, 0x1002, (u64)-1, 0);
            if ((long)b == -1) { b = 0; }
            if (b) {
                long total = 0;
                while (total < sz) {
                    s32 n = (s32)NC(__G, fn_kread, (u64)fd,
                                    (u64)(b + total), (u64)(sz - total), 0,0,0);
                    if (n <= 0) break;
                    total += n;
                }
                f->buf = b;
                f->size = total;
            }
        }
        NC(__G, fn_kclose, (u64)fd, 0,0,0,0,0);
        f->fd = -1;
    }
    return f;
}

int fclose(FILE *f) {
    if (!f || f == &__null_file) return 0;
    if (f->buf && fn_munmap)
        NC(__G, fn_munmap, (u64)f->buf, (u64)f->size, 0, 0, 0, 0);
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

/* printf family — silent.  Doom uses them only for debug. */
int printf(const char *fmt, ...)                           { (void)fmt; return 0; }
int fprintf(FILE *f, const char *fmt, ...)                 { (void)f; (void)fmt; return 0; }
int vfprintf(FILE *f, const char *fmt, __builtin_va_list a) { (void)f; (void)fmt; (void)a; return 0; }
int sprintf(char *b, const char *fmt, ...)                 { if (b) b[0]=0; (void)fmt; return 0; }
int snprintf(char *b, size_t n, const char *fmt, ...)      { if (b && n) b[0]=0; (void)fmt; return 0; }
int vsnprintf(char *b, size_t n, const char *fmt, __builtin_va_list a)
                                                           { if (b && n) b[0]=0; (void)fmt; (void)a; return 0; }
int sscanf(const char *s, const char *fmt, ...)            { (void)s; (void)fmt; return 0; }

/* glibc redirects sscanf() → __isoc99_sscanf() at the header level
 * when _GNU_SOURCE / C99 is in play.  Provide the symbols so the
 * linker is satisfied.  Same for the v-variant just in case. */
int __isoc99_sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt;
    return 0;
}
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

/* ===== process ===== */
void exit(int code) { (void)code; for (;;) {} }
int  system(const char *cmd) { (void)cmd; return -1; }
