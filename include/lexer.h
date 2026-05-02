/*
 * lexer.h — Token types and lexer interface for the AC language.
 *
 * The lexer converts raw source text into a flat stream of typed tokens.
 * It tracks source locations for diagnostic messages and supports up to
 * 4-token look-ahead via an internal ring buffer.
 */
#pragma once
#include "common.h"

/* =========================================================================
 * Token kinds — every terminal in the AC grammar.
 * ========================================================================= */
typedef enum {
    /* --- Literals -------------------------------------------------------- */
    TOK_INT_LIT,    /* 42  0xFF  0b1010  0o77  0xDEAD_BEEF               */
    TOK_FLOAT_LIT,  /* 3.14  1.0e-5  .5f                                 */
    TOK_CHAR_LIT,   /* 'a'  '\n'  '\x41'                                 */
    TOK_STR_LIT,    /* "hello\nworld"                                    */
    TOK_TRUE,       /* true                                              */
    TOK_FALSE,      /* false                                             */
    TOK_NULL,       /* null                                              */

    /* --- Identifier ------------------------------------------------------ */
    TOK_IDENT,

    /* --- Keywords -------------------------------------------------------- */
    TOK_FN,
    TOK_LET,
    TOK_VAR,
    TOK_STRUCT,
    TOK_EXTERN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_RETURN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_AS,
    TOK_SIZEOF,

    /* --- Primitive type keywords ----------------------------------------- */
    TOK_I8, TOK_I16, TOK_I32, TOK_I64,
    TOK_U8, TOK_U16, TOK_U32, TOK_U64,
    TOK_F32, TOK_F64,
    TOK_BOOL, TOK_CHAR_KW, TOK_VOID,

    /* --- Arithmetic operators -------------------------------------------- */
    TOK_PLUS,         /* +   */
    TOK_MINUS,        /* -   */
    TOK_STAR,         /* *   */
    TOK_SLASH,        /* /   */
    TOK_PERCENT,      /* %   */

    /* --- Bitwise operators ----------------------------------------------- */
    TOK_AMP,          /* &   */
    TOK_PIPE,         /* |   */
    TOK_CARET,        /* ^   */
    TOK_TILDE,        /* ~   */
    TOK_LSHIFT,       /* <<  */
    TOK_RSHIFT,       /* >>  */

    /* --- Logical operators ----------------------------------------------- */
    TOK_AMP_AMP,      /* &&  */
    TOK_PIPE_PIPE,    /* ||  */
    TOK_BANG,         /* !   */

    /* --- Comparison operators -------------------------------------------- */
    TOK_EQ_EQ,        /* ==  */
    TOK_BANG_EQ,      /* !=  */
    TOK_LT,           /* <   */
    TOK_GT,           /* >   */
    TOK_LT_EQ,        /* <=  */
    TOK_GT_EQ,        /* >=  */

    /* --- Assignment operators -------------------------------------------- */
    TOK_EQ,           /* =   */
    TOK_PLUS_EQ,      /* +=  */
    TOK_MINUS_EQ,     /* -=  */
    TOK_STAR_EQ,      /* *=  */
    TOK_SLASH_EQ,     /* /=  */
    TOK_PERCENT_EQ,   /* %=  */
    TOK_AMP_EQ,       /* &=  */
    TOK_PIPE_EQ,      /* |=  */
    TOK_CARET_EQ,     /* ^=  */
    TOK_LSHIFT_EQ,    /* <<= */
    TOK_RSHIFT_EQ,    /* >>= */

    /* --- Increment / decrement ------------------------------------------- */
    TOK_PLUS_PLUS,    /* ++  */
    TOK_MINUS_MINUS,  /* --  */

    /* --- Member access / arrows ------------------------------------------ */
    TOK_DOT,          /* .   */
    TOK_ARROW,        /* ->  */
    TOK_ELLIPSIS,     /* ... */

    /* --- Punctuation ----------------------------------------------------- */
    TOK_LPAREN,       /* (   */
    TOK_RPAREN,       /* )   */
    TOK_LBRACE,       /* {   */
    TOK_RBRACE,       /* }   */
    TOK_LBRACKET,     /* [   */
    TOK_RBRACKET,     /* ]   */
    TOK_SEMICOLON,    /* ;   */
    TOK_COLON,        /* :   */
    TOK_COMMA,        /* ,   */

    /* --- Control --------------------------------------------------------- */
    TOK_EOF,
    TOK_ERROR,        /* lexer error; value holds offending character      */

    TOK__COUNT,
} TokKind;

/* =========================================================================
 * Token — immutable; text points into the source buffer (not owned).
 * ========================================================================= */
typedef struct {
    TokKind    kind;
    SrcLoc     loc;
    const char *text;      /* raw source text slice                         */
    usize       text_len;
    union {
        i64  int_val;      /* TOK_INT_LIT, TOK_CHAR_LIT                     */
        f64  flt_val;      /* TOK_FLOAT_LIT                                 */
    };
} Token;

/* =========================================================================
 * Lexer
 * ========================================================================= */
#define LEXER_PEEK_BUF 4

typedef struct {
    const char *src;
    usize       src_len;
    usize       pos;
    i32         line;
    i32         col;
    const char *filename;
    Arena      *arena;
    /* ring buffer for look-ahead */
    Token   peek[LEXER_PEEK_BUF];
    int     peek_count;
    int     peek_head;  /* index of oldest peeked token */
} Lexer;

/* =========================================================================
 * Public API
 * ========================================================================= */
Lexer      *lexer_new(const char *src, usize len, const char *filename, Arena *arena);
void        lexer_free(Lexer *l);

Token       lexer_next(Lexer *l);
Token       lexer_peek_n(Lexer *l, int n);  /* 0 = next, 1 = one beyond, etc. */
#define     lexer_peek(l) lexer_peek_n((l), 0)

const char *tok_kind_name(TokKind k);
void        tok_dump(const Token *t, FILE *out);
