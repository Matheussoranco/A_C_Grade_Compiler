/*
 * ast.h — Abstract Syntax Tree node definitions for the AC language.
 *
 * All AST nodes are allocated in an Arena and share the same AstNode
 * discriminated-union structure. The semantic pass annotates nodes with
 * resolved types (node->type) and resolved symbols (node->sym).
 */
#pragma once
#include "common.h"
#include "lexer.h"
#include "types.h"

/* Forward declarations */
typedef struct AstNode AstNode;
typedef struct Symbol  Symbol;

/* =========================================================================
 * Node kinds — one entry per grammatical construct.
 * ========================================================================= */
typedef enum {
    /* --- Top-level declarations ------------------------------------------ */
    AST_PROGRAM,       /* root: list of top-level decls               */
    AST_FN_DECL,       /* fn name(params) -> ret { body }             */
    AST_STRUCT_DECL,   /* struct Name { fields }                      */
    AST_GLOBAL_LET,    /* top-level let name: T = expr;               */
    AST_EXTERN_FN,     /* extern fn name(params) -> ret;              */

    /* --- Statements ------------------------------------------------------ */
    AST_BLOCK,         /* { stmt* }                                   */
    AST_LET_STMT,      /* let name : T = expr ;                       */
    AST_VAR_STMT,      /* var name : T = expr ;                       */
    AST_ASSIGN,        /* lvalue = rvalue                             */
    AST_COMPOUND_ASSIGN, /* lvalue op= rvalue  (op = +,-,*,etc.)     */
    AST_IF,            /* if (cond) then [else alt]                   */
    AST_WHILE,         /* while (cond) body                           */
    AST_FOR,           /* for (init; cond; step) body                 */
    AST_RETURN,        /* return [expr]                               */
    AST_BREAK,
    AST_CONTINUE,
    AST_EXPR_STMT,     /* expression used as statement                */

    /* --- Expressions ----------------------------------------------------- */
    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_BOOL_LIT,
    AST_CHAR_LIT,
    AST_STR_LIT,
    AST_NULL_LIT,
    AST_IDENT,         /* variable / function reference               */
    AST_UNARY,         /* op expr                                     */
    AST_BINARY,        /* expr op expr                                */
    AST_TERNARY,       /* cond ? then : else                          */
    AST_CALL,          /* callee(args...)                             */
    AST_INDEX,         /* base[index]                                 */
    AST_FIELD,         /* base.field   (dot access)                   */
    AST_ARROW,         /* base->field  (pointer member access)        */
    AST_CAST,          /* expr as T                                   */
    AST_SIZEOF_EXPR,   /* sizeof(T)                                   */
    AST_ADDR,          /* &expr                                       */
    AST_DEREF,         /* *expr                                       */
    AST_PRE_INC,       /* ++expr                                      */
    AST_PRE_DEC,       /* --expr                                      */
    AST_POST_INC,      /* expr++                                      */
    AST_POST_DEC,      /* expr--                                      */

    /* --- Type nodes (used inside declarations and casts) ----------------- */
    AST_TYPE_NAME,     /* a type annotation: used by parser           */

    AST__COUNT,
} AstKind;

/* =========================================================================
 * Function parameter
 * ========================================================================= */
typedef struct {
    const char *name;
    Type       *type;
    SrcLoc      loc;
    Symbol     *sym; /* filled by semantic pass */
} Param;

/* =========================================================================
 * Struct field (AST level, before layout)
 * ========================================================================= */
typedef struct {
    const char *name;
    Type       *type;
    SrcLoc      loc;
} AstField;

/* =========================================================================
 * The unified AST node
 * ========================================================================= */
struct AstNode {
    AstKind  kind;
    SrcLoc   loc;
    Type    *type;  /* annotated by semantic pass; NULL before that          */
    Symbol  *sym;   /* for IDENT / FN_DECL / LET_STMT / VAR_STMT nodes       */

    union {
        /* AST_PROGRAM */
        struct { Vec *decls; } program;

        /* AST_FN_DECL */
        struct {
            const char *name;
            Param      *params;
            usize       n_params;
            Type       *ret_type;
            AstNode    *body;    /* NULL for extern (handled by AST_EXTERN_FN) */
            bool        is_extern;
            bool        variadic;
        } fn;

        /* AST_STRUCT_DECL */
        struct {
            const char *name;
            AstField   *fields;
            usize       n_fields;
        } strct;

        /* AST_GLOBAL_LET */
        struct {
            const char *name;
            Type       *annot;   /* explicit type annotation, may be NULL */
            AstNode    *init;    /* initializer expression                */
            bool        is_const;
        } global;

        /* AST_EXTERN_FN — same as fn but no body */

        /* AST_BLOCK */
        struct { Vec *stmts; } block;

        /* AST_LET_STMT / AST_VAR_STMT */
        struct {
            const char *name;
            Type       *annot;
            AstNode    *init;
        } local;

        /* AST_ASSIGN / AST_COMPOUND_ASSIGN */
        struct {
            AstNode  *lhs;
            AstNode  *rhs;
            TokKind   op; /* TOK_EQ for plain, TOK_PLUS_EQ etc. for compound */
        } assign;

        /* AST_IF */
        struct {
            AstNode *cond;
            AstNode *then;
            AstNode *else_; /* may be NULL */
        } if_;

        /* AST_WHILE */
        struct {
            AstNode *cond;
            AstNode *body;
        } while_;

        /* AST_FOR */
        struct {
            AstNode *init;  /* stmt (let or assign or expr) */
            AstNode *cond;
            AstNode *step;
            AstNode *body;
        } for_;

        /* AST_RETURN */
        struct { AstNode *val; /* NULL for bare return */ } ret;

        /* AST_EXPR_STMT */
        struct { AstNode *expr; } expr_stmt;

        /* AST_INT_LIT */
        struct { i64 val; } int_lit;

        /* AST_FLOAT_LIT */
        struct { f64 val; } flt_lit;

        /* AST_BOOL_LIT */
        struct { bool val; } bool_lit;

        /* AST_CHAR_LIT */
        struct { i32 val; } char_lit;

        /* AST_STR_LIT */
        struct { const char *val; usize len; } str_lit;

        /* AST_IDENT */
        struct { const char *name; } ident;

        /* AST_UNARY */
        struct { TokKind op; AstNode *operand; } unary;

        /* AST_BINARY */
        struct { TokKind op; AstNode *lhs; AstNode *rhs; } binary;

        /* AST_TERNARY */
        struct { AstNode *cond; AstNode *then; AstNode *else_; } ternary;

        /* AST_CALL */
        struct {
            AstNode *callee;
            Vec     *args;     /* Vec<AstNode*> */
        } call;

        /* AST_INDEX */
        struct { AstNode *base; AstNode *idx; } index;

        /* AST_FIELD / AST_ARROW */
        struct { AstNode *base; const char *field; } member;

        /* AST_CAST */
        struct { AstNode *expr; Type *to; } cast;

        /* AST_SIZEOF_EXPR */
        struct { Type *of; } sizeof_;

        /* AST_ADDR / AST_DEREF / AST_PRE_INC / AST_PRE_DEC /
           AST_POST_INC / AST_POST_DEC */
        struct { AstNode *expr; } unop;
    };
};

/* =========================================================================
 * Constructors — all nodes are arena-allocated.
 * ========================================================================= */
AstNode *ast_program(Arena *a, Vec *decls, SrcLoc loc);
AstNode *ast_fn_decl(Arena *a, const char *name, Param *params, usize np,
                     Type *ret, AstNode *body, bool is_extern, bool variadic, SrcLoc loc);
AstNode *ast_struct_decl(Arena *a, const char *name, AstField *fields, usize nf, SrcLoc loc);
AstNode *ast_global_let(Arena *a, const char *name, Type *annot, AstNode *init, bool is_const, SrcLoc loc);
AstNode *ast_block(Arena *a, Vec *stmts, SrcLoc loc);
AstNode *ast_let(Arena *a, const char *name, Type *annot, AstNode *init, SrcLoc loc);
AstNode *ast_var(Arena *a, const char *name, Type *annot, AstNode *init, SrcLoc loc);
AstNode *ast_assign(Arena *a, TokKind op, AstNode *lhs, AstNode *rhs, SrcLoc loc);
AstNode *ast_if(Arena *a, AstNode *cond, AstNode *then, AstNode *else_, SrcLoc loc);
AstNode *ast_while(Arena *a, AstNode *cond, AstNode *body, SrcLoc loc);
AstNode *ast_for(Arena *a, AstNode *init, AstNode *cond, AstNode *step, AstNode *body, SrcLoc loc);
AstNode *ast_return(Arena *a, AstNode *val, SrcLoc loc);
AstNode *ast_break(Arena *a, SrcLoc loc);
AstNode *ast_continue(Arena *a, SrcLoc loc);
AstNode *ast_expr_stmt(Arena *a, AstNode *expr, SrcLoc loc);

AstNode *ast_int_lit(Arena *a, i64 v, SrcLoc loc);
AstNode *ast_float_lit(Arena *a, f64 v, SrcLoc loc);
AstNode *ast_bool_lit(Arena *a, bool v, SrcLoc loc);
AstNode *ast_char_lit(Arena *a, i32 v, SrcLoc loc);
AstNode *ast_str_lit(Arena *a, const char *v, usize len, SrcLoc loc);
AstNode *ast_null_lit(Arena *a, SrcLoc loc);
AstNode *ast_ident(Arena *a, const char *name, SrcLoc loc);
AstNode *ast_unary(Arena *a, TokKind op, AstNode *operand, SrcLoc loc);
AstNode *ast_binary(Arena *a, TokKind op, AstNode *lhs, AstNode *rhs, SrcLoc loc);
AstNode *ast_ternary(Arena *a, AstNode *cond, AstNode *then, AstNode *else_, SrcLoc loc);
AstNode *ast_call(Arena *a, AstNode *callee, Vec *args, SrcLoc loc);
AstNode *ast_index(Arena *a, AstNode *base, AstNode *idx, SrcLoc loc);
AstNode *ast_field(Arena *a, AstNode *base, const char *field, bool arrow, SrcLoc loc);
AstNode *ast_cast(Arena *a, AstNode *expr, Type *to, SrcLoc loc);
AstNode *ast_sizeof(Arena *a, Type *of, SrcLoc loc);
AstNode *ast_addr(Arena *a, AstNode *expr, SrcLoc loc);
AstNode *ast_deref(Arena *a, AstNode *expr, SrcLoc loc);
AstNode *ast_pre_inc(Arena *a, AstNode *expr, bool is_dec, SrcLoc loc);
AstNode *ast_post_inc(Arena *a, AstNode *expr, bool is_dec, SrcLoc loc);

/* Pretty-print the AST for debugging */
void ast_dump(const AstNode *n, int indent, FILE *out);
