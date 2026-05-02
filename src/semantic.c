/*
 * semantic.c — Semantic analysis: name resolution, type checking,
 *               constant folding, and control-flow validation.
 */
#include "../include/semantic.h"

/* =========================================================================
 * Context helpers
 * ========================================================================= */
SemaCtx *sema_new(Arena *arena, TypeCtx *types) {
    SemaCtx *ctx = arena_calloc(arena, sizeof(SemaCtx));
    ctx->arena   = arena;
    ctx->types   = types;
    ctx->symtab  = symtab_new(arena);
    ctx->str_lits = vec_new();
    return ctx;
}

/* =========================================================================
 * Forward declarations (all mutually recursive)
 * ========================================================================= */
static Type *sema_expr(SemaCtx *ctx, AstNode *n);
static void  sema_stmt(SemaCtx *ctx, AstNode *n);
static void  sema_block(SemaCtx *ctx, AstNode *n);
static void  sema_fn(SemaCtx *ctx, AstNode *n);

/* =========================================================================
 * Type-checking helpers
 * ========================================================================= */
static void require_numeric(SemaCtx *ctx, AstNode *n, const char *ctx_str) {
    if (n->type && !ty_is_numeric(n->type))
        diag_error(n->loc, "%s requires a numeric type, got '%s'",
                   ctx_str, ty_to_str(n->type));
}

static void require_integer(SemaCtx *ctx, AstNode *n, const char *ctx_str) {
    if (n->type && !ty_is_integer(n->type))
        diag_error(n->loc, "%s requires an integer type, got '%s'",
                   ctx_str, ty_to_str(n->type));
    (void)ctx;
}

static void require_scalar(SemaCtx *ctx, AstNode *n, const char *ctx_str) {
    if (n->type && !ty_is_scalar(n->type))
        diag_error(n->loc, "%s requires a scalar type, got '%s'",
                   ctx_str, ty_to_str(n->type));
    (void)ctx;
}

static bool is_lvalue(const AstNode *n) {
    switch (n->kind) {
        case AST_IDENT:
        case AST_DEREF:
        case AST_INDEX:
        case AST_FIELD:
        case AST_ARROW:
            return true;
        case AST_PRE_INC: case AST_PRE_DEC:
            return true;
        default:
            return false;
    }
}

/* Resolve TY_UNRESOLVED types (struct forward refs) */
static Type *resolve_type(SemaCtx *ctx, Type *t, SrcLoc loc) {
    if (!t) return ty_void(ctx->types);
    if (t->kind == TY_UNRESOLVED) {
        Type *resolved = ty_struct_lookup(ctx->types, t->unresolved_name);
        if (!resolved) {
            diag_error(loc, "undeclared type '%s'", t->unresolved_name);
            return ty_void(ctx->types);
        }
        return resolved;
    }
    if (t->kind == TY_PTR && t->ptr_base->kind == TY_UNRESOLVED) {
        Type *base = resolve_type(ctx, t->ptr_base, loc);
        return ty_ptr(ctx->types, base);
    }
    return t;
}

/* =========================================================================
 * Struct registration pass — collect all struct names before checking bodies.
 * ========================================================================= */
static void collect_struct(SemaCtx *ctx, AstNode *n) {
    /* Create a forward-declared struct type */
    const char *name = n->strct.name;
    if (ty_struct_lookup(ctx->types, name)) {
        diag_error(n->loc, "redefinition of struct '%s'", name);
        return;
    }
    Type *ty = ty_struct_new(ctx->types, name);

    /* Resolve fields and complete the struct */
    StructField *fields = NULL;
    usize nf = n->strct.n_fields;
    if (nf > 0) {
        fields = arena_alloc(ctx->arena, nf * sizeof(StructField));
        for (usize i = 0; i < nf; i++) {
            fields[i].name   = n->strct.fields[i].name;
            fields[i].type   = resolve_type(ctx, n->strct.fields[i].type,
                                             n->strct.fields[i].loc);
            fields[i].offset = 0; /* filled by ty_struct_complete */
        }
    }
    ty_struct_complete(ctx->types, ty, fields, nf);

    symtab_declare(ctx->symtab, SYM_STRUCT, name, ty, n->loc);
    n->type = ty;
}

/* =========================================================================
 * Function signature registration — first pass
 * ========================================================================= */
static void collect_fn(SemaCtx *ctx, AstNode *n) {
    usize np = n->fn.n_params;
    Type **ptypes = np > 0 ? arena_alloc(ctx->arena, np * sizeof(Type*)) : NULL;
    for (usize i = 0; i < np; i++)
        ptypes[i] = resolve_type(ctx, n->fn.params[i].type, n->fn.params[i].loc);

    Type *ret = resolve_type(ctx, n->fn.ret_type, n->loc);
    n->fn.ret_type = ret;
    Type *fn_type  = ty_fn(ctx->types, ret, ptypes, np, n->fn.variadic);
    n->type = fn_type;

    Symbol *sym = symtab_declare(ctx->symtab, SYM_FN, n->fn.name, fn_type, n->loc);
    if (sym) { sym->decl_node = n; n->sym = sym; }
}

/* =========================================================================
 * Global variable registration
 * ========================================================================= */
static void collect_global(SemaCtx *ctx, AstNode *n) {
    Type *ty = n->global.annot
             ? resolve_type(ctx, n->global.annot, n->loc)
             : NULL; /* type inferred from init later */

    Symbol *sym = symtab_declare(ctx->symtab, SYM_CONST, n->global.name, ty, n->loc);
    if (sym) { sym->decl_node = n; sym->is_global = true; n->sym = sym; }
}

/* =========================================================================
 * Expression type-checker
 * ========================================================================= */
static Type *sema_expr(SemaCtx *ctx, AstNode *n) {
    if (!n) return ty_void(ctx->types);
    switch (n->kind) {
        /* --- Literals --- */
        case AST_INT_LIT:
            n->type = ty_i64(ctx->types);
            return n->type;
        case AST_FLOAT_LIT:
            n->type = ty_f64(ctx->types);
            return n->type;
        case AST_BOOL_LIT:
            n->type = ty_bool(ctx->types);
            return n->type;
        case AST_CHAR_LIT:
            n->type = ty_char(ctx->types);
            return n->type;
        case AST_STR_LIT:
            n->type = ty_ptr(ctx->types, ty_char(ctx->types));
            vec_push(ctx->str_lits, (void*)n->str_lit.val);
            return n->type;
        case AST_NULL_LIT:
            n->type = ty_ptr(ctx->types, ty_void(ctx->types));
            return n->type;

        /* --- Identifier --- */
        case AST_IDENT: {
            Symbol *sym = symtab_lookup(ctx->symtab, n->ident.name);
            if (!sym) {
                diag_error(n->loc, "undeclared identifier '%s'", n->ident.name);
                n->type = ty_i32(ctx->types);
                return n->type;
            }
            n->sym  = sym;
            n->type = sym->type;
            return n->type;
        }

        /* --- Unary operators --- */
        case AST_UNARY: {
            Type *ot = sema_expr(ctx, n->unary.operand);
            switch (n->unary.op) {
                case TOK_BANG:
                    n->type = ty_bool(ctx->types);
                    break;
                case TOK_TILDE:
                    require_integer(ctx, n->unary.operand, "bitwise not");
                    n->type = ot;
                    break;
                case TOK_MINUS:
                    require_numeric(ctx, n->unary.operand, "negation");
                    n->type = ot;
                    break;
                default:
                    n->type = ot;
                    break;
            }
            return n->type;
        }

        /* --- Address-of / deref --- */
        case AST_ADDR: {
            Type *ot = sema_expr(ctx, n->unop.expr);
            if (!is_lvalue(n->unop.expr))
                diag_error(n->loc, "cannot take address of rvalue");
            n->type = ty_ptr(ctx->types, ot);
            return n->type;
        }
        case AST_DEREF: {
            Type *ot = sema_expr(ctx, n->unop.expr);
            if (ot->kind != TY_PTR) {
                diag_error(n->loc, "cannot dereference non-pointer type '%s'", ty_to_str(ot));
                n->type = ty_void(ctx->types);
            } else {
                n->type = ot->ptr_base;
            }
            return n->type;
        }

        /* --- Pre/post increment/decrement --- */
        case AST_PRE_INC: case AST_PRE_DEC:
        case AST_POST_INC: case AST_POST_DEC: {
            Type *ot = sema_expr(ctx, n->unop.expr);
            if (!is_lvalue(n->unop.expr))
                diag_error(n->loc, "increment/decrement requires an lvalue");
            if (!ty_is_scalar(ot))
                diag_error(n->loc, "increment/decrement requires a scalar type");
            n->type = ot;
            return n->type;
        }

        /* --- Binary operators --- */
        case AST_BINARY: {
            Type *lt = sema_expr(ctx, n->binary.lhs);
            Type *rt = sema_expr(ctx, n->binary.rhs);
            switch (n->binary.op) {
                /* Arithmetic */
                case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
                case TOK_SLASH: case TOK_PERCENT:
                    require_numeric(ctx, n->binary.lhs, "binary arithmetic");
                    require_numeric(ctx, n->binary.rhs, "binary arithmetic");
                    n->type = ty_arith_common(ctx->types, lt, rt);
                    break;
                /* Bitwise */
                case TOK_AMP: case TOK_PIPE: case TOK_CARET:
                case TOK_LSHIFT: case TOK_RSHIFT:
                    require_integer(ctx, n->binary.lhs, "bitwise operator");
                    require_integer(ctx, n->binary.rhs, "bitwise operator");
                    n->type = ty_arith_common(ctx->types, lt, rt);
                    break;
                /* Logical */
                case TOK_AMP_AMP: case TOK_PIPE_PIPE:
                    n->type = ty_bool(ctx->types);
                    break;
                /* Comparison */
                case TOK_EQ_EQ: case TOK_BANG_EQ:
                case TOK_LT: case TOK_GT:
                case TOK_LT_EQ: case TOK_GT_EQ:
                    n->type = ty_bool(ctx->types);
                    break;
                default:
                    n->type = lt;
                    break;
            }
            return n->type;
        }

        /* --- Assignment / compound assignment --- */
        case AST_ASSIGN:
        case AST_COMPOUND_ASSIGN: {
            Type *lt = sema_expr(ctx, n->assign.lhs);
            Type *rt = sema_expr(ctx, n->assign.rhs);
            if (!is_lvalue(n->assign.lhs))
                diag_error(n->loc, "left-hand side of assignment is not an lvalue");
            /* Check immutability for 'let' bindings */
            if (n->assign.lhs->kind == AST_IDENT && n->assign.lhs->sym) {
                Symbol *sym = n->assign.lhs->sym;
                if (sym->is_const)
                    diag_error(n->loc, "'%s' is immutable (declared with 'let')", sym->name);
            }
            if (!ty_compatible(rt, lt))
                diag_error(n->loc, "type mismatch in assignment: cannot assign '%s' to '%s'",
                           ty_to_str(rt), ty_to_str(lt));
            n->type = lt;
            return n->type;
        }

        /* --- Ternary --- */
        case AST_TERNARY: {
            sema_expr(ctx, n->ternary.cond);
            Type *tt = sema_expr(ctx, n->ternary.then);
            Type *et = sema_expr(ctx, n->ternary.else_);
            if (!ty_compatible(tt, et))
                diag_error(n->loc, "branches of ternary have incompatible types");
            n->type = tt;
            return n->type;
        }

        /* --- Function call --- */
        case AST_CALL: {
            Type *callee_type = sema_expr(ctx, n->call.callee);
            Type *fn_type = callee_type;
            /* Unwrap pointer-to-function */
            if (fn_type->kind == TY_PTR && fn_type->ptr_base->kind == TY_FN)
                fn_type = fn_type->ptr_base;

            if (fn_type->kind != TY_FN) {
                diag_error(n->loc, "called object is not a function (type '%s')",
                           ty_to_str(callee_type));
                n->type = ty_void(ctx->types);
                return n->type;
            }

            usize n_args = n->call.args->len;
            usize n_params = fn_type->fn.n_params;
            bool variadic  = fn_type->fn.variadic;

            if (variadic ? n_args < n_params : n_args != n_params) {
                diag_error(n->loc, "wrong number of arguments: expected %zu, got %zu",
                           n_params, n_args);
            }

            for (usize i = 0; i < n_args; i++) {
                AstNode *arg = vec_at(n->call.args, i);
                sema_expr(ctx, arg);
                if (i < n_params) {
                    Type *pt = fn_type->fn.params[i];
                    if (arg->type && !ty_compatible(arg->type, pt))
                        diag_error(arg->loc,
                            "argument %zu: expected '%s', got '%s'",
                            i + 1, ty_to_str(pt), ty_to_str(arg->type));
                }
            }
            n->type = fn_type->fn.ret;
            return n->type;
        }

        /* --- Array index --- */
        case AST_INDEX: {
            Type *bt = sema_expr(ctx, n->index.base);
            sema_expr(ctx, n->index.idx);
            require_integer(ctx, n->index.idx, "array subscript");
            if (bt->kind == TY_PTR) {
                n->type = bt->ptr_base;
            } else if (bt->kind == TY_ARRAY) {
                n->type = bt->array.elem;
            } else {
                diag_error(n->loc, "subscript of non-array/pointer type '%s'", ty_to_str(bt));
                n->type = ty_void(ctx->types);
            }
            return n->type;
        }

        /* --- Struct field access --- */
        case AST_FIELD:
        case AST_ARROW: {
            Type *bt = sema_expr(ctx, n->member.base);
            Type *st = bt;
            if (n->kind == AST_ARROW) {
                if (bt->kind != TY_PTR) {
                    diag_error(n->loc, "'->' requires a pointer type, got '%s'", ty_to_str(bt));
                    n->type = ty_void(ctx->types);
                    return n->type;
                }
                st = bt->ptr_base;
            }
            if (st->kind != TY_STRUCT) {
                diag_error(n->loc, "member access on non-struct type '%s'", ty_to_str(st));
                n->type = ty_void(ctx->types);
                return n->type;
            }
            for (usize i = 0; i < st->s.n_fields; i++) {
                if (strcmp(st->s.fields[i].name, n->member.field) == 0) {
                    n->type = st->s.fields[i].type;
                    return n->type;
                }
            }
            diag_error(n->loc, "struct '%s' has no field '%s'",
                       st->s.name, n->member.field);
            n->type = ty_void(ctx->types);
            return n->type;
        }

        /* --- Cast --- */
        case AST_CAST: {
            Type *from = sema_expr(ctx, n->cast.expr);
            Type *to   = resolve_type(ctx, n->cast.to, n->loc);
            n->cast.to = to;
            /* Most numeric → numeric casts are allowed; validate others */
            if (!ty_is_scalar(from) && from->kind != TY_ARRAY)
                diag_error(n->loc, "cannot cast from '%s'", ty_to_str(from));
            if (!ty_is_scalar(to))
                diag_error(n->loc, "cannot cast to '%s'", ty_to_str(to));
            n->type = to;
            return n->type;
        }

        /* --- sizeof --- */
        case AST_SIZEOF_EXPR: {
            Type *of = resolve_type(ctx, n->sizeof_.of, n->loc);
            n->sizeof_.of = of;
            /* Constant-fold immediately */
            n->kind = AST_INT_LIT;
            n->int_lit.val = (i64)of->size;
            n->type = ty_u64(ctx->types);
            return n->type;
        }

        default:
            diag_error(n->loc, "sema_expr: unhandled node kind %d", (int)n->kind);
            n->type = ty_void(ctx->types);
            return n->type;
    }
}

/* =========================================================================
 * Statement type-checker
 * ========================================================================= */
static void sema_stmt(SemaCtx *ctx, AstNode *n) {
    if (!n) return;
    switch (n->kind) {
        case AST_BLOCK:
            sema_block(ctx, n);
            break;

        case AST_LET_STMT:
        case AST_VAR_STMT: {
            bool is_let = (n->kind == AST_LET_STMT);
            Type *annot = n->local.annot
                        ? resolve_type(ctx, n->local.annot, n->loc)
                        : NULL;
            Type *init_type = NULL;
            if (n->local.init)
                init_type = sema_expr(ctx, n->local.init);

            Type *var_type = annot ? annot : init_type;
            if (!var_type) {
                diag_error(n->loc, "cannot infer type of '%s'; provide an annotation", n->local.name);
                var_type = ty_i32(ctx->types);
            }
            if (annot && init_type && !ty_compatible(init_type, annot))
                diag_error(n->loc, "type mismatch: cannot initialize '%s' with '%s'",
                           ty_to_str(annot), ty_to_str(init_type));

            Symbol *sym = symtab_declare(ctx->symtab, SYM_VAR, n->local.name, var_type, n->loc);
            if (sym) {
                sym->is_const  = is_let;
                sym->decl_node = n;
                n->sym  = sym;
                n->type = var_type;
            }
            break;
        }

        case AST_ASSIGN:
        case AST_COMPOUND_ASSIGN:
            sema_expr(ctx, n);
            break;

        case AST_EXPR_STMT:
            sema_expr(ctx, n->expr_stmt.expr);
            break;

        case AST_IF:
            sema_expr(ctx, n->if_.cond);
            require_scalar(ctx, n->if_.cond, "if condition");
            sema_stmt(ctx, n->if_.then);
            if (n->if_.else_) sema_stmt(ctx, n->if_.else_);
            break;

        case AST_WHILE:
            ctx->loop_depth++;
            sema_expr(ctx, n->while_.cond);
            require_scalar(ctx, n->while_.cond, "while condition");
            sema_stmt(ctx, n->while_.body);
            ctx->loop_depth--;
            break;

        case AST_FOR:
            symtab_push_scope(ctx->symtab, false);
            ctx->loop_depth++;
            if (n->for_.init) sema_stmt(ctx, n->for_.init);
            if (n->for_.cond) {
                sema_expr(ctx, n->for_.cond);
                require_scalar(ctx, n->for_.cond, "for condition");
            }
            if (n->for_.step) sema_stmt(ctx, n->for_.step);
            sema_stmt(ctx, n->for_.body);
            ctx->loop_depth--;
            symtab_pop_scope(ctx->symtab);
            break;

        case AST_RETURN: {
            Type *ret_type = ctx->cur_fn_ret;
            if (n->ret.val) {
                Type *vt = sema_expr(ctx, n->ret.val);
                if (ret_type && ret_type->kind == TY_VOID && vt->kind != TY_VOID)
                    diag_error(n->loc, "void function should not return a value");
                else if (ret_type && ret_type->kind != TY_VOID && !ty_compatible(vt, ret_type))
                    diag_error(n->loc, "return type mismatch: expected '%s', got '%s'",
                               ty_to_str(ret_type), ty_to_str(vt));
            } else if (ret_type && ret_type->kind != TY_VOID) {
                diag_error(n->loc, "non-void function should return a value");
            }
            break;
        }

        case AST_BREAK:
        case AST_CONTINUE:
            if (ctx->loop_depth == 0)
                diag_error(n->loc, "'%s' outside of a loop",
                           n->kind == AST_BREAK ? "break" : "continue");
            break;

        default:
            diag_error(n->loc, "sema_stmt: unhandled node kind %d", (int)n->kind);
            break;
    }
}

static void sema_block(SemaCtx *ctx, AstNode *n) {
    symtab_push_scope(ctx->symtab, false);
    for (usize i = 0; i < n->block.stmts->len; i++)
        sema_stmt(ctx, vec_at(n->block.stmts, i));
    symtab_pop_scope(ctx->symtab);
}

/* =========================================================================
 * Function body checker
 * ========================================================================= */
static void sema_fn(SemaCtx *ctx, AstNode *n) {
    if (n->fn.is_extern) return;

    Type *fn_type = n->type; /* set in collect_fn */
    ctx->cur_fn_ret = fn_type ? fn_type->fn.ret : ty_void(ctx->types);

    symtab_push_scope(ctx->symtab, true);

    /* Declare parameters in this scope */
    for (usize i = 0; i < n->fn.n_params; i++) {
        Param *par = &n->fn.params[i];
        par->type = resolve_type(ctx, par->type, par->loc);
        Symbol *sym = symtab_declare(ctx->symtab, SYM_PARAM, par->name, par->type, par->loc);
        if (sym) { sym->decl_node = n; par->sym = sym; }
    }

    /* Type-check the body */
    if (n->fn.body) {
        /* Don't push another scope — block already does that */
        for (usize i = 0; i < n->fn.body->block.stmts->len; i++)
            sema_stmt(ctx, vec_at(n->fn.body->block.stmts, i));
    }

    symtab_pop_scope(ctx->symtab);
    ctx->cur_fn_ret = NULL;
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */
bool sema_analyze(SemaCtx *ctx, AstNode *program) {
    assert(program->kind == AST_PROGRAM);
    Vec *decls = program->program.decls;

    /* Pass 1: collect struct types */
    for (usize i = 0; i < decls->len; i++) {
        AstNode *d = vec_at(decls, i);
        if (d->kind == AST_STRUCT_DECL) collect_struct(ctx, d);
    }

    /* Pass 2: collect function and global signatures */
    for (usize i = 0; i < decls->len; i++) {
        AstNode *d = vec_at(decls, i);
        if (d->kind == AST_FN_DECL)    collect_fn(ctx, d);
        if (d->kind == AST_GLOBAL_LET) collect_global(ctx, d);
    }

    /* Pass 3: check global variable initializers */
    for (usize i = 0; i < decls->len; i++) {
        AstNode *d = vec_at(decls, i);
        if (d->kind == AST_GLOBAL_LET && d->global.init) {
            Type *init_type = sema_expr(ctx, d->global.init);
            if (!d->sym) continue;
            if (!d->sym->type) d->sym->type = init_type;
        }
    }

    /* Pass 4: check function bodies */
    for (usize i = 0; i < decls->len; i++) {
        AstNode *d = vec_at(decls, i);
        if (d->kind == AST_FN_DECL) sema_fn(ctx, d);
    }

    return !g_had_error;
}
