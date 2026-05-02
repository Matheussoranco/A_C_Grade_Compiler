/*
 * common.h — Shared primitives, diagnostics, arena allocator, data structures.
 * Every compilation unit includes this header.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>

/* =========================================================================
 * Compiler version
 * ========================================================================= */
#define AC_VERSION        "1.0.0"
#define AC_LANG_NAME      "AC"
#define AC_FILE_EXT       ".ac"
#define AC_TARGET_TRIPLE  "x86_64-linux-gnu"

/* =========================================================================
 * Type aliases
 * ========================================================================= */
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;
typedef size_t   usize;
typedef ptrdiff_t isize;

/* =========================================================================
 * Source location
 * ========================================================================= */
typedef struct {
    const char *file;
    i32         line;
    i32         col;
} SrcLoc;

#define SRCLOC_NONE ((SrcLoc){NULL, 0, 0})

/* =========================================================================
 * Diagnostics
 * ========================================================================= */
extern int  g_error_count;
extern int  g_warn_count;
extern bool g_had_error;

void diag_emit(const char *level, SrcLoc loc, const char *fmt, ...);

static inline void diag_note(SrcLoc loc, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    diag_emit("note", loc, "%s", buf);
}
static inline void diag_warn(SrcLoc loc, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    diag_emit("warning", loc, "%s", buf);
    g_warn_count++;
}
static inline void diag_error(SrcLoc loc, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    diag_emit("error", loc, "%s", buf);
    g_error_count++;
    g_had_error = true;
}
void diag_fatal(SrcLoc loc, const char *fmt, ...); /* prints + exits */
void diag_ice(const char *file, int line, const char *msg); /* internal error */

#define ICE(msg)       diag_ice(__FILE__, __LINE__, (msg))
#define UNREACHABLE()  ICE("unreachable code")

/* =========================================================================
 * Arena allocator — allocation-only, freed all at once.
 * Ideal for AST nodes that live for the full compilation.
 * ========================================================================= */
#define ARENA_BLOCK_SZ (64u * 1024u)

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    usize used;
    usize cap;
    u8    data[]; /* flexible array member */
} ArenaChunk;

typedef struct {
    ArenaChunk *head;
    usize       total;
} Arena;

Arena *arena_new(void);
void  *arena_alloc(Arena *a, usize sz);
void  *arena_calloc(Arena *a, usize sz);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, usize n);
void   arena_free(Arena *a);

/* =========================================================================
 * Dynamic array (vector of void*)
 * ========================================================================= */
typedef struct {
    void  **items;
    usize   len;
    usize   cap;
} Vec;

Vec  *vec_new(void);
void  vec_push(Vec *v, void *item);
void *vec_at(const Vec *v, usize i);
void  vec_free(Vec *v);
void  vec_reset(Vec *v);
void  vec_ensure(Vec *v, usize cap);

/* =========================================================================
 * String builder
 * ========================================================================= */
typedef struct {
    char *buf;
    usize len;
    usize cap;
} StrBuf;

StrBuf *sb_new(void);
void    sb_putc(StrBuf *sb, char c);
void    sb_puts(StrBuf *sb, const char *s);
void    sb_printf(StrBuf *sb, const char *fmt, ...);
char   *sb_take(StrBuf *sb); /* caller owns returned string */
void    sb_free(StrBuf *sb);

/* =========================================================================
 * Hash map — open-chaining, string keys, void* values.
 * ========================================================================= */
typedef struct HEntry {
    const char   *key;
    void         *val;
    struct HEntry *next;
    u64           hash;
} HEntry;

typedef struct {
    HEntry **buckets;
    usize    n_buckets;
    usize    count;
} HMap;

HMap  *hmap_new(void);
void   hmap_put(HMap *m, const char *key, void *val);
void  *hmap_get(HMap *m, const char *key);
bool   hmap_has(HMap *m, const char *key);
void   hmap_del(HMap *m, const char *key);
void   hmap_free(HMap *m);

/* iterate: HEntry *e; for (usize _b=0;_b<m->n_buckets;_b++) for (e=m->buckets[_b];e;e=e->next) */

/* =========================================================================
 * Misc utilities
 * ========================================================================= */
u64   fnv1a(const char *s, usize len);
char *ac_strndup(const char *s, usize n);
/* replaces \ escape sequences in a string literal value, returns malloc'd buf */
char *unescape_string(const char *s, usize len, usize *out_len);
i32   unescape_char(const char *s);
