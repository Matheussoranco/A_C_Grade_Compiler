/*
 * runtime.c — AC language runtime support library.
 *
 * Provides helper functions callable from AC programs.
 * Compile: gcc -c runtime.c -o runtime.o
 * Link with generated object: gcc output.o runtime.o -o program
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * I/O helpers
 * ========================================================================= */
void ac_print_int(int64_t v)    { printf("%lld\n", (long long)v); }
void ac_print_uint(uint64_t v)  { printf("%llu\n", (unsigned long long)v); }
void ac_print_float(double v)   { printf("%g\n", v); }
void ac_print_bool(int v)       { puts(v ? "true" : "false"); }
void ac_print_char(int c)       { putchar(c); }
void ac_print_str(const char *s){ puts(s ? s : "(null)"); }
void ac_newline(void)           { putchar('\n'); }

int64_t  ac_read_int(void) {
    long long v = 0;
    int rc = scanf("%lld", &v);
    if (rc != 1) {
        /* EOF or invalid input: consume the rest of the line to avoid
         * infinite loops on repeated reads, then return 0. */
        int c;
        if (rc == 0) {
            while ((c = getchar()) != '\n' && c != EOF) { }
        }
        return 0;
    }
    return (int64_t)v;
}
double ac_read_float(void) {
    double v = 0.0;
    int rc = scanf("%lf", &v);
    if (rc != 1) {
        int c;
        if (rc == 0) {
            while ((c = getchar()) != '\n' && c != EOF) { }
        }
        return 0.0;
    }
    return v;
}

/* =========================================================================
 * Memory helpers (thin wrappers around libc for AC name-mangling)
 * ========================================================================= */
void *ac_malloc(uint64_t sz) { return malloc((size_t)sz); }
void  ac_free(void *p)       { free(p); }
void *ac_realloc(void *p, uint64_t sz) { return realloc(p, (size_t)sz); }
void *ac_memcpy(void *dst, const void *src, uint64_t n)
                              { return memcpy(dst, src, (size_t)n); }
void *ac_memset(void *dst, int c, uint64_t n)
                              { return memset(dst, c, (size_t)n); }

/* =========================================================================
 * Math helpers
 * ========================================================================= */
double ac_sqrt(double x)  { return sqrt(x); }
double ac_pow(double b, double e) { return pow(b, e); }
double ac_abs_f(double x) { return fabs(x); }
int64_t ac_abs_i(int64_t x){ return x < 0 ? -x : x; }
int64_t ac_min_i(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t ac_max_i(int64_t a, int64_t b) { return a > b ? a : b; }

/* =========================================================================
 * String helpers
 * ========================================================================= */
int64_t ac_strlen(const char *s) { return s ? (int64_t)strlen(s) : 0; }
int     ac_strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    return strcmp(a, b);
}
/* Bounded strcat: the AC compiler cannot know dst capacity, so the runtime
 * enforces a sane cap (64 KiB) and truncates instead of overflowing.
 * Returns dst (or NULL on NULL dst). */
#define AC_STRCAT_CAP (64u * 1024u)
char   *ac_strcat(char *dst, const char *src) {
    if (!dst) return NULL;
    if (!src) return dst;
    size_t dlen = strnlen(dst, AC_STRCAT_CAP);
    size_t slen = strlen(src);
    if (dlen >= AC_STRCAT_CAP) return dst; /* no room: leave unchanged */
    size_t room = AC_STRCAT_CAP - 1 - dlen;
    size_t copy = slen < room ? slen : room;
    memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = '\0';
    return dst;
}

/* =========================================================================
 * Program control
 * ========================================================================= */
void ac_exit(int64_t code) { exit((int)code); }
