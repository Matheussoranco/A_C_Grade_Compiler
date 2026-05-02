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
    int64_t v = 0;
    scanf("%lld", &v);
    return v;
}
double ac_read_float(void) {
    double v = 0.0;
    scanf("%lf", &v);
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
int64_t ac_strlen(const char *s) { return (int64_t)strlen(s); }
int     ac_strcmp(const char *a, const char *b) { return strcmp(a, b); }
char   *ac_strcat(char *dst, const char *src)   { return strcat(dst, src); }

/* =========================================================================
 * Program control
 * ========================================================================= */
void ac_exit(int64_t code) { exit((int)code); }
