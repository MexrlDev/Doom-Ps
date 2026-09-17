/*
 * ps_libc.c — minimal libc replacement for doom-ps.
 *
 * v11: added putc (silent, same as fputc) and strtod (parse double).
 */

#include "core.h"
#include <stddef.h>
#include <stdarg.h>

typedef struct _ps_file FILE;

/* ============================================================
 * Error capture
 * ============================================================ */
static void (*__error_cb)(const char *msg) = 0;
static char __last_err[256];
static int  __last_err_len = 0;

void ps_libc_set_error_cb(void (*cb)(const char *msg)) {
    __error_cb = cb;
}

static void __capture_err(const char *s, int len) {
    int i = 0;
    __last_err_len = 0;
    while (i < len && __last_err_len < 255) {
        __last_err[__last_err_len++] = s[i++];
    }
    __last_err[__last_err_len] = 0;
}

/* ===== one-time init ===== */
static void *__G, *__D;
static void *fn_mmap, *fn_munmap;
static void *fn_kopen, *fn_kread, *fn_kwrite, *fn_kclose, *fn_klseek, *fn_kmkdir;
static void *fn_alloc_dm, *fn_map_dm, *fn_dm_size;
static void *fn_sendto;

static s32 __log_fd = -1;
static u8  __log_sa[16];
static int __log_ready = 0;

static void ps_libc_log(const char *msg) {
    if (!__log_ready || __log_fd < 0 || !fn_sendto) return;
    u64 n = 0;
    while (n < 512 && msg[n]) n++;
    if (n == 0) return;
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

/* ============================================================
 * vsnprintf engine
 * ============================================================ */
typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    int    ovf;
} _out;

static void _putc(_out *o, char c) {
    if (o->pos + 1 < o->cap) {
        o->buf[o->pos] = c;
    } else {
        o->ovf = 1;
    }
    o->pos++;
}

static void _puts(_out *o, const char *s) {
    if (!s) s = "(null)";
    while (*s) _putc(o, *s++);
}

static void _putu(_out *o, unsigned long long v, int base, int upper,
                  int min_width, char pad_with) {
    char tmp[32];
    int t = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = digits[v % base]; v /= base; }
    while (min_width > t) { _putc(o, pad_with); min_width--; }
    while (t) _putc(o, tmp[--t]);
}

static void _putd(_out *o, long long v, int min_width, int zero_pad) {
    if (v < 0) {
        _putc(o, '-');
        v = -v;
        if (min_width > 0) min_width--;
    }
    _putu(o, (unsigned long long)v, 10, 0, min_width, zero_pad ? '0' : ' ');
}

static void _putf(_out *o, double d) {
    if (d < 0) { _putc(o, '-'); d = -d; }
    unsigned long long ip = (unsigned long long)d;
    _putu(o, ip, 10, 0, 0, ' ');
    _putc(o, '.');
    double frac = d - (double)ip;
    for (int i = 0; i < 6; i++) {
        frac *= 10.0;
        int digit = (int)frac;
        _putc(o, '0' + digit);
        frac -= digit;
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    _out o = { buf, n, 0, 0 };
    if (!buf || n == 0) {
        o.buf = (char *)0;
        o.cap = 0;
    }
    if (!fmt) {
        if (n) buf[0] = 0;
        return 0;
    }

    while (*fmt) {
        if (*fmt != '%') {
            _putc(&o, *fmt++);
            continue;
        }
        fmt++;

        if (*fmt == '%') { _putc(&o, '%'); fmt++; continue; }

        int zero_pad = 0, left = 0;
        for (;;) {
            if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') { fmt++; }
            else break;
        }
        (void)left;

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        if (*fmt == '.') {
            fmt++;
            int precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
            if (precision > width) {
                width = precision;
                zero_pad = 1;
            }
        }

        int is_long = 0, is_ll = 0, is_short = 0;
        for (;;) {
            if (*fmt == 'l') {
                if (is_long) is_ll = 1;
                is_long = 1;
                fmt++;
            } else if (*fmt == 'h') { is_short = 1; fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { is_long = 1; fmt++; }
            else break;
        }
        (void)is_short;

        switch (*fmt) {
        case 'd': case 'i': {
            long long v;
            if (is_ll) v = va_arg(ap, long long);
            else if (is_long) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            _putd(&o, v, width, zero_pad);
            break;
        }
        case 'u': {
            unsigned long long v;
            if (is_ll) v = va_arg(ap, unsigned long long);
            else if (is_long) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
            _putu(&o, v, 10, 0, width, zero_pad ? '0' : ' ');
            break;
        }
        case 'x': {
            unsigned long long v;
            if (is_ll) v = va_arg(ap, unsigned long long);
            else if (is_long) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
            _putu(&o, v, 16, 0, width, zero_pad ? '0' : ' ');
            break;
        }
        case 'X': {
            unsigned long long v;
            if (is_ll) v = va_arg(ap, unsigned long long);
            else if (is_long) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
            _putu(&o, v, 16, 1, width, zero_pad ? '0' : ' ');
            break;
        }
        case 'o': {
            unsigned long long v;
            if (is_ll) v = va_arg(ap, unsigned long long);
            else if (is_long) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
            _putu(&o, v, 8, 0, width, zero_pad ? '0' : ' ');
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            _puts(&o, "0x");
            _putu(&o, (unsigned long long)p, 16, 0, 0, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            _puts(&o, s);
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            _putc(&o, (char)c);
            break;
        }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            double d = va_arg(ap, double);
            _putf(&o, d);
            break;
        }
        case 'n': {
            int *p = va_arg(ap, int *);
            if (p) *p = (int)o.pos;
            break;
        }
        case 0:
            goto done;
        default:
            _putc(&o, '%');
            _putc(&o, *fmt);
            break;
        }
        fmt++;
    }

done:
    if (n) {
        if (o.pos < n) buf[o.pos] = 0;
        else           buf[n - 1] = 0;
    }
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================================================
 * printf family
 * ============================================================ */
static int __printf_count = 0;

static void log_printf_output(const char *tag, const char *buf, int len) {
    if (__printf_count >= 200) return;
    __printf_count++;

    char out[560]; int p = 0;
    while (tag && *tag && p < 20) out[p++] = *tag++;
    out[p++] = ' ';
    int i = 0;
    while (i < len && i < 500 && p < 550) out[p++] = buf[i++];
    out[p++] = '\n'; out[p] = 0;
    ps_libc_log(out);
}

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    log_printf_output("[stdout]", buf, r);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf_output("[stdout]", buf, r);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f;
    char buf[512];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    log_printf_output("[stderr]", buf, r);
    __capture_err(buf, r);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf_output("[fprintf]", buf, r);
    return r;
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
    if (ret != 0) { log_hex("ps_libc: DMEM alloc ret=", (u64)(u32)ret); return 0; }
    void *vmem = 0;
    ret = (s32)NC(__G, fn_map_dm, (u64)&vmem, size, 3, 0, phys, 0x200000);
    if (ret != 0) { log_hex("ps_libc: DMEM map ret=", (u64)(u32)ret); return 0; }
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
        64ULL*1024*1024, 48ULL*1024*1024, 32ULL*1024*1024,
        24ULL*1024*1024, 16ULL*1024*1024, 0
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

static int __malloc_count = 0;

void *malloc(size_t size) {
    if (!__pool) {
        pool_init();
        if (!__pool) return 0;
    }
    if (__malloc_count < 64) {
        int n = __malloc_count++;
        char b[80]; int p = 0;
        const char *pre = "ps_libc: malloc #";
        while (*pre) b[p++] = *pre++;
        if (n >= 10) b[p++] = '0' + (n / 10) % 10;
        b[p++] = '0' + n % 10;
        b[p++] = ' '; b[p++] = 's'; b[p++] = 'i'; b[p++] = 'z'; b[p++] = 'e';
        b[p++] = '=';
        b[p++] = '0'; b[p++] = 'x';
        const char h[] = "0123456789ABCDEF";
        u32 v = (u32)size;
        for (int k = 0; k < 8; k++) b[p++] = h[(v >> (28 - k*4)) & 0xF];
        b[p++] = '\n'; b[p] = 0;
        ps_libc_log(b);
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

/* ============================================================
 * mem* — fast rep movsb / rep stosb
 * ============================================================ */
void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    if (n == 0) return ret;
    __asm__ volatile (
        "rep movsb"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory"
    );
    return ret;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

void *memset(void *d, int c, size_t n) {
    void *ret = d;
    if (n == 0) return ret;
    __asm__ volatile (
        "rep stosb"
        : "+D"(d), "+c"(n)
        : "a"((unsigned char)c)
        : "memory"
    );
    return ret;
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
char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++));
    return r;
}
char *strncat(char *d, const char *s, size_t n) {
    char *r = d;
    while (*d) d++;
    while (n-- && (*d = *s++)) d++;
    *d = 0;
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

static int __strdup_count = 0;

char *strdup(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (len < 4096 && s[len]) len++;
    if (len >= 4096) {
        ps_libc_log("ps_libc: strdup: NO NULL in 4096 bytes\n");
        return 0;
    }
    if (__strdup_count < 32) {
        __strdup_count++;
        char b[160]; int p = 0;
        const char *pre = "ps_libc: strdup(\"";
        while (*pre && p < 24) b[p++] = *pre++;
        for (size_t k = 0; k < len && p < 130; k++) b[p++] = s[k];
        b[p++] = '"'; b[p++] = ')'; b[p++] = '\n'; b[p] = 0;
        ps_libc_log(b);
    }
    char *p = (char *)malloc(len + 1);
    if (p) {
        for (size_t k = 0; k < len; k++) p[k] = s[k];
        p[len] = 0;
    }
    return p;
}

char *strtok(char *s, const char *delim) {
    static char *last = 0;
    if (s) last = s;
    if (!last) return 0;
    while (*last && strchr(delim, *last)) last++;
    if (!*last) { last = 0; return 0; }
    char *start = last;
    while (*last && !strchr(delim, *last)) last++;
    if (*last) { *last++ = 0; }
    else last = 0;
    return start;
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

/* --- v11 addition: strtod parses a double from a string --- */
double strtod(const char *s, char **endptr) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    double v = 0.0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        while (exp-- > 0) {
            if (eneg) v /= 10.0;
            else      v *= 10.0;
        }
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

long strtol(const char *s, char **endptr, int base) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (base == 0 || base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
        else if (base == 0) base = 10;
    }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtol(s, endptr, base);
}
long long atoll(const char *s) { return (long long)atoi(s); }

void qsort(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *)) {
    unsigned char *b = (unsigned char *)base;
    for (size_t i = 1; i < n; i++) {
        unsigned char tmp[64];
        if (sz > 64) return;
        for (size_t k = 0; k < sz; k++) tmp[k] = b[i*sz + k];
        size_t j = i;
        while (j > 0 && cmp(b + (j-1)*sz, tmp) > 0) {
            for (size_t k = 0; k < sz; k++) b[j*sz + k] = b[(j-1)*sz + k];
            j--;
        }
        for (size_t k = 0; k < sz; k++) b[j*sz + k] = tmp[k];
    }
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    const unsigned char *b = (const unsigned char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = cmp(key, b + mid * sz);
        if (r == 0) return (void *)(b + mid * sz);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

static int __errno_val = 0;
int *__errno_location(void) { return &__errno_val; }

/* ===== stdio ===== */
struct _ps_file {
    int fd;
    int writable;
    long pos;
    long size;
    unsigned char *buf;
};

static struct _ps_file __null_file = { -1, 0, 0, 0, 0 };
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

    if (__fopen_count < 32) {
        __fopen_count++;
        char b[240]; int p = 0;
        const char *pre = "ps_libc: fopen ENTRY path=\"";
        while (*pre && p < 40) b[p++] = *pre++;
        int i = 0;
        while (i < 150 && path && path[i] && p < 220) b[p++] = path[i++];
        b[p++] = '"';
        b[p++] = ' ';
        b[p++] = 'm';
        b[p++] = '=';
        b[p++] = (mode && mode[0]) ? mode[0] : '?';
        b[p++] = '\n'; b[p] = 0;
        ps_libc_log(b);
    }

    int writable = (mode && (mode[0] == 'w' || mode[0] == 'a')) ? 1 : 0;
    int flags = writable ? 0x601 : 0x0000;

    s32 fd = (s32)NC(__G, fn_kopen, (u64)path, flags, 0x1FF, 0, 0, 0);

    if (__fopen_count <= 32) {
        char b[80]; int p = 0;
        const char *pre = "ps_libc: fopen fd=0x";
        while (*pre && p < 24) b[p++] = *pre++;
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
int  ferror(FILE *f) { (void)f; return 0; }
void clearerr(FILE *f) { (void)f; }
int  fileno(FILE *f) { return f ? f->fd : -1; }

int remove(const char *path) { (void)path; return 0; }
int rename(const char *a, const char *b) { (void)a; (void)b; return 0; }

int mkdir(const char *path, unsigned int mode) {
    if (!fn_kmkdir) return -1;
    return (s32)NC(__G, fn_kmkdir, (u64)path, (u64)mode, 0,0,0,0);
}

/* ============================================================
 * sscanf — minimal parser
 * ============================================================ */
int vsscanf(const char *s, const char *fmt, va_list ap) {
    int matched = 0;
    if (!s || !fmt) return 0;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t') { fmt++; continue; }
        if (*fmt != '%') {
            if (*s == *fmt) { s++; fmt++; continue; }
            else return matched;
        }
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') fmt++;
        int is_long = 0;
        while (*fmt == 'l' || *fmt == 'h') { if (*fmt=='l') is_long = 1; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            while (*s == ' ' || *s == '\t') s++;
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            else if (*s == '+') s++;
            long v = 0; int any = 0;
            while (*s >= '0' && *s <= '9') { v = v*10 + (*s-'0'); s++; any = 1; }
            if (!any) return matched;
            if (is_long) *va_arg(ap, long *) = neg ? -v : v;
            else *va_arg(ap, int *) = neg ? -(int)v : (int)v;
            matched++;
            break;
        }
        case 'u': {
            while (*s == ' ' || *s == '\t') s++;
            unsigned long v = 0; int any = 0;
            while (*s >= '0' && *s <= '9') { v = v*10 + (*s-'0'); s++; any = 1; }
            if (!any) return matched;
            if (is_long) *va_arg(ap, unsigned long *) = v;
            else *va_arg(ap, unsigned *) = (unsigned)v;
            matched++;
            break;
        }
        case 'x': case 'X': {
            while (*s == ' ' || *s == '\t') s++;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            unsigned long v = 0; int any = 0;
            for (;;) {
                int d;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                else break;
                v = v*16 + d; s++; any = 1;
            }
            if (!any) return matched;
            if (is_long) *va_arg(ap, unsigned long *) = v;
            else *va_arg(ap, unsigned *) = (unsigned)v;
            matched++;
            break;
        }
        case 's': {
            while (*s == ' ' || *s == '\t') s++;
            char *out = va_arg(ap, char *);
            int any = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') { *out++ = *s++; any = 1; }
            *out = 0;
            if (!any) return matched;
            matched++;
            break;
        }
        case 'c': {
            char *out = va_arg(ap, char *);
            if (*s) { *out = *s++; matched++; }
            break;
        }
        case '%': {
            if (*s == '%') { s++; }
            else return matched;
            break;
        }
        default:
            return matched;
        }
        fmt++;
    }
    return matched;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
int __isoc99_sscanf(const char *s, const char *fmt, ...)
    __asm__("__isoc99_sscanf");
int __isoc99_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
int __isoc99_vsscanf(const char *s, const char *fmt, va_list ap)
    __asm__("__isoc99_vsscanf");
int __isoc99_vsscanf(const char *s, const char *fmt, va_list ap) {
    return vsscanf(s, fmt, ap);
}

/* ===== puts / putchar family ===== */
int puts(const char *s) {
    if (s) { ps_libc_log("[puts] "); ps_libc_log(s); ps_libc_log("\n"); }
    return 0;
}
int putchar(int c)               { return c; }
int putc(int c, FILE *f)         { (void)f; return c; }   /* v11 addition */
int fputc(int c, FILE *f)        { (void)f; return c; }
int fputs(const char *s, FILE *f){ (void)s; (void)f; return 0; }

int fgetc(FILE *f)               { (void)f; return -1; }
int getc(FILE *f)                { (void)f; return -1; }
int ungetc(int c, FILE *f)       { (void)f; return c; }
char *fgets(char *b, int n, FILE *f) { if (b && n) b[0]=0; (void)f; return 0; }

int atexit(void (*fn)(void)) { (void)fn; return 0; }
char *getenv(const char *name) { (void)name; return 0; }
void _exit(int code) { (void)code; for (;;) {} }

void exit(int code) {
    char b[60]; int p = 0;
    const char *pre = "ps_libc: *** exit(";
    while (*pre) b[p++] = *pre++;
    b[p++] = '0'; b[p++] = 'x';
    const char h[] = "0123456789ABCDEF";
    u32 v = (u32)code;
    for (int k = 0; k < 8; k++) b[p++] = h[(v >> (28 - k*4)) & 0xF];
    b[p++] = ')'; b[p++] = ' '; b[p++] = '*'; b[p++] = '*'; b[p++] = '*';
    b[p++] = '\n'; b[p] = 0;
    ps_libc_log(b);

    if (__error_cb && __last_err_len > 0) {
        __error_cb(__last_err);
    }
    for (;;) {}
}

int  system(const char *cmd) { (void)cmd; return -1; }
