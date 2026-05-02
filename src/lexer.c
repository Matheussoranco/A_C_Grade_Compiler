/*
 * lexer.c — Lexical analysis for the AC language.
 *
 * Converts raw UTF-8 source text into a stream of typed tokens.
 * Supports multi-character look-ahead via an internal ring buffer.
 */
#include "../include/lexer.h"

/* =========================================================================
 * Keyword table — order matters for longest-match preference.
 * ========================================================================= */
typedef struct { const char *kw; TokKind kind; } KwEntry;

static const KwEntry KEYWORDS[] = {
    /* control flow */
    {"fn",       TOK_FN},
    {"let",      TOK_LET},
    {"var",      TOK_VAR},
    {"struct",   TOK_STRUCT},
    {"extern",   TOK_EXTERN},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"return",   TOK_RETURN},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"as",       TOK_AS},
    {"sizeof",   TOK_SIZEOF},
    /* literals */
    {"true",     TOK_TRUE},
    {"false",    TOK_FALSE},
    {"null",     TOK_NULL},
    /* primitive types */
    {"i8",       TOK_I8},
    {"i16",      TOK_I16},
    {"i32",      TOK_I32},
    {"i64",      TOK_I64},
    {"u8",       TOK_U8},
    {"u16",      TOK_U16},
    {"u32",      TOK_U32},
    {"u64",      TOK_U64},
    {"f32",      TOK_F32},
    {"f64",      TOK_F64},
    {"bool",     TOK_BOOL},
    {"char",     TOK_CHAR_KW},
    {"void",     TOK_VOID},
    {NULL,       TOK_EOF},
};

static TokKind lookup_keyword(const char *text, usize len) {
    for (int i = 0; KEYWORDS[i].kw; i++) {
        if (strlen(KEYWORDS[i].kw) == len &&
            memcmp(KEYWORDS[i].kw, text, len) == 0)
            return KEYWORDS[i].kind;
    }
    return TOK_IDENT;
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */
static char peek_ch(Lexer *l, int ahead) {
    usize idx = l->pos + (usize)ahead;
    return idx < l->src_len ? l->src[idx] : '\0';
}

static char advance_ch(Lexer *l) {
    char c = l->src[l->pos];
    l->pos++;
    if (c == '\n') { l->line++; l->col = 1; }
    else            { l->col++; }
    return c;
}

static Token make_tok(Lexer *l, TokKind k, const char *start, usize len, SrcLoc loc) {
    Token t;
    t.kind     = k;
    t.loc      = loc;
    t.text     = start;
    t.text_len = len;
    t.int_val  = 0;
    return t;
}

/* =========================================================================
 * Scan one token from current position
 * ========================================================================= */
static Token scan_token(Lexer *l) {
restart:
    /* Skip whitespace */
    while (l->pos < l->src_len && isspace((u8)l->src[l->pos]))
        advance_ch(l);

    if (l->pos >= l->src_len) {
        SrcLoc loc = {l->filename, l->line, l->col};
        return make_tok(l, TOK_EOF, l->src + l->pos, 0, loc);
    }

    SrcLoc loc = {l->filename, l->line, l->col};
    const char *start = l->src + l->pos;
    char c = peek_ch(l, 0);

    /* --- Line comment ---------------------------------------------------- */
    if (c == '/' && peek_ch(l, 1) == '/') {
        while (l->pos < l->src_len && l->src[l->pos] != '\n')
            advance_ch(l);
        goto restart;
    }

    /* --- Block comment --------------------------------------------------- */
    if (c == '/' && peek_ch(l, 1) == '*') {
        advance_ch(l); advance_ch(l); /* consume '/*' */
        while (l->pos < l->src_len) {
            if (l->src[l->pos] == '*' && peek_ch(l, 1) == '/') {
                advance_ch(l); advance_ch(l);
                goto restart;
            }
            advance_ch(l);
        }
        diag_error(loc, "unterminated block comment");
        goto restart;
    }

    /* --- String literal -------------------------------------------------- */
    if (c == '"') {
        advance_ch(l); /* consume '"' */
        const char *val_start = l->src + l->pos;
        while (l->pos < l->src_len && l->src[l->pos] != '"') {
            if (l->src[l->pos] == '\\') advance_ch(l); /* skip escape char */
            advance_ch(l);
        }
        usize val_len = (usize)(l->src + l->pos - val_start);
        if (l->pos >= l->src_len) {
            diag_error(loc, "unterminated string literal");
        } else {
            advance_ch(l); /* consume closing '"' */
        }
        Token t = make_tok(l, TOK_STR_LIT, val_start, val_len, loc);
        return t;
    }

    /* --- Character literal ----------------------------------------------- */
    if (c == '\'') {
        advance_ch(l); /* consume "'" */
        const char *val_start = l->src + l->pos;
        if (l->pos < l->src_len && l->src[l->pos] == '\\') {
            advance_ch(l); /* escape char */
        }
        if (l->pos >= l->src_len) {
            diag_error(loc, "unterminated character literal");
            Token t = make_tok(l, TOK_ERROR, start, 1, loc);
            return t;
        }
        advance_ch(l); /* the actual character */
        usize val_len = (usize)(l->src + l->pos - val_start);
        if (l->pos >= l->src_len || l->src[l->pos] != '\'') {
            diag_error(loc, "expected closing ' in character literal");
        } else {
            advance_ch(l);
        }
        Token t = make_tok(l, TOK_CHAR_LIT, val_start, val_len, loc);
        t.int_val = unescape_char(val_start);
        return t;
    }

    /* --- Integer / float literal ----------------------------------------- */
    if (isdigit((u8)c) || (c == '.' && isdigit((u8)peek_ch(l, 1)))) {
        bool is_float = false;
        /* Hex */
        if (c == '0' && (peek_ch(l, 1) == 'x' || peek_ch(l, 1) == 'X')) {
            advance_ch(l); advance_ch(l);
            while (isxdigit((u8)peek_ch(l, 0)) || peek_ch(l, 0) == '_')
                advance_ch(l);
        }
        /* Binary */
        else if (c == '0' && (peek_ch(l, 1) == 'b' || peek_ch(l, 1) == 'B')) {
            advance_ch(l); advance_ch(l);
            while (peek_ch(l, 0) == '0' || peek_ch(l, 0) == '1' || peek_ch(l, 0) == '_')
                advance_ch(l);
        }
        /* Octal */
        else if (c == '0' && (peek_ch(l, 1) == 'o' || peek_ch(l, 1) == 'O')) {
            advance_ch(l); advance_ch(l);
            while ((peek_ch(l, 0) >= '0' && peek_ch(l, 0) <= '7') || peek_ch(l, 0) == '_')
                advance_ch(l);
        }
        /* Decimal */
        else {
            while (isdigit((u8)peek_ch(l, 0)) || peek_ch(l, 0) == '_')
                advance_ch(l);
            if (peek_ch(l, 0) == '.') {
                is_float = true;
                advance_ch(l);
                while (isdigit((u8)peek_ch(l, 0)) || peek_ch(l, 0) == '_')
                    advance_ch(l);
            }
            /* Exponent */
            if (peek_ch(l, 0) == 'e' || peek_ch(l, 0) == 'E') {
                is_float = true;
                advance_ch(l);
                if (peek_ch(l, 0) == '+' || peek_ch(l, 0) == '-')
                    advance_ch(l);
                while (isdigit((u8)peek_ch(l, 0)))
                    advance_ch(l);
            }
        }
        usize len = (usize)(l->src + l->pos - start);
        /* Strip underscores for parsing */
        char buf[64]; usize bi = 0;
        for (usize i = 0; i < len && bi < sizeof(buf) - 1; i++)
            if (start[i] != '_') buf[bi++] = start[i];
        buf[bi] = '\0';

        Token t = make_tok(l, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, len, loc);
        if (is_float) {
            t.flt_val = strtod(buf, NULL);
        } else {
            t.int_val = (i64)strtoull(buf, NULL, 0);
        }
        return t;
    }

    /* --- Identifier / keyword -------------------------------------------- */
    if (isalpha((u8)c) || c == '_') {
        while (l->pos < l->src_len && (isalnum((u8)l->src[l->pos]) || l->src[l->pos] == '_'))
            advance_ch(l);
        usize len = (usize)(l->src + l->pos - start);
        TokKind kind = lookup_keyword(start, len);
        Token t = make_tok(l, kind, start, len, loc);
        if (kind == TOK_TRUE)  t.int_val = 1;
        if (kind == TOK_FALSE) t.int_val = 0;
        return t;
    }

    /* --- Multi-character operators (sorted longest-first) ---------------- */
    advance_ch(l);
    char c1 = peek_ch(l, 0);
    char c2 = peek_ch(l, 1);

#define OP3(a,b,cc,k) if (c==a && c1==b && c2==cc) { advance_ch(l); advance_ch(l); return make_tok(l,k,start,3,loc); }
#define OP2(a,b,k)    if (c==a && c1==b) { advance_ch(l); return make_tok(l,k,start,2,loc); }
#define OP1(a,k)      if (c==a) return make_tok(l,k,start,1,loc)

    OP3('<','<','=', TOK_LSHIFT_EQ);
    OP3('>','>','=', TOK_RSHIFT_EQ);
    OP3('.','.','.', TOK_ELLIPSIS);

    OP2('<','<',  TOK_LSHIFT);
    OP2('>','>',  TOK_RSHIFT);
    OP2('=','=',  TOK_EQ_EQ);
    OP2('!','=',  TOK_BANG_EQ);
    OP2('<','=',  TOK_LT_EQ);
    OP2('>','=',  TOK_GT_EQ);
    OP2('&','&',  TOK_AMP_AMP);
    OP2('|','|',  TOK_PIPE_PIPE);
    OP2('+','+',  TOK_PLUS_PLUS);
    OP2('-','-',  TOK_MINUS_MINUS);
    OP2('+','=',  TOK_PLUS_EQ);
    OP2('-','=',  TOK_MINUS_EQ);
    OP2('*','=',  TOK_STAR_EQ);
    OP2('/','=',  TOK_SLASH_EQ);
    OP2('%','=',  TOK_PERCENT_EQ);
    OP2('&','=',  TOK_AMP_EQ);
    OP2('|','=',  TOK_PIPE_EQ);
    OP2('^','=',  TOK_CARET_EQ);
    OP2('-','>',  TOK_ARROW);

    OP1('+', TOK_PLUS);
    OP1('-', TOK_MINUS);
    OP1('*', TOK_STAR);
    OP1('/', TOK_SLASH);
    OP1('%', TOK_PERCENT);
    OP1('&', TOK_AMP);
    OP1('|', TOK_PIPE);
    OP1('^', TOK_CARET);
    OP1('~', TOK_TILDE);
    OP1('!', TOK_BANG);
    OP1('<', TOK_LT);
    OP1('>', TOK_GT);
    OP1('=', TOK_EQ);
    OP1('.', TOK_DOT);
    OP1('(', TOK_LPAREN);
    OP1(')', TOK_RPAREN);
    OP1('{', TOK_LBRACE);
    OP1('}', TOK_RBRACE);
    OP1('[', TOK_LBRACKET);
    OP1(']', TOK_RBRACKET);
    OP1(';', TOK_SEMICOLON);
    OP1(':', TOK_COLON);
    OP1(',', TOK_COMMA);

#undef OP3
#undef OP2
#undef OP1

    diag_error(loc, "unexpected character '%c' (0x%02X)", c, (u8)c);
    return make_tok(l, TOK_ERROR, start, 1, loc);
}

/* =========================================================================
 * Public API
 * ========================================================================= */
Lexer *lexer_new(const char *src, usize len, const char *filename, Arena *arena) {
    Lexer *l = arena_calloc(arena, sizeof(Lexer));
    l->src      = src;
    l->src_len  = len;
    l->pos      = 0;
    l->line     = 1;
    l->col      = 1;
    l->filename = filename;
    l->arena    = arena;
    return l;
}

void lexer_free(Lexer *l) {
    (void)l; /* arena-managed */
}

static void fill_peek(Lexer *l) {
    while (l->peek_count < LEXER_PEEK_BUF) {
        int slot = (l->peek_head + l->peek_count) % LEXER_PEEK_BUF;
        l->peek[slot] = scan_token(l);
        l->peek_count++;
        if (l->peek[slot].kind == TOK_EOF) break;
    }
}

Token lexer_peek_n(Lexer *l, int n) {
    fill_peek(l);
    if (n >= l->peek_count) {
        /* Return EOF */
        int slot = (l->peek_head + l->peek_count - 1) % LEXER_PEEK_BUF;
        return l->peek[slot];
    }
    return l->peek[(l->peek_head + n) % LEXER_PEEK_BUF];
}

Token lexer_next(Lexer *l) {
    fill_peek(l);
    Token t = l->peek[l->peek_head];
    if (l->peek_count > 0) {
        l->peek_head = (l->peek_head + 1) % LEXER_PEEK_BUF;
        l->peek_count--;
    }
    return t;
}

/* =========================================================================
 * Debug helpers
 * ========================================================================= */
const char *tok_kind_name(TokKind k) {
    switch (k) {
        case TOK_INT_LIT:      return "INT_LIT";
        case TOK_FLOAT_LIT:    return "FLOAT_LIT";
        case TOK_CHAR_LIT:     return "CHAR_LIT";
        case TOK_STR_LIT:      return "STR_LIT";
        case TOK_TRUE:         return "true";
        case TOK_FALSE:        return "false";
        case TOK_NULL:         return "null";
        case TOK_IDENT:        return "IDENT";
        case TOK_FN:           return "fn";
        case TOK_LET:          return "let";
        case TOK_VAR:          return "var";
        case TOK_STRUCT:       return "struct";
        case TOK_EXTERN:       return "extern";
        case TOK_IF:           return "if";
        case TOK_ELSE:         return "else";
        case TOK_WHILE:        return "while";
        case TOK_FOR:          return "for";
        case TOK_RETURN:       return "return";
        case TOK_BREAK:        return "break";
        case TOK_CONTINUE:     return "continue";
        case TOK_AS:           return "as";
        case TOK_SIZEOF:       return "sizeof";
        case TOK_I8:           return "i8";
        case TOK_I16:          return "i16";
        case TOK_I32:          return "i32";
        case TOK_I64:          return "i64";
        case TOK_U8:           return "u8";
        case TOK_U16:          return "u16";
        case TOK_U32:          return "u32";
        case TOK_U64:          return "u64";
        case TOK_F32:          return "f32";
        case TOK_F64:          return "f64";
        case TOK_BOOL:         return "bool";
        case TOK_CHAR_KW:      return "char";
        case TOK_VOID:         return "void";
        case TOK_PLUS:         return "+";
        case TOK_MINUS:        return "-";
        case TOK_STAR:         return "*";
        case TOK_SLASH:        return "/";
        case TOK_PERCENT:      return "%";
        case TOK_AMP:          return "&";
        case TOK_PIPE:         return "|";
        case TOK_CARET:        return "^";
        case TOK_TILDE:        return "~";
        case TOK_BANG:         return "!";
        case TOK_LT:           return "<";
        case TOK_GT:           return ">";
        case TOK_EQ:           return "=";
        case TOK_EQ_EQ:        return "==";
        case TOK_BANG_EQ:      return "!=";
        case TOK_LT_EQ:        return "<=";
        case TOK_GT_EQ:        return ">=";
        case TOK_AMP_AMP:      return "&&";
        case TOK_PIPE_PIPE:    return "||";
        case TOK_LSHIFT:       return "<<";
        case TOK_RSHIFT:       return ">>";
        case TOK_PLUS_EQ:      return "+=";
        case TOK_MINUS_EQ:     return "-=";
        case TOK_STAR_EQ:      return "*=";
        case TOK_SLASH_EQ:     return "/=";
        case TOK_PERCENT_EQ:   return "%=";
        case TOK_AMP_EQ:       return "&=";
        case TOK_PIPE_EQ:      return "|=";
        case TOK_CARET_EQ:     return "^=";
        case TOK_LSHIFT_EQ:    return "<<=";
        case TOK_RSHIFT_EQ:    return ">>=";
        case TOK_PLUS_PLUS:    return "++";
        case TOK_MINUS_MINUS:  return "--";
        case TOK_ARROW:        return "->";
        case TOK_DOT:          return ".";
        case TOK_ELLIPSIS:     return "...";
        case TOK_LPAREN:       return "(";
        case TOK_RPAREN:       return ")";
        case TOK_LBRACE:       return "{";
        case TOK_RBRACE:       return "}";
        case TOK_LBRACKET:     return "[";
        case TOK_RBRACKET:     return "]";
        case TOK_SEMICOLON:    return ";";
        case TOK_COLON:        return ":";
        case TOK_COMMA:        return ",";
        case TOK_EOF:          return "EOF";
        case TOK_ERROR:        return "ERROR";
        default:               return "?";
    }
}

void tok_dump(const Token *t, FILE *out) {
    fprintf(out, "[%s:%d:%d] %-14s ",
            t->loc.file ? t->loc.file : "?",
            t->loc.line, t->loc.col,
            tok_kind_name(t->kind));
    if (t->kind == TOK_INT_LIT)
        fprintf(out, "%lld", (long long)t->int_val);
    else if (t->kind == TOK_FLOAT_LIT)
        fprintf(out, "%g", t->flt_val);
    else if (t->text && t->text_len)
        fprintf(out, "%.*s", (int)t->text_len, t->text);
    fputc('\n', out);
}
