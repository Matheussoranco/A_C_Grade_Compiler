/*
 * types.h — Type representation for the AC type system.
 *
 * Types are interned into a global table so that pointer equality implies
 * structural equality. This enables O(1) type comparison throughout the
 * semantic and IR-generation passes.
 */
#pragma once
#include "common.h"

/* =========================================================================
 * Type kinds
 * ========================================================================= */
typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_I8,  TY_I16, TY_I32, TY_I64,
    TY_U8,  TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64,
    TY_CHAR,    /* alias for u8, used for text */
    TY_PTR,     /* *T                          */
    TY_ARRAY,   /* [N]T                        */
    TY_STRUCT,
    TY_FN,      /* function type               */
    TY_UNRESOLVED, /* placeholder during parsing */
} TypeKind;

/* Forward declaration */
typedef struct Type Type;
typedef struct StructField StructField;
typedef struct FnType FnType;

/* =========================================================================
 * Type node
 * ========================================================================= */
struct StructField {
    const char   *name;
    Type         *type;
    usize         offset; /* byte offset within struct (set by layout pass) */
};

struct FnType {
    Type  *ret;
    Type **params;
    usize  n_params;
    bool   variadic;
};

struct Type {
    TypeKind kind;
    usize    size;   /* sizeof in bytes (0 = unknown/void)        */
    usize    align;  /* alignment in bytes                        */
    union {
        Type        *ptr_base;   /* TY_PTR: pointee                */
        struct {
            Type    *elem;       /* TY_ARRAY: element type         */
            usize    n;          /* TY_ARRAY: element count        */
        } array;
        struct {
            const char   *name;
            StructField  *fields;
            usize         n_fields;
            bool          complete; /* false = forward-declared     */
        } s;                     /* TY_STRUCT                      */
        FnType fn;               /* TY_FN                          */
        const char *unresolved_name; /* TY_UNRESOLVED              */
    };
};

/* =========================================================================
 * Type registry — all types interned here; freed at end of compilation.
 * ========================================================================= */
typedef struct {
    Type *void_, *bool_, *char_;
    Type *i8, *i16, *i32, *i64;
    Type *u8, *u16, *u32, *u64;
    Type *f32, *f64;
} TypePrims;

typedef struct {
    Arena     *arena;
    HMap      *structs;    /* name → Type* for struct types    */
    Vec       *all;        /* all allocated types              */
    HMap      *ptr_cache;  /* "0x..." → Type* for ptr types    */
    TypePrims  p;          /* singleton primitive types        */
} TypeCtx;

TypeCtx *typectx_new(Arena *arena);

/* Primitive singletons (returned by pointer, always the same object) */
Type *ty_void(TypeCtx *ctx);
Type *ty_bool(TypeCtx *ctx);
Type *ty_i8(TypeCtx *ctx);  Type *ty_i16(TypeCtx *ctx);
Type *ty_i32(TypeCtx *ctx); Type *ty_i64(TypeCtx *ctx);
Type *ty_u8(TypeCtx *ctx);  Type *ty_u16(TypeCtx *ctx);
Type *ty_u32(TypeCtx *ctx); Type *ty_u64(TypeCtx *ctx);
Type *ty_f32(TypeCtx *ctx); Type *ty_f64(TypeCtx *ctx);
Type *ty_char(TypeCtx *ctx);

/* Derived types */
Type *ty_ptr(TypeCtx *ctx, Type *base);
Type *ty_array(TypeCtx *ctx, Type *elem, usize n);
Type *ty_fn(TypeCtx *ctx, Type *ret, Type **params, usize n_params, bool variadic);

/* Struct types */
Type *ty_struct_new(TypeCtx *ctx, const char *name);
void  ty_struct_complete(TypeCtx *ctx, Type *s, StructField *fields, usize n);
Type *ty_struct_lookup(TypeCtx *ctx, const char *name);

/* Predicates */
bool ty_is_integer(const Type *t);
bool ty_is_signed(const Type *t);
bool ty_is_unsigned(const Type *t);
bool ty_is_float(const Type *t);
bool ty_is_numeric(const Type *t);
bool ty_is_scalar(const Type *t);   /* arithmetic or pointer */
bool ty_is_ptr(const Type *t);
bool ty_is_void_ptr(const Type *t);
bool ty_equal(const Type *a, const Type *b);
bool ty_compatible(const Type *a, const Type *b); /* implicit conversion OK */

/* Arithmetic conversion (usual arithmetic conversions, C11 §6.3.1.8) */
Type *ty_arith_common(TypeCtx *ctx, const Type *a, const Type *b);

/* Implicit cast — returns NULL if not valid */
Type *ty_implicit_cast(const Type *from, const Type *to);

/* Layout computation */
void ty_compute_layout(Type *t);

/* Pretty-printing */
void ty_print(const Type *t, FILE *out);
char *ty_to_str(const Type *t); /* malloc'd string */
