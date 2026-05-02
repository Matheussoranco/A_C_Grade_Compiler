/*
 * ast.c — AST node constructors and pretty-printer.
 */
#include "../include/ast.h"

/* =========================================================================
 * Convenience: allocate + zero a node, then set kind + loc
 * ========================================================================= */
static AstNode *node(Arena *a, AstKind k, SrcLoc loc) {
    AstNode *n = arena_calloc(a, sizeof(AstNode));
    n->kind = k;
    n->loc  = loc;
    return n;
}

/* =========================================================================
 * Constructors
 * ========================================================================= */
AstNode *ast_program(Arena *a, Vec *decls, SrcLoc loc) {
    AstNode *n = node(a, AST_PROGRAM, loc);
    n->program.decls = decls;
    return n;
}

AstNode *ast_fn_decl(Arena *a, const char *name, Param *params, usize np,
                     Type *ret, AstNode *body, bool is_extern, bool variadic, SrcLoc loc) {
    AstNode *n = node(a, AST_FN_DECL, loc);
    n->fn.name      = name;
    n->fn.params    = params;
    n->fn.n_params  = np;
    n->fn.ret_type  = ret;
    n->fn.body      = body;
    n->fn.is_extern = is_extern;
    n->fn.variadic  = variadic;
    return n;
}

AstNode *ast_struct_decl(Arena *a, const char *name, AstField *fields, usize nf, SrcLoc loc) {
    AstNode *n = node(a, AST_STRUCT_DECL, loc);
    n->strct.name     = name;
    n->strct.fields   = fields;
    n->strct.n_fields = nf;
    return n;
}

AstNode *ast_global_let(Arena *a, const char *name, Type *annot, AstNode *init,
                         bool is_const, SrcLoc loc) {
    AstNode *n = node(a, AST_GLOBAL_LET, loc);
    n->global.name     = name;
    n->global.annot    = annot;
    n->global.init     = init;
    n->global.is_const = is_const;
    return n;
}

AstNode *ast_block(Arena *a, Vec *stmts, SrcLoc loc) {
    AstNode *n = node(a, AST_BLOCK, loc);
    n->block.stmts = stmts;
    return n;
}

AstNode *ast_let(Arena *a, const char *name, Type *annot, AstNode *init, SrcLoc loc) {
    AstNode *n = node(a, AST_LET_STMT, loc);
    n->local.name  = name;
    n->local.annot = annot;
    n->local.init  = init;
    return n;
}

AstNode *ast_var(Arena *a, const char *name, Type *annot, AstNode *init, SrcLoc loc) {
    AstNode *n = node(a, AST_VAR_STMT, loc);
    n->local.name  = name;
    n->local.annot = annot;
    n->local.init  = init;
    return n;
}

AstNode *ast_assign(Arena *a, TokKind op, AstNode *lhs, AstNode *rhs, SrcLoc loc) {
    AstNode *n = node(a, op == TOK_EQ ? AST_ASSIGN : AST_COMPOUND_ASSIGN, loc);
    n->assign.op  = op;
    n->assign.lhs = lhs;
    n->assign.rhs = rhs;
    return n;
}

AstNode *ast_if(Arena *a, AstNode *cond, AstNode *then, AstNode *else_, SrcLoc loc) {
    AstNode *n = node(a, AST_IF, loc);
    n->if_.cond  = cond;
    n->if_.then  = then;
    n->if_.else_ = else_;
    return n;
}

AstNode *ast_while(Arena *a, AstNode *cond, AstNode *body, SrcLoc loc) {
    AstNode *n = node(a, AST_WHILE, loc);
    n->while_.cond = cond;
    n->while_.body = body;
    return n;
}

AstNode *ast_for(Arena *a, AstNode *init, AstNode *cond, AstNode *step, AstNode *body, SrcLoc loc) {
    AstNode *n = node(a, AST_FOR, loc);
    n->for_.init = init;
    n->for_.cond = cond;
    n->for_.step = step;
    n->for_.body = body;
    return n;
}

AstNode *ast_return(Arena *a, AstNode *val, SrcLoc loc) {
    AstNode *n = node(a, AST_RETURN, loc);
    n->ret.val = val;
    return n;
}

AstNode *ast_break(Arena *a, SrcLoc loc) { return node(a, AST_BREAK, loc); }
AstNode *ast_continue(Arena *a, SrcLoc loc) { return node(a, AST_CONTINUE, loc); }

AstNode *ast_expr_stmt(Arena *a, AstNode *expr, SrcLoc loc) {
    AstNode *n = node(a, AST_EXPR_STMT, loc);
    n->expr_stmt.expr = expr;
    return n;
}

AstNode *ast_int_lit(Arena *a, i64 v, SrcLoc loc) {
    AstNode *n = node(a, AST_INT_LIT, loc);
    n->int_lit.val = v;
    return n;
}

AstNode *ast_float_lit(Arena *a, f64 v, SrcLoc loc) {
    AstNode *n = node(a, AST_FLOAT_LIT, loc);
    n->flt_lit.val = v;
    return n;
}

AstNode *ast_bool_lit(Arena *a, bool v, SrcLoc loc) {
    AstNode *n = node(a, AST_BOOL_LIT, loc);
    n->bool_lit.val = v;
    return n;
}

AstNode *ast_char_lit(Arena *a, i32 v, SrcLoc loc) {
    AstNode *n = node(a, AST_CHAR_LIT, loc);
    n->char_lit.val = v;
    return n;
}

AstNode *ast_str_lit(Arena *a, const char *v, usize len, SrcLoc loc) {
    AstNode *n = node(a, AST_STR_LIT, loc);
    n->str_lit.val = v;
    n->str_lit.len = len;
    return n;
}

AstNode *ast_null_lit(Arena *a, SrcLoc loc) { return node(a, AST_NULL_LIT, loc); }

AstNode *ast_ident(Arena *a, const char *name, SrcLoc loc) {
    AstNode *n = node(a, AST_IDENT, loc);
    n->ident.name = name;
    return n;
}

AstNode *ast_unary(Arena *a, TokKind op, AstNode *operand, SrcLoc loc) {
    AstNode *n = node(a, AST_UNARY, loc);
    n->unary.op      = op;
    n->unary.operand = operand;
    return n;
}

AstNode *ast_binary(Arena *a, TokKind op, AstNode *lhs, AstNode *rhs, SrcLoc loc) {
    AstNode *n = node(a, AST_BINARY, loc);
    n->binary.op  = op;
    n->binary.lhs = lhs;
    n->binary.rhs = rhs;
    return n;
}

AstNode *ast_ternary(Arena *a, AstNode *cond, AstNode *then, AstNode *else_, SrcLoc loc) {
    AstNode *n = node(a, AST_TERNARY, loc);
    n->ternary.cond  = cond;
    n->ternary.then  = then;
    n->ternary.else_ = else_;
    return n;
}

AstNode *ast_call(Arena *a, AstNode *callee, Vec *args, SrcLoc loc) {
    AstNode *n = node(a, AST_CALL, loc);
    n->call.callee = callee;
    n->call.args   = args;
    return n;
}

AstNode *ast_index(Arena *a, AstNode *base, AstNode *idx, SrcLoc loc) {
    AstNode *n = node(a, AST_INDEX, loc);
    n->index.base = base;
    n->index.idx  = idx;
    return n;
}

AstNode *ast_field(Arena *a, AstNode *base, const char *field, bool arrow, SrcLoc loc) {
    AstNode *n = node(a, arrow ? AST_ARROW : AST_FIELD, loc);
    n->member.base  = base;
    n->member.field = field;
    return n;
}

AstNode *ast_cast(Arena *a, AstNode *expr, Type *to, SrcLoc loc) {
    AstNode *n = node(a, AST_CAST, loc);
    n->cast.expr = expr;
    n->cast.to   = to;
    return n;
}

AstNode *ast_sizeof(Arena *a, Type *of, SrcLoc loc) {
    AstNode *n = node(a, AST_SIZEOF_EXPR, loc);
    n->sizeof_.of = of;
    return n;
}

AstNode *ast_addr(Arena *a, AstNode *expr, SrcLoc loc) {
    AstNode *n = node(a, AST_ADDR, loc);
    n->unop.expr = expr;
    return n;
}

AstNode *ast_deref(Arena *a, AstNode *expr, SrcLoc loc) {
    AstNode *n = node(a, AST_DEREF, loc);
    n->unop.expr = expr;
    return n;
}

AstNode *ast_pre_inc(Arena *a, AstNode *expr, bool is_dec, SrcLoc loc) {
    AstNode *n = node(a, is_dec ? AST_PRE_DEC : AST_PRE_INC, loc);
    n->unop.expr = expr;
    return n;
}

AstNode *ast_post_inc(Arena *a, AstNode *expr, bool is_dec, SrcLoc loc) {
    AstNode *n = node(a, is_dec ? AST_POST_DEC : AST_POST_INC, loc);
    n->unop.expr = expr;
    return n;
}

/* =========================================================================
 * Pretty-printer
 * ========================================================================= */
static void indent(int d, FILE *out) {
    for (int i = 0; i < d * 2; i++) fputc(' ', out);
}

void ast_dump(const AstNode *n, int d, FILE *out) {
    if (!n) { indent(d, out); fputs("(null)\n", out); return; }
    indent(d, out);
    switch (n->kind) {
        case AST_PROGRAM:
            fprintf(out, "Program (%zu decls)\n", n->program.decls->len);
            for (usize i = 0; i < n->program.decls->len; i++)
                ast_dump(vec_at(n->program.decls, i), d + 1, out);
            break;
        case AST_FN_DECL:
            fprintf(out, "FnDecl '%s'%s\n", n->fn.name,
                    n->fn.is_extern ? " [extern]" : "");
            if (n->fn.body) ast_dump(n->fn.body, d + 1, out);
            break;
        case AST_STRUCT_DECL:
            fprintf(out, "StructDecl '%s' (%zu fields)\n",
                    n->strct.name, n->strct.n_fields);
            break;
        case AST_GLOBAL_LET:
            fprintf(out, "GlobalLet '%s'%s\n", n->global.name,
                    n->global.is_const ? " [const]" : "");
            if (n->global.init) ast_dump(n->global.init, d + 1, out);
            break;
        case AST_BLOCK:
            fprintf(out, "Block (%zu stmts)\n", n->block.stmts->len);
            for (usize i = 0; i < n->block.stmts->len; i++)
                ast_dump(vec_at(n->block.stmts, i), d + 1, out);
            break;
        case AST_LET_STMT: case AST_VAR_STMT:
            fprintf(out, "%s '%s'\n",
                    n->kind == AST_LET_STMT ? "Let" : "Var", n->local.name);
            if (n->local.init) ast_dump(n->local.init, d + 1, out);
            break;
        case AST_ASSIGN: case AST_COMPOUND_ASSIGN:
            fprintf(out, "Assign (%s)\n", tok_kind_name(n->assign.op));
            ast_dump(n->assign.lhs, d + 1, out);
            ast_dump(n->assign.rhs, d + 1, out);
            break;
        case AST_IF:
            fputs("If\n", out);
            ast_dump(n->if_.cond,  d + 1, out);
            ast_dump(n->if_.then,  d + 1, out);
            if (n->if_.else_) ast_dump(n->if_.else_, d + 1, out);
            break;
        case AST_WHILE:
            fputs("While\n", out);
            ast_dump(n->while_.cond, d + 1, out);
            ast_dump(n->while_.body, d + 1, out);
            break;
        case AST_FOR:
            fputs("For\n", out);
            ast_dump(n->for_.init, d + 1, out);
            ast_dump(n->for_.cond, d + 1, out);
            ast_dump(n->for_.step, d + 1, out);
            ast_dump(n->for_.body, d + 1, out);
            break;
        case AST_RETURN:
            fputs("Return\n", out);
            if (n->ret.val) ast_dump(n->ret.val, d + 1, out);
            break;
        case AST_BREAK:    fputs("Break\n",    out); break;
        case AST_CONTINUE: fputs("Continue\n", out); break;
        case AST_EXPR_STMT:
            fputs("ExprStmt\n", out);
            ast_dump(n->expr_stmt.expr, d + 1, out);
            break;
        case AST_INT_LIT:
            fprintf(out, "Int(%lld)\n", (long long)n->int_lit.val); break;
        case AST_FLOAT_LIT:
            fprintf(out, "Float(%g)\n",  n->flt_lit.val); break;
        case AST_BOOL_LIT:
            fprintf(out, "Bool(%s)\n",   n->bool_lit.val ? "true" : "false"); break;
        case AST_CHAR_LIT:
            fprintf(out, "Char(%d)\n",   n->char_lit.val); break;
        case AST_STR_LIT:
            fprintf(out, "Str(\"%.*s\")\n", (int)n->str_lit.len, n->str_lit.val); break;
        case AST_NULL_LIT:
            fputs("Null\n", out); break;
        case AST_IDENT:
            fprintf(out, "Ident('%s')\n", n->ident.name); break;
        case AST_UNARY:
            fprintf(out, "Unary(%s)\n", tok_kind_name(n->unary.op));
            ast_dump(n->unary.operand, d + 1, out);
            break;
        case AST_BINARY:
            fprintf(out, "Binary(%s)\n", tok_kind_name(n->binary.op));
            ast_dump(n->binary.lhs, d + 1, out);
            ast_dump(n->binary.rhs, d + 1, out);
            break;
        case AST_TERNARY:
            fputs("Ternary\n", out);
            ast_dump(n->ternary.cond,  d + 1, out);
            ast_dump(n->ternary.then,  d + 1, out);
            ast_dump(n->ternary.else_, d + 1, out);
            break;
        case AST_CALL:
            fputs("Call\n", out);
            ast_dump(n->call.callee, d + 1, out);
            for (usize i = 0; i < n->call.args->len; i++)
                ast_dump(vec_at(n->call.args, i), d + 2, out);
            break;
        case AST_INDEX:
            fputs("Index\n", out);
            ast_dump(n->index.base, d + 1, out);
            ast_dump(n->index.idx,  d + 1, out);
            break;
        case AST_FIELD:
            fprintf(out, "Field('.%s')\n", n->member.field);
            ast_dump(n->member.base, d + 1, out);
            break;
        case AST_ARROW:
            fprintf(out, "Arrow('->%s')\n", n->member.field);
            ast_dump(n->member.base, d + 1, out);
            break;
        case AST_CAST:
            fputs("Cast\n", out);
            ast_dump(n->cast.expr, d + 1, out);
            break;
        case AST_SIZEOF_EXPR:
            fputs("Sizeof\n", out); break;
        case AST_ADDR:
            fputs("Addr\n", out);
            ast_dump(n->unop.expr, d + 1, out);
            break;
        case AST_DEREF:
            fputs("Deref\n", out);
            ast_dump(n->unop.expr, d + 1, out);
            break;
        case AST_PRE_INC:  fputs("PreInc\n",  out); ast_dump(n->unop.expr,d+1,out); break;
        case AST_PRE_DEC:  fputs("PreDec\n",  out); ast_dump(n->unop.expr,d+1,out); break;
        case AST_POST_INC: fputs("PostInc\n", out); ast_dump(n->unop.expr,d+1,out); break;
        case AST_POST_DEC: fputs("PostDec\n", out); ast_dump(n->unop.expr,d+1,out); break;
        default:
            fprintf(out, "AstNode(kind=%d)\n", (int)n->kind);
    }
}
