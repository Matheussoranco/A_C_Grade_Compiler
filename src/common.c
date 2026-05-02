/*
 * common.c — Implementation of shared utilities.
 */
#include "../include/common.h"

/* =========================================================================
 * Global diagnostic state
 * ========================================================================= */
int  g_error_count = 0;
int  g_warn_count  = 0;
bool g_had_error   = false;

void diag_emit(const char *level, SrcLoc loc, const char *fmt, ...) {
    if (loc.file && loc.line > 0)
        fprintf(stderr, "%s:%d:%d: %s: ", loc.file, loc.line, loc.col, level);
    else if (loc.file)
        fprintf(stderr, "%s: %s: ", loc.file, level);
    else
        fprintf(stderr, "%s: ", level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void diag_fatal(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_emit("fatal error", loc, "%s", buf);
    exit(1);
}

void diag_ice(const char *file, int line, const char *msg) {
    fprintf(stderr, "\n*** Internal Compiler Error ***\n");
    fprintf(stderr, "  at %s:%d\n", file, line);
    fprintf(stderr, "  %s\n", msg);
    fprintf(stderr, "Please report this bug.\n");
    abort();
}

/* =========================================================================
 * Arena allocator
 * ========================================================================= */
static ArenaChunk *chunk_new(usize cap) {
    ArenaChunk *c = malloc(sizeof(ArenaChunk) + cap);
    if (!c) { fprintf(stderr, "OOM\n"); exit(1); }
    c->next = NULL;
    c->used = 0;
    c->cap  = cap;
    return c;
}

Arena *arena_new(void) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) { fprintf(stderr, "OOM\n"); exit(1); }
    a->head  = chunk_new(ARENA_BLOCK_SZ);
    a->total = 0;
    return a;
}

void *arena_alloc(Arena *a, usize sz) {
    /* align to 8 bytes */
    sz = (sz + 7u) & ~7u;
    if (sz == 0) sz = 8;

    if (a->head->used + sz > a->head->cap) {
        usize block = sz > ARENA_BLOCK_SZ ? sz * 2 : ARENA_BLOCK_SZ;
        ArenaChunk *c = chunk_new(block);
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += sz;
    a->total += sz;
    return p;
}

void *arena_calloc(Arena *a, usize sz) {
    void *p = arena_alloc(a, sz);
    memset(p, 0, sz);
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    usize n = strlen(s);
    char *d = arena_alloc(a, n + 1);
    memcpy(d, s, n + 1);
    return d;
}

char *arena_strndup(Arena *a, const char *s, usize n) {
    char *d = arena_alloc(a, n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

/* =========================================================================
 * Dynamic array
 * ========================================================================= */
Vec *vec_new(void) {
    Vec *v = malloc(sizeof(Vec));
    v->items = NULL;
    v->len   = 0;
    v->cap   = 0;
    return v;
}

void vec_ensure(Vec *v, usize cap) {
    if (v->cap >= cap) return;
    usize new_cap = v->cap ? v->cap * 2 : 8;
    if (new_cap < cap) new_cap = cap;
    v->items = realloc(v->items, new_cap * sizeof(void*));
    if (!v->items) { fprintf(stderr, "OOM\n"); exit(1); }
    v->cap = new_cap;
}

void vec_push(Vec *v, void *item) {
    vec_ensure(v, v->len + 1);
    v->items[v->len++] = item;
}

void *vec_at(const Vec *v, usize i) {
    assert(i < v->len);
    return v->items[i];
}

void vec_free(Vec *v) {
    free(v->items);
    free(v);
}

void vec_reset(Vec *v) {
    v->len = 0;
}

/* =========================================================================
 * String builder
 * ========================================================================= */
StrBuf *sb_new(void) {
    StrBuf *sb = malloc(sizeof(StrBuf));
    sb->cap = 256;
    sb->len = 0;
    sb->buf = malloc(sb->cap);
    sb->buf[0] = '\0';
    return sb;
}

static void sb_grow(StrBuf *sb, usize needed) {
    if (sb->len + needed + 1 <= sb->cap) return;
    usize new_cap = sb->cap * 2;
    if (new_cap < sb->len + needed + 1) new_cap = sb->len + needed + 256;
    sb->buf = realloc(sb->buf, new_cap);
    sb->cap = new_cap;
}

void sb_putc(StrBuf *sb, char c) {
    sb_grow(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) {
    usize n = strlen(s);
    sb_grow(sb, n);
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Compute required size */
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    va_end(ap);
    if (n <= 0) return;
    sb_grow(sb, (usize)n);
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (usize)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (usize)n;
}

char *sb_take(StrBuf *sb) {
    char *s = sb->buf;
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    return s;
}

void sb_free(StrBuf *sb) {
    free(sb->buf);
    free(sb);
}

/* =========================================================================
 * Hash map
 * ========================================================================= */
#define HMAP_INIT_BUCKETS 16
#define HMAP_LOAD_THRESH  0.75

u64 fnv1a(const char *s, usize len) {
    u64 h = 14695981039346656037ULL;
    for (usize i = 0; i < len; i++) {
        h ^= (u8)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

HMap *hmap_new(void) {
    HMap *m = malloc(sizeof(HMap));
    m->n_buckets = HMAP_INIT_BUCKETS;
    m->count     = 0;
    m->buckets   = calloc(m->n_buckets, sizeof(HEntry*));
    return m;
}

static void hmap_rehash(HMap *m) {
    usize new_nb = m->n_buckets * 2;
    HEntry **new_b = calloc(new_nb, sizeof(HEntry*));
    for (usize i = 0; i < m->n_buckets; i++) {
        HEntry *e = m->buckets[i];
        while (e) {
            HEntry *next = e->next;
            usize idx = e->hash % new_nb;
            e->next = new_b[idx];
            new_b[idx] = e;
            e = next;
        }
    }
    free(m->buckets);
    m->buckets   = new_b;
    m->n_buckets = new_nb;
}

void hmap_put(HMap *m, const char *key, void *val) {
    if ((double)m->count / m->n_buckets > HMAP_LOAD_THRESH)
        hmap_rehash(m);
    u64 h = fnv1a(key, strlen(key));
    usize idx = h % m->n_buckets;
    for (HEntry *e = m->buckets[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) { e->val = val; return; }
    }
    HEntry *e = malloc(sizeof(HEntry));
    e->key  = key;
    e->val  = val;
    e->hash = h;
    e->next = m->buckets[idx];
    m->buckets[idx] = e;
    m->count++;
}

void *hmap_get(HMap *m, const char *key) {
    u64 h = fnv1a(key, strlen(key));
    usize idx = h % m->n_buckets;
    for (HEntry *e = m->buckets[idx]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e->val;
    return NULL;
}

bool hmap_has(HMap *m, const char *key) {
    return hmap_get(m, key) != NULL;
}

void hmap_del(HMap *m, const char *key) {
    u64 h = fnv1a(key, strlen(key));
    usize idx = h % m->n_buckets;
    HEntry **pp = &m->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            HEntry *dead = *pp;
            *pp = dead->next;
            free(dead);
            m->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void hmap_free(HMap *m) {
    for (usize i = 0; i < m->n_buckets; i++) {
        HEntry *e = m->buckets[i];
        while (e) { HEntry *n = e->next; free(e); e = n; }
    }
    free(m->buckets);
    free(m);
}

/* =========================================================================
 * Misc utilities
 * ========================================================================= */
char *ac_strndup(const char *s, usize n) {
    char *d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* Process escape sequences in a string literal value.
 * Input: raw content between the quotes (no surrounding quotes).
 * Output: malloc'd buffer with decoded bytes, *out_len set. */
char *unescape_string(const char *s, usize len, usize *out_len) {
    char *buf = malloc(len + 1);
    usize j = 0;
    for (usize i = 0; i < len; ) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n':  buf[j++] = '\n'; break;
                case 't':  buf[j++] = '\t'; break;
                case 'r':  buf[j++] = '\r'; break;
                case '\\': buf[j++] = '\\'; break;
                case '\'': buf[j++] = '\''; break;
                case '"':  buf[j++] = '"';  break;
                case '0':  buf[j++] = '\0'; break;
                case 'a':  buf[j++] = '\a'; break;
                case 'b':  buf[j++] = '\b'; break;
                case 'f':  buf[j++] = '\f'; break;
                case 'v':  buf[j++] = '\v'; break;
                case 'x': {
                    /* Hex escape: \xNN */
                    i++;
                    unsigned v = 0;
                    while (i < len && isxdigit((unsigned char)s[i])) {
                        v = v * 16 + (isdigit((unsigned char)s[i])
                            ? s[i] - '0'
                            : tolower((unsigned char)s[i]) - 'a' + 10);
                        i++;
                    }
                    buf[j++] = (char)v;
                    continue;
                }
                default:
                    buf[j++] = '\\';
                    buf[j++] = s[i];
                    break;
            }
            i++;
        } else {
            buf[j++] = s[i++];
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

i32 unescape_char(const char *s) {
    if (s[0] != '\\') return (u8)s[0];
    switch (s[1]) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '0':  return '\0';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case 'x': {
            unsigned v = 0;
            const char *p = s + 2;
            while (isxdigit((unsigned char)*p)) {
                v = v * 16 + (isdigit((unsigned char)*p)
                    ? *p - '0'
                    : tolower((unsigned char)*p) - 'a' + 10);
                p++;
            }
            return (i32)v;
        }
        default:
            return (u8)s[1];
    }
}
