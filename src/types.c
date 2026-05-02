/*
 * types.c — Type system implementation.
 */
#include "../include/types.h"

/* TypeCtx is fully defined in types.h (includes TypePrims p and ptr_cache) */

static Type *prim_new(Arena *a, TypeKind k, usize sz, usize align) {
    Type *t = arena_calloc(a, sizeof(Type));
    t->kind  = k;
    t->size  = sz;
    t->align = align;
    return t;
}

TypeCtx *typectx_new(Arena *arena) {
    TypeCtx *ctx = arena_calloc(arena, sizeof(TypeCtx));
    ctx->arena   = arena;
    ctx->structs = hmap_new();
    ctx->all     = vec_new();
    ctx->ptr_cache = hmap_new();

    ctx->p.void_ = prim_new(arena, TY_VOID, 0, 1);
    ctx->p.bool_ = prim_new(arena, TY_BOOL, 1, 1);
    ctx->p.char_ = prim_new(arena, TY_CHAR, 1, 1);
    ctx->p.i8    = prim_new(arena, TY_I8,   1, 1);
    ctx->p.i16   = prim_new(arena, TY_I16,  2, 2);
    ctx->p.i32   = prim_new(arena, TY_I32,  4, 4);
    ctx->p.i64   = prim_new(arena, TY_I64,  8, 8);
    ctx->p.u8    = prim_new(arena, TY_U8,   1, 1);
    ctx->p.u16   = prim_new(arena, TY_U16,  2, 2);
    ctx->p.u32   = prim_new(arena, TY_U32,  4, 4);
    ctx->p.u64   = prim_new(arena, TY_U64,  8, 8);
    ctx->p.f32   = prim_new(arena, TY_F32,  4, 4);
    ctx->p.f64   = prim_new(arena, TY_F64,  8, 8);
    return ctx;
}

Type *ty_void(TypeCtx *c) { return c->p.void_; }
Type *ty_bool(TypeCtx *c) { return c->p.bool_; }
Type *ty_i8  (TypeCtx *c) { return c->p.i8;    }
Type *ty_i16 (TypeCtx *c) { return c->p.i16;   }
Type *ty_i32 (TypeCtx *c) { return c->p.i32;   }
Type *ty_i64 (TypeCtx *c) { return c->p.i64;   }
Type *ty_u8  (TypeCtx *c) { return c->p.u8;    }
Type *ty_u16 (TypeCtx *c) { return c->p.u16;   }
Type *ty_u32 (TypeCtx *c) { return c->p.u32;   }
Type *ty_u64 (TypeCtx *c) { return c->p.u64;   }
Type *ty_f32 (TypeCtx *c) { return c->p.f32;   }
Type *ty_f64 (TypeCtx *c) { return c->p.f64;   }
Type *ty_char(TypeCtx *c) { return c->p.char_;  }

Type *ty_ptr(TypeCtx *ctx, Type *base) {
    /* Cache key: pointer to the type object address */
    char key[32];
    snprintf(key, sizeof key, "%p", (void*)base);
    Type *cached = hmap_get(ctx->ptr_cache, key);
    if (cached) return cached;

    Type *t      = arena_calloc(ctx->arena, sizeof(Type));
    t->kind      = TY_PTR;
    t->size      = 8; /* 64-bit pointers */
    t->align     = 8;
    t->ptr_base  = base;
    hmap_put(ctx->ptr_cache, arena_strdup(ctx->arena, key), t);
    vec_push(ctx->all, t);
    return t;
}

Type *ty_array(TypeCtx *ctx, Type *elem, usize n) {
    Type *t       = arena_calloc(ctx->arena, sizeof(Type));
    t->kind       = TY_ARRAY;
    t->array.elem = elem;
    t->array.n    = n;
    t->size       = elem->size * n;
    t->align      = elem->align;
    vec_push(ctx->all, t);
    return t;
}

Type *ty_fn(TypeCtx *ctx, Type *ret, Type **params, usize n, bool variadic) {
    Type *t        = arena_calloc(ctx->arena, sizeof(Type));
    t->kind        = TY_FN;
    t->size        = 8; /* function pointer size */
    t->align       = 8;
    t->fn.ret      = ret;
    t->fn.n_params = n;
    t->fn.variadic = variadic;
    if (n > 0) {
        t->fn.params = arena_alloc(ctx->arena, n * sizeof(Type*));
        memcpy(t->fn.params, params, n * sizeof(Type*));
    }
    vec_push(ctx->all, t);
    return t;
}

Type *ty_struct_new(TypeCtx *ctx, const char *name) {
    Type *t  = arena_calloc(ctx->arena, sizeof(Type));
    t->kind  = TY_STRUCT;
    t->s.name = arena_strdup(ctx->arena, name);
    t->s.complete = false;
    hmap_put(ctx->structs, t->s.name, t);
    vec_push(ctx->all, t);
    return t;
}

void ty_struct_complete(TypeCtx *ctx, Type *s, StructField *fields, usize n) {
    s->s.fields   = fields;
    s->s.n_fields = n;
    s->s.complete = true;
    ty_compute_layout(s);
    (void)ctx;
}

Type *ty_struct_lookup(TypeCtx *ctx, const char *name) {
    return hmap_get(ctx->structs, name);
}

/* =========================================================================
 * Layout computation
 * ========================================================================= */
void ty_compute_layout(Type *t) {
    if (t->kind != TY_STRUCT || !t->s.complete) return;
    usize off   = 0;
    usize align = 1;
    for (usize i = 0; i < t->s.n_fields; i++) {
        StructField *f = &t->s.fields[i];
        usize fa = f->type->align ? f->type->align : 1;
        off = (off + fa - 1) & ~(fa - 1); /* align field */
        f->offset = off;
        off += f->type->size;
        if (fa > align) align = fa;
    }
    /* Pad struct size to its alignment */
    t->size  = (off + align - 1) & ~(align - 1);
    t->align = align;
}

/* =========================================================================
 * Predicates
 * ========================================================================= */
bool ty_is_integer(const Type *t) {
    switch (t->kind) {
        case TY_BOOL: case TY_CHAR:
        case TY_I8: case TY_I16: case TY_I32: case TY_I64:
        case TY_U8: case TY_U16: case TY_U32: case TY_U64:
            return true;
        default: return false;
    }
}

bool ty_is_signed(const Type *t) {
    return t->kind == TY_I8 || t->kind == TY_I16 ||
           t->kind == TY_I32 || t->kind == TY_I64;
}

bool ty_is_unsigned(const Type *t) {
    return t->kind == TY_U8  || t->kind == TY_U16 ||
           t->kind == TY_U32 || t->kind == TY_U64 ||
           t->kind == TY_BOOL || t->kind == TY_CHAR;
}

bool ty_is_float(const Type *t) {
    return t->kind == TY_F32 || t->kind == TY_F64;
}

bool ty_is_numeric(const Type *t) {
    return ty_is_integer(t) || ty_is_float(t);
}

bool ty_is_scalar(const Type *t) {
    return ty_is_numeric(t) || t->kind == TY_PTR;
}

bool ty_is_ptr(const Type *t) {
    return t->kind == TY_PTR;
}

bool ty_is_void_ptr(const Type *t) {
    return t->kind == TY_PTR && t->ptr_base->kind == TY_VOID;
}

bool ty_equal(const Type *a, const Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_PTR:   return ty_equal(a->ptr_base, b->ptr_base);
        case TY_ARRAY:
            return a->array.n == b->array.n && ty_equal(a->array.elem, b->array.elem);
        case TY_STRUCT: return strcmp(a->s.name, b->s.name) == 0;
        case TY_FN: {
            if (!ty_equal(a->fn.ret, b->fn.ret)) return false;
            if (a->fn.n_params != b->fn.n_params) return false;
            for (usize i = 0; i < a->fn.n_params; i++)
                if (!ty_equal(a->fn.params[i], b->fn.params[i])) return false;
            return a->fn.variadic == b->fn.variadic;
        }
        default: return true; /* same kind = same primitive */
    }
}

bool ty_compatible(const Type *a, const Type *b) {
    if (ty_equal(a, b)) return true;
    /* Numeric conversions */
    if (ty_is_numeric(a) && ty_is_numeric(b)) return true;
    /* null → pointer */
    if (b->kind == TY_PTR) return true; /* any value → pointer (with warning) */
    /* void* ↔ T* */
    if (a->kind == TY_PTR && b->kind == TY_PTR)
        return a->ptr_base->kind == TY_VOID || b->ptr_base->kind == TY_VOID;
    return false;
}

/* Usual arithmetic conversions (C11 §6.3.1.8 simplified) */
Type *ty_arith_common(TypeCtx *ctx, const Type *a, const Type *b) {
    /* float wins */
    if (a->kind == TY_F64 || b->kind == TY_F64) return ty_f64(ctx);
    if (a->kind == TY_F32 || b->kind == TY_F32) return ty_f64(ctx);
    /* promote to at least i32 */
    usize sa = a->size > 4 ? a->size : 4;
    usize sb = b->size > 4 ? b->size : 4;
    usize sz = sa > sb ? sa : sb;
    bool sign = ty_is_signed(a) || ty_is_signed(b);
    if (sz == 8) return sign ? ty_i64(ctx) : ty_u64(ctx);
    return sign ? ty_i32(ctx) : ty_u32(ctx);
}

Type *ty_implicit_cast(const Type *from, const Type *to) {
    if (ty_equal(from, to)) return (Type*)to;
    if (ty_is_numeric(from) && ty_is_numeric(to)) return (Type*)to;
    if (from->kind == TY_PTR && to->kind == TY_PTR) {
        if (from->ptr_base->kind == TY_VOID || to->ptr_base->kind == TY_VOID)
            return (Type*)to;
    }
    return NULL;
}

/* =========================================================================
 * Pretty-printing
 * ========================================================================= */
void ty_print(const Type *t, FILE *out) {
    if (!t) { fprintf(out, "<null-type>"); return; }
    switch (t->kind) {
        case TY_VOID:  fputs("void",  out); return;
        case TY_BOOL:  fputs("bool",  out); return;
        case TY_CHAR:  fputs("char",  out); return;
        case TY_I8:    fputs("i8",    out); return;
        case TY_I16:   fputs("i16",   out); return;
        case TY_I32:   fputs("i32",   out); return;
        case TY_I64:   fputs("i64",   out); return;
        case TY_U8:    fputs("u8",    out); return;
        case TY_U16:   fputs("u16",   out); return;
        case TY_U32:   fputs("u32",   out); return;
        case TY_U64:   fputs("u64",   out); return;
        case TY_F32:   fputs("f32",   out); return;
        case TY_F64:   fputs("f64",   out); return;
        case TY_PTR:
            fputc('*', out);
            ty_print(t->ptr_base, out);
            return;
        case TY_ARRAY:
            fprintf(out, "[%zu]", t->array.n);
            ty_print(t->array.elem, out);
            return;
        case TY_STRUCT:
            fprintf(out, "struct %s", t->s.name);
            return;
        case TY_FN:
            fputs("fn(", out);
            for (usize i = 0; i < t->fn.n_params; i++) {
                if (i) fputs(", ", out);
                ty_print(t->fn.params[i], out);
            }
            if (t->fn.variadic) fputs(t->fn.n_params ? ", ..." : "...", out);
            fputs(") -> ", out);
            ty_print(t->fn.ret, out);
            return;
        case TY_UNRESOLVED:
            fprintf(out, "?%s", t->unresolved_name ? t->unresolved_name : "?");
            return;
        default:
            fputs("<unknown-type>", out);
    }
}

char *ty_to_str(const Type *t) {
    StrBuf *sb = sb_new();
    /* Reuse ty_print via a temp FILE* — simpler: just use StrBuf */
    if (!t) { sb_puts(sb, "<null>"); return sb_take(sb); }
    char buf[256];
    /* Abuse the stdout trick — just build manually */
    switch (t->kind) {
        case TY_VOID:  sb_puts(sb, "void");  break;
        case TY_BOOL:  sb_puts(sb, "bool");  break;
        case TY_CHAR:  sb_puts(sb, "char");  break;
        case TY_I8:    sb_puts(sb, "i8");    break;
        case TY_I16:   sb_puts(sb, "i16");   break;
        case TY_I32:   sb_puts(sb, "i32");   break;
        case TY_I64:   sb_puts(sb, "i64");   break;
        case TY_U8:    sb_puts(sb, "u8");    break;
        case TY_U16:   sb_puts(sb, "u16");   break;
        case TY_U32:   sb_puts(sb, "u32");   break;
        case TY_U64:   sb_puts(sb, "u64");   break;
        case TY_F32:   sb_puts(sb, "f32");   break;
        case TY_F64:   sb_puts(sb, "f64");   break;
        case TY_PTR: {
            char *base = ty_to_str(t->ptr_base);
            sb_putc(sb, '*');
            sb_puts(sb, base);
            free(base);
            break;
        }
        case TY_ARRAY: {
            char *elem = ty_to_str(t->array.elem);
            snprintf(buf, sizeof buf, "[%zu]", t->array.n);
            sb_puts(sb, buf);
            sb_puts(sb, elem);
            free(elem);
            break;
        }
        case TY_STRUCT:
            sb_printf(sb, "struct %s", t->s.name);
            break;
        default:
            sb_puts(sb, "<type>");
    }
    return sb_take(sb);
}
