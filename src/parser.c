/*
 * parser.c — Recursive-descent parser for the AC language.
 *
 * Grammar overview (informal):
 *   program     := decl*
 *   decl        := fn_decl | struct_decl | extern_fn | global_let
 *   fn_decl     := 'fn' IDENT '(' params ')' ['->' type] block
 *   extern_fn   := 'extern' 'fn' IDENT '(' params [',' '...'] ')' ['->' type] ';'
 *   struct_decl := 'struct' IDENT '{' (IDENT ':' type ',')* '}'
 *   global_let  := 'let' IDENT [':' type] '=' expr ';'
 *   params      := (IDENT ':' type (',' IDENT ':' type)*)?
 *   type        := '*' type | '[' INT ']' type | IDENT | prim_kw
 *   block       := '{' stmt* '}'
 *   stmt        := let_stmt | var_stmt | if_stmt | while_stmt | for_stmt
 *                | return_stmt | break_stmt | continue_stmt | assign_or_expr
 *   expr        := ... (Pratt / precedence climbing)
 *
 * Error recovery: on a parse error, the parser advances past the next ';'
 * or '}' (statement-level synchronization) to reduce cascading errors.
 */
#include "../include/ast.h"
#include "../include/lexer.h"
#include "../include/types.h"

/* =========================================================================
 * Parser state
 * ========================================================================= */
typedef struct {
    Lexer   *lex;
    Arena   *arena;
    TypeCtx *types;
    bool     had_error;
} Parser;

/* -------------------------------------------------------------------------
 * Token utilities
 * ------------------------------------------------------------------------- */
static Token peek(Parser *p) {
    return lexer_peek(p->lex);
}


static Token advance(Parser *p) {
    return lexer_next(p->lex);
}

static bool check(Parser *p, TokKind k) {
    return peek(p).kind == k;
}

static bool match(Parser *p, TokKind k) {
    if (check(p, k)) { advance(p); return true; }
    return false;
}

static Token expect(Parser *p, TokKind k) {
    Token t = peek(p);
    if (t.kind != k) {
        diag_error(t.loc, "expected '%s', got '%s'",
                   tok_kind_name(k), tok_kind_name(t.kind));
        p->had_error = true;
    } else {
        advance(p);
    }
    return t;
}

/* Synchronize to next statement boundary after an error */
static void synchronize(Parser *p) {
    while (true) {
        TokKind k = peek(p).kind;
        if (k == TOK_EOF) return;
        if (k == TOK_SEMICOLON) { advance(p); return; }
        if (k == TOK_RBRACE)    return;
        if (k == TOK_FN || k == TOK_LET || k == TOK_VAR ||
            k == TOK_STRUCT || k == TOK_IF || k == TOK_WHILE ||
            k == TOK_FOR || k == TOK_RETURN) return;
        advance(p);
    }
}

static const char *tok_text(Parser *p, Token t) {
    return arena_strndup(p->arena, t.text, t.text_len);
}

/* =========================================================================
 * Type parsing
 * ========================================================================= */
static Type *parse_type(Parser *p);

static Type *parse_type(Parser *p) {
    Token t = peek(p);

    /* Pointer: '*' type */
    if (t.kind == TOK_STAR) {
        advance(p);
        Type *base = parse_type(p);
        return ty_ptr(p->types, base);
    }

    /* Fixed array: '[' INT ']' type  */
    if (t.kind == TOK_LBRACKET) {
        advance(p);
        Token sz_tok = expect(p, TOK_INT_LIT);
        usize n = (usize)sz_tok.int_val;
        expect(p, TOK_RBRACKET);
        Type *elem = parse_type(p);
        return ty_array(p->types, elem, n);
    }

    advance(p);
    switch (t.kind) {
        case TOK_VOID:    return ty_void(p->types);
        case TOK_BOOL:    return ty_bool(p->types);
        case TOK_CHAR_KW: return ty_char(p->types);
        case TOK_I8:      return ty_i8(p->types);
        case TOK_I16:     return ty_i16(p->types);
        case TOK_I32:     return ty_i32(p->types);
        case TOK_I64:     return ty_i64(p->types);
        case TOK_U8:      return ty_u8(p->types);
        case TOK_U16:     return ty_u16(p->types);
        case TOK_U32:     return ty_u32(p->types);
        case TOK_U64:     return ty_u64(p->types);
        case TOK_F32:     return ty_f32(p->types);
        case TOK_F64:     return ty_f64(p->types);
        case TOK_IDENT: {
            /* Struct or unresolved type name */
            const char *name = tok_text(p, t);
            Type *existing = ty_struct_lookup(p->types, name);
            if (existing) return existing;
            /* Forward reference: create unresolved placeholder */
            Type *unres = arena_calloc(p->arena, sizeof(Type));
            unres->kind = TY_UNRESOLVED;
            unres->unresolved_name = name;
            return unres;
        }
        default:
            diag_error(t.loc, "expected a type, got '%s'", tok_kind_name(t.kind));
            p->had_error = true;
            return ty_void(p->types);
    }
}

/* =========================================================================
 * Expression parsing — Pratt / precedence climbing
 * ========================================================================= */

/* Operator precedence levels */
typedef enum {
    PREC_NONE,
    PREC_COMMA,     /* ,        (not used as operator) */
    PREC_ASSIGN,    /* =  +=  -= etc. */
    PREC_TERNARY,   /* ?:       */
    PREC_OR,        /* ||       */
    PREC_AND,       /* &&       */
    PREC_BITOR,     /* |        */
    PREC_BITXOR,    /* ^        */
    PREC_BITAND,    /* &        */
    PREC_EQ,        /* == !=    */
    PREC_CMP,       /* < > <= >= */
    PREC_SHIFT,     /* << >>    */
    PREC_ADD,       /* + -      */
    PREC_MUL,       /* * / %    */
    PREC_CAST,      /* as       */
    PREC_UNARY,     /* ! ~ - (prefix) & * ++ -- */
    PREC_POSTFIX,   /* [] . -> () ++ -- */
    PREC_PRIMARY,
} Prec;

static Prec tok_infix_prec(TokKind k) {
    switch (k) {
        case TOK_EQ: case TOK_PLUS_EQ: case TOK_MINUS_EQ:
        case TOK_STAR_EQ: case TOK_SLASH_EQ: case TOK_PERCENT_EQ:
        case TOK_AMP_EQ: case TOK_PIPE_EQ: case TOK_CARET_EQ:
        case TOK_LSHIFT_EQ: case TOK_RSHIFT_EQ:
            return PREC_ASSIGN;
        case TOK_PIPE_PIPE:   return PREC_OR;
        case TOK_AMP_AMP:     return PREC_AND;
        case TOK_PIPE:        return PREC_BITOR;
        case TOK_CARET:       return PREC_BITXOR;
        case TOK_AMP:         return PREC_BITAND;
        case TOK_EQ_EQ:  case TOK_BANG_EQ: return PREC_EQ;
        case TOK_LT: case TOK_GT:
        case TOK_LT_EQ: case TOK_GT_EQ:    return PREC_CMP;
        case TOK_LSHIFT: case TOK_RSHIFT:  return PREC_SHIFT;
        case TOK_PLUS:   case TOK_MINUS:   return PREC_ADD;
        case TOK_STAR:   case TOK_SLASH:
        case TOK_PERCENT:                  return PREC_MUL;
        case TOK_AS:                       return PREC_CAST;
        default: return PREC_NONE;
    }
}

static bool is_right_assoc(TokKind k) {
    return k == TOK_EQ || k == TOK_PLUS_EQ || k == TOK_MINUS_EQ ||
           k == TOK_STAR_EQ || k == TOK_SLASH_EQ || k == TOK_PERCENT_EQ ||
           k == TOK_AMP_EQ  || k == TOK_PIPE_EQ  || k == TOK_CARET_EQ ||
           k == TOK_LSHIFT_EQ || k == TOK_RSHIFT_EQ;
}

static AstNode *parse_expr(Parser *p, Prec min_prec);

/* Parse a primary / prefix expression */
static AstNode *parse_prefix(Parser *p) {
    Token t = peek(p);
    SrcLoc loc = t.loc;

    /* Integer literal */
    if (t.kind == TOK_INT_LIT) {
        advance(p);
        return ast_int_lit(p->arena, t.int_val, loc);
    }
    /* Float literal */
    if (t.kind == TOK_FLOAT_LIT) {
        advance(p);
        return ast_float_lit(p->arena, t.flt_val, loc);
    }
    /* Bool literal */
    if (t.kind == TOK_TRUE) { advance(p); return ast_bool_lit(p->arena, true,  loc); }
    if (t.kind == TOK_FALSE){ advance(p); return ast_bool_lit(p->arena, false, loc); }
    /* Null */
    if (t.kind == TOK_NULL) { advance(p); return ast_null_lit(p->arena, loc); }
    /* Char literal */
    if (t.kind == TOK_CHAR_LIT) {
        advance(p);
        return ast_char_lit(p->arena, (i32)t.int_val, loc);
    }
    /* String literal */
    if (t.kind == TOK_STR_LIT) {
        advance(p);
        const char *v = arena_strndup(p->arena, t.text, t.text_len);
        return ast_str_lit(p->arena, v, t.text_len, loc);
    }
    /* Identifier */
    if (t.kind == TOK_IDENT) {
        advance(p);
        const char *name = tok_text(p, t);
        return ast_ident(p->arena, name, loc);
    }
    /* Grouped expression */
    if (t.kind == TOK_LPAREN) {
        advance(p);
        AstNode *e = parse_expr(p, PREC_NONE);
        expect(p, TOK_RPAREN);
        return e;
    }
    /* Unary operators */
    if (t.kind == TOK_BANG || t.kind == TOK_TILDE || t.kind == TOK_MINUS) {
        advance(p);
        AstNode *operand = parse_expr(p, PREC_UNARY);
        return ast_unary(p->arena, t.kind, operand, loc);
    }
    /* Address-of */
    if (t.kind == TOK_AMP) {
        advance(p);
        AstNode *operand = parse_expr(p, PREC_UNARY);
        return ast_addr(p->arena, operand, loc);
    }
    /* Dereference */
    if (t.kind == TOK_STAR) {
        advance(p);
        AstNode *operand = parse_expr(p, PREC_UNARY);
        return ast_deref(p->arena, operand, loc);
    }
    /* Prefix ++ / -- */
    if (t.kind == TOK_PLUS_PLUS || t.kind == TOK_MINUS_MINUS) {
        advance(p);
        AstNode *operand = parse_expr(p, PREC_UNARY);
        return ast_pre_inc(p->arena, operand, t.kind == TOK_MINUS_MINUS, loc);
    }
    /* sizeof */
    if (t.kind == TOK_SIZEOF) {
        advance(p);
        expect(p, TOK_LPAREN);
        Type *of = parse_type(p);
        expect(p, TOK_RPAREN);
        return ast_sizeof(p->arena, of, loc);
    }

    diag_error(t.loc, "unexpected token '%s' in expression", tok_kind_name(t.kind));
    p->had_error = true;
    advance(p);
    return ast_int_lit(p->arena, 0, loc); /* error recovery sentinel */
}

/* Parse postfix / infix / cast continuations */
static AstNode *parse_expr(Parser *p, Prec min_prec) {
    AstNode *lhs = parse_prefix(p);

    for (;;) {
        Token t = peek(p);
        SrcLoc loc = t.loc;

        /* Postfix: function call */
        if (t.kind == TOK_LPAREN) {
            advance(p);
            Vec *args = vec_new();
            if (!check(p, TOK_RPAREN)) {
                do {
                    vec_push(args, parse_expr(p, PREC_ASSIGN));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN);
            lhs = ast_call(p->arena, lhs, args, loc);
            continue;
        }
        /* Postfix: array index */
        if (t.kind == TOK_LBRACKET) {
            advance(p);
            AstNode *idx = parse_expr(p, PREC_NONE);
            expect(p, TOK_RBRACKET);
            lhs = ast_index(p->arena, lhs, idx, loc);
            continue;
        }
        /* Postfix: field access (dot) */
        if (t.kind == TOK_DOT) {
            advance(p);
            Token field = expect(p, TOK_IDENT);
            lhs = ast_field(p->arena, lhs, tok_text(p, field), false, loc);
            continue;
        }
        /* Postfix: pointer member access (->) */
        if (t.kind == TOK_ARROW) {
            advance(p);
            Token field = expect(p, TOK_IDENT);
            lhs = ast_field(p->arena, lhs, tok_text(p, field), true, loc);
            continue;
        }
        /* Postfix: post-increment / post-decrement */
        if (t.kind == TOK_PLUS_PLUS || t.kind == TOK_MINUS_MINUS) {
            advance(p);
            lhs = ast_post_inc(p->arena, lhs, t.kind == TOK_MINUS_MINUS, loc);
            continue;
        }

        /* Ternary */
        if (t.kind == TOK_PLUS && min_prec < PREC_TERNARY) {
            /* handled below as infix */
        }
        /* Actual ternary ? : */
        /* We treat it as right-assoc at PREC_TERNARY, but parse it separately */

        /* Infix operators */
        Prec prec = tok_infix_prec(t.kind);
        if (prec == PREC_NONE || prec < min_prec) break;

        /* Cast: expr 'as' type */
        if (t.kind == TOK_AS) {
            if (prec < min_prec) break;
            advance(p);
            Type *to = parse_type(p);
            lhs = ast_cast(p->arena, lhs, to, loc);
            continue;
        }

        advance(p);

        /* For ternary '?' we need special handling */
        /* (Ternary is parsed at PREC_OR level since '?' is not in the table) */

        Prec next = is_right_assoc(t.kind) ? prec : (Prec)(prec + 1);
        AstNode *rhs = parse_expr(p, next);

        /* Assignment operators → AST_ASSIGN / AST_COMPOUND_ASSIGN */
        if (t.kind == TOK_EQ || t.kind == TOK_PLUS_EQ || t.kind == TOK_MINUS_EQ ||
            t.kind == TOK_STAR_EQ || t.kind == TOK_SLASH_EQ || t.kind == TOK_PERCENT_EQ ||
            t.kind == TOK_AMP_EQ || t.kind == TOK_PIPE_EQ || t.kind == TOK_CARET_EQ ||
            t.kind == TOK_LSHIFT_EQ || t.kind == TOK_RSHIFT_EQ) {
            lhs = ast_assign(p->arena, t.kind, lhs, rhs, loc);
        } else {
            lhs = ast_binary(p->arena, t.kind, lhs, rhs, loc);
        }
    }
    return lhs;
}

/* =========================================================================
 * Statement parsing
 * ========================================================================= */
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);

static AstNode *parse_block(Parser *p) {
    SrcLoc loc = peek(p).loc;
    expect(p, TOK_LBRACE);
    Vec *stmts = vec_new();
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode *s = parse_stmt(p);
        if (s) vec_push(stmts, s);
    }
    expect(p, TOK_RBRACE);
    return ast_block(p->arena, stmts, loc);
}

static AstNode *parse_stmt(Parser *p) {
    Token t = peek(p);
    SrcLoc loc = t.loc;

    /* let name [: type] = expr ; */
    if (t.kind == TOK_LET || t.kind == TOK_VAR) {
        advance(p);
        bool is_let = (t.kind == TOK_LET);
        Token name = expect(p, TOK_IDENT);
        Type *annot = NULL;
        if (match(p, TOK_COLON)) annot = parse_type(p);
        AstNode *init = NULL;
        if (match(p, TOK_EQ)) init = parse_expr(p, PREC_NONE);
        expect(p, TOK_SEMICOLON);
        return is_let
            ? ast_let(p->arena, tok_text(p, name), annot, init, loc)
            : ast_var(p->arena, tok_text(p, name), annot, init, loc);
    }

    /* if ( cond ) block [else block] */
    if (t.kind == TOK_IF) {
        advance(p);
        expect(p, TOK_LPAREN);
        AstNode *cond = parse_expr(p, PREC_NONE);
        expect(p, TOK_RPAREN);
        AstNode *then = parse_block(p);
        AstNode *else_ = NULL;
        if (match(p, TOK_ELSE)) {
            if (check(p, TOK_IF)) else_ = parse_stmt(p);
            else                  else_ = parse_block(p);
        }
        return ast_if(p->arena, cond, then, else_, loc);
    }

    /* while ( cond ) block */
    if (t.kind == TOK_WHILE) {
        advance(p);
        expect(p, TOK_LPAREN);
        AstNode *cond = parse_expr(p, PREC_NONE);
        expect(p, TOK_RPAREN);
        AstNode *body = parse_block(p);
        return ast_while(p->arena, cond, body, loc);
    }

    /* for ( init_stmt ; cond ; step_expr ) block */
    if (t.kind == TOK_FOR) {
        advance(p);
        expect(p, TOK_LPAREN);
        AstNode *init = NULL;
        if (!check(p, TOK_SEMICOLON)) init = parse_stmt(p);
        else expect(p, TOK_SEMICOLON);
        AstNode *cond = NULL;
        if (!check(p, TOK_SEMICOLON)) cond = parse_expr(p, PREC_NONE);
        expect(p, TOK_SEMICOLON);
        AstNode *step = NULL;
        if (!check(p, TOK_RPAREN)) step = parse_expr(p, PREC_NONE);
        expect(p, TOK_RPAREN);
        AstNode *body = parse_block(p);
        /* Wrap step in expr_stmt if it's an expression */
        if (step && step->kind != AST_EXPR_STMT)
            step = ast_expr_stmt(p->arena, step, step->loc);
        return ast_for(p->arena, init, cond, step, body, loc);
    }

    /* return [expr] ; */
    if (t.kind == TOK_RETURN) {
        advance(p);
        AstNode *val = NULL;
        if (!check(p, TOK_SEMICOLON)) val = parse_expr(p, PREC_NONE);
        expect(p, TOK_SEMICOLON);
        return ast_return(p->arena, val, loc);
    }

    /* break ; */
    if (t.kind == TOK_BREAK) {
        advance(p); expect(p, TOK_SEMICOLON);
        return ast_break(p->arena, loc);
    }

    /* continue ; */
    if (t.kind == TOK_CONTINUE) {
        advance(p); expect(p, TOK_SEMICOLON);
        return ast_continue(p->arena, loc);
    }

    /* Nested block */
    if (t.kind == TOK_LBRACE) {
        return parse_block(p);
    }

    /* Expression statement (including assignments) */
    {
        AstNode *expr = parse_expr(p, PREC_NONE);
        expect(p, TOK_SEMICOLON);
        if (expr->kind == AST_ASSIGN || expr->kind == AST_COMPOUND_ASSIGN)
            return expr;
        return ast_expr_stmt(p->arena, expr, loc);
    }
}

/* =========================================================================
 * Top-level declaration parsing
 * ========================================================================= */
static Param *parse_params(Parser *p, usize *out_count, bool *out_variadic) {
    *out_count    = 0;
    *out_variadic = false;
    if (check(p, TOK_RPAREN)) return NULL;

    /* Temporary dynamic list */
    Param tmp[64];
    usize n = 0;

    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        if (check(p, TOK_ELLIPSIS)) {
            advance(p);
            *out_variadic = true;
            break;
        }
        if (n >= 64) {
            diag_error(peek(p).loc, "too many parameters (max 64)");
            break;
        }
        Token name = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        Type *ty = parse_type(p);
        tmp[n].name = tok_text(p, name);
        tmp[n].type = ty;
        tmp[n].loc  = name.loc;
        tmp[n].sym  = NULL;
        n++;
        if (!match(p, TOK_COMMA)) break;
    }
    if (n == 0) return NULL;
    Param *params = arena_alloc(p->arena, n * sizeof(Param));
    memcpy(params, tmp, n * sizeof(Param));
    *out_count = n;
    return params;
}

static AstNode *parse_fn(Parser *p, bool is_extern) {
    SrcLoc loc = peek(p).loc;
    expect(p, TOK_FN);
    Token name = expect(p, TOK_IDENT);
    expect(p, TOK_LPAREN);
    usize np;
    bool  variadic;
    Param *params = parse_params(p, &np, &variadic);
    expect(p, TOK_RPAREN);

    Type *ret = ty_void(p->types);
    if (match(p, TOK_ARROW)) ret = parse_type(p);

    AstNode *body = NULL;
    if (!is_extern) {
        body = parse_block(p);
    } else {
        expect(p, TOK_SEMICOLON);
    }
    return ast_fn_decl(p->arena, tok_text(p, name), params, np,
                       ret, body, is_extern, variadic, loc);
}

static AstNode *parse_struct(Parser *p) {
    SrcLoc loc = peek(p).loc;
    expect(p, TOK_STRUCT);
    Token name = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);

    AstField tmp[64];
    usize n = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (n >= 64) { diag_error(peek(p).loc, "too many struct fields"); break; }
        Token fname = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        Type *ft = parse_type(p);
        tmp[n].name = tok_text(p, fname);
        tmp[n].type = ft;
        tmp[n].loc  = fname.loc;
        n++;
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RBRACE);

    AstField *fields = NULL;
    if (n > 0) {
        fields = arena_alloc(p->arena, n * sizeof(AstField));
        memcpy(fields, tmp, n * sizeof(AstField));
    }
    return ast_struct_decl(p->arena, tok_text(p, name), fields, n, loc);
}

static AstNode *parse_global_let(Parser *p) {
    SrcLoc loc = peek(p).loc;
    expect(p, TOK_LET);
    Token name = expect(p, TOK_IDENT);
    Type *annot = NULL;
    if (match(p, TOK_COLON)) annot = parse_type(p);
    AstNode *init = NULL;
    if (match(p, TOK_EQ)) init = parse_expr(p, PREC_NONE);
    expect(p, TOK_SEMICOLON);
    return ast_global_let(p->arena, tok_text(p, name), annot, init, true, loc);
}

static AstNode *parse_decl(Parser *p) {
    Token t = peek(p);
    if (t.kind == TOK_FN)     return parse_fn(p, false);
    if (t.kind == TOK_STRUCT) return parse_struct(p);
    if (t.kind == TOK_LET)    return parse_global_let(p);
    if (t.kind == TOK_EXTERN) {
        SrcLoc loc = t.loc;
        advance(p);
        if (!check(p, TOK_FN)) {
            diag_error(loc, "expected 'fn' after 'extern'");
            synchronize(p);
            return NULL;
        }
        return parse_fn(p, true);
    }
    diag_error(t.loc, "expected a top-level declaration (fn, struct, let, extern), got '%s'",
               tok_kind_name(t.kind));
    p->had_error = true;
    synchronize(p);
    return NULL;
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */
AstNode *parse_program(Lexer *lex, Arena *arena, TypeCtx *types) {
    Parser p;
    p.lex       = lex;
    p.arena     = arena;
    p.types     = types;
    p.had_error = false;

    Vec *decls = vec_new();
    SrcLoc loc = peek(&p).loc;

    while (!check(&p, TOK_EOF)) {
        AstNode *d = parse_decl(&p);
        if (d) vec_push(decls, d);
    }

    return ast_program(arena, decls, loc);
}
