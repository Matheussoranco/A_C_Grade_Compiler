/*
 * irgen.c — AST → TAC IR lowering.
 *
 * Walks the semantically-annotated AST and emits IR instructions.
 * Strategy:
 *   - Each expression returns a VReg (or IOP_UNDEF for void).
 *   - Lvalue nodes return a pointer VReg; callers load/store through it.
 *   - Labels are created eagerly and "patched" by emitting IR_LABEL_DEF.
 */
#include "../include/irgen.h"
#include "../include/ir.h"
#include "../include/ast.h"

/* =========================================================================
 * Context helpers
 * ========================================================================= */
IrGenCtx *irgen_new(IrBuilder *b, Arena *arena, TypeCtx *types) {
    IrGenCtx *ctx = arena_calloc(arena, sizeof(IrGenCtx));
    ctx->b      = b;
    ctx->arena  = arena;
    ctx->types  = types;
    ctx->loop_cap = 16;
    ctx->break_stack = arena_alloc(arena, ctx->loop_cap * sizeof(i32));
    ctx->cont_stack  = arena_alloc(arena, ctx->loop_cap * sizeof(i32));
    return ctx;
}

static void push_loop(IrGenCtx *ctx, i32 brk, i32 cont) {
    assert(ctx->loop_sp < ctx->loop_cap);
    ctx->break_stack[ctx->loop_sp] = brk;
    ctx->cont_stack [ctx->loop_sp] = cont;
    ctx->loop_sp++;
}

static void pop_loop(IrGenCtx *ctx) {
    assert(ctx->loop_sp > 0);
    ctx->loop_sp--;
}

/* =========================================================================
 * Determine the IR binary opcode for an AST binary operator.
 * ========================================================================= */
static IrOp binop_for(TokKind tok, Type *t) {
    bool flt  = ty_is_float(t);
    bool sign = ty_is_signed(t);
    switch (tok) {
        case TOK_PLUS:     return flt ? IR_FADD : IR_ADD;
        case TOK_MINUS:    return flt ? IR_FSUB : IR_SUB;
        case TOK_STAR:     return flt ? IR_FMUL : IR_MUL;
        case TOK_SLASH:    return flt ? IR_FDIV : (sign ? IR_SDIV : IR_UDIV);
        case TOK_PERCENT:  return sign ? IR_SREM : IR_UREM;
        case TOK_AMP:      return IR_AND;
        case TOK_PIPE:     return IR_OR;
        case TOK_CARET:    return IR_XOR;
        case TOK_LSHIFT:   return IR_SHL;
        case TOK_RSHIFT:   return sign ? IR_ASHR : IR_LSHR;
        case TOK_AMP_AMP:  return IR_AND; /* short-circuit handled separately */
        case TOK_PIPE_PIPE:return IR_OR;
        /* Comparison → produces i32 0/1 */
        case TOK_EQ_EQ:    return flt ? IR_FEQ  : IR_EQ;
        case TOK_BANG_EQ:  return flt ? IR_FNE  : IR_NE;
        case TOK_LT:       return flt ? IR_FLT  : (sign ? IR_SLT : IR_ULT);
        case TOK_GT:       return flt ? IR_FGT  : (sign ? IR_SGT : IR_UGT);
        case TOK_LT_EQ:    return flt ? IR_FLE  : (sign ? IR_SLE : IR_ULE);
        case TOK_GT_EQ:    return flt ? IR_FGE  : (sign ? IR_SGE : IR_UGE);
        default:           return IR_ADD;
    }
}

/* Determine IR cast opcode */
static IrOp cast_op_for(Type *from, Type *to) {
    if (ty_is_float(from) && ty_is_integer(to)) return IR_FTOI;
    if (ty_is_integer(from) && ty_is_float(to))
        return ty_is_signed(from) ? IR_ITOF : IR_UITOF;
    if (ty_is_float(from) && ty_is_float(to))
        return from->size < to->size ? IR_FPEXT : IR_FPTRUNC;
    if (ty_is_integer(from) && ty_is_integer(to)) {
        if (from->size < to->size)
            return ty_is_signed(from) ? IR_SEXT : IR_ZEXT;
        if (from->size > to->size) return IR_TRUNC;
        return IR_MOV; /* same size */
    }
    if (from->kind == TY_PTR && to->kind == TY_PTR) return IR_BITCAST;
    if (from->kind == TY_PTR && ty_is_integer(to))  return IR_PTRTOINT;
    if (ty_is_integer(from)  && to->kind == TY_PTR) return IR_INTTOPTR;
    return IR_BITCAST;
}

/* =========================================================================
 * Core code generators — forward declarations
 * ========================================================================= */
static IOp  gen_expr(IrGenCtx *ctx, AstNode *n);        /* rvalue  */
static IOp  gen_lvalue(IrGenCtx *ctx, AstNode *n);      /* address */
static void gen_stmt(IrGenCtx *ctx, AstNode *n);

/* =========================================================================
 * lvalue address generation
 * ========================================================================= */
static IOp gen_lvalue(IrGenCtx *ctx, AstNode *n) {
    IrBuilder *b = ctx->b;
    switch (n->kind) {
        case AST_IDENT: {
            Symbol *sym = n->sym;
            assert(sym);
            if (sym->is_global) {
                VReg r = ir_lea(b, sym, n->loc);
                return iop_vreg(r);
            }
            /* Local: sym has a stack alloca vreg stored in ir_reg */
            VReg r; r.id = sym->ir_reg; r.type = ty_ptr(ctx->types, sym->type);
            r.name = sym->name;
            return iop_vreg(r);
        }
        case AST_DEREF: {
            IOp ptr = gen_expr(ctx, n->unop.expr);
            return ptr;
        }
        case AST_INDEX: {
            IOp base = gen_lvalue(ctx, n->index.base);
            IOp idx  = gen_expr(ctx, n->index.idx);
            Type *elem = n->index.base->type;
            if (elem->kind == TY_PTR)  elem = elem->ptr_base;
            else if (elem->kind == TY_ARRAY) elem = elem->array.elem;
            VReg ptr = ir_getidx(b, base, idx, elem, n->loc);
            return iop_vreg(ptr);
        }
        case AST_FIELD:
        case AST_ARROW: {
            IOp base_ptr;
            Type *st;
            if (n->kind == AST_FIELD) {
                base_ptr = gen_lvalue(ctx, n->member.base);
                st = n->member.base->type;
                if (st->kind == TY_PTR) st = st->ptr_base;
            } else {
                IOp base_val = gen_expr(ctx, n->member.base);
                base_ptr = base_val;
                st = n->member.base->type;
                if (st->kind == TY_PTR) st = st->ptr_base;
            }
            for (usize i = 0; i < st->s.n_fields; i++) {
                if (strcmp(st->s.fields[i].name, n->member.field) == 0) {
                    VReg fptr = ir_getfld(b, base_ptr, st->s.fields[i].offset,
                                          st->s.fields[i].type, n->loc);
                    return iop_vreg(fptr);
                }
            }
            /* Should never reach here after semantic pass */
            return iop_undef();
        }
        default:
            diag_error(n->loc, "irgen: invalid lvalue node %d", (int)n->kind);
            return iop_undef();
    }
}

/* =========================================================================
 * rvalue (expression) generation — returns an IOp holding the value.
 * ========================================================================= */
static IOp gen_expr(IrGenCtx *ctx, AstNode *n) {
    IrBuilder *b = ctx->b;
    if (!n) return iop_undef();

    switch (n->kind) {
        case AST_INT_LIT:
            return iop_imm_int(n->int_lit.val, n->type);
        case AST_FLOAT_LIT:
            return iop_imm_flt(n->flt_lit.val, n->type);
        case AST_BOOL_LIT:
            return iop_imm_int(n->bool_lit.val ? 1 : 0, n->type);
        case AST_CHAR_LIT:
            return iop_imm_int(n->char_lit.val, n->type);
        case AST_NULL_LIT:
            return iop_imm_int(0, n->type);
        case AST_STR_LIT: {
            /* String literals are stored as .Lstr_N labels */
            const char *lbl = ir_str_label(b->mod, n->str_lit.val, ctx->arena);
            IOp o = iop_imm_str(lbl, n->type);
            return o;
        }

        case AST_IDENT: {
            Symbol *sym = n->sym;
            if (!sym) return iop_undef();
            /* Functions: return a pointer to the function */
            if (sym->kind == SYM_FN) {
                return iop_global(sym);
            }
            /* Variables: load from address */
            IOp addr = gen_lvalue(ctx, n);
            VReg val = ir_load(b, addr, sym->type, n->loc);
            return iop_vreg(val);
        }

        case AST_UNARY: {
            IOp op = gen_expr(ctx, n->unary.operand);
            switch (n->unary.op) {
                case TOK_MINUS: {
                    VReg r = ir_unop(b, ty_is_float(n->type) ? IR_FNEG : IR_NEG, op, n->loc);
                    return iop_vreg(r);
                }
                case TOK_BANG: {
                    /* !x ≡ x == 0 */
                    IOp zero = iop_imm_int(0, n->unary.operand->type);
                    VReg r = ir_binop(b, IR_EQ, op, zero, n->loc);
                    return iop_vreg(r);
                }
                case TOK_TILDE: {
                    VReg r = ir_unop(b, IR_NOT, op, n->loc);
                    return iop_vreg(r);
                }
                default:
                    return op;
            }
        }

        case AST_ADDR: {
            IOp addr = gen_lvalue(ctx, n->unop.expr);
            return addr;
        }

        case AST_DEREF: {
            IOp ptr = gen_expr(ctx, n->unop.expr);
            VReg val = ir_load(b, ptr, n->type, n->loc);
            return iop_vreg(val);
        }

        case AST_PRE_INC: case AST_PRE_DEC: {
            IOp addr = gen_lvalue(ctx, n->unop.expr);
            VReg old = ir_load(b, addr, n->type, n->loc);
            IOp one  = ty_is_float(n->type)
                     ? iop_imm_flt(1.0, n->type)
                     : iop_imm_int(1, n->type);
            IrOp op = (n->kind == AST_PRE_INC)
                    ? (ty_is_float(n->type) ? IR_FADD : IR_ADD)
                    : (ty_is_float(n->type) ? IR_FSUB : IR_SUB);
            VReg new_ = ir_binop(b, op, iop_vreg(old), one, n->loc);
            ir_store(b, addr, iop_vreg(new_), n->loc);
            return iop_vreg(new_);
        }

        case AST_POST_INC: case AST_POST_DEC: {
            IOp addr = gen_lvalue(ctx, n->unop.expr);
            VReg old = ir_load(b, addr, n->type, n->loc);
            IOp one  = ty_is_float(n->type)
                     ? iop_imm_flt(1.0, n->type)
                     : iop_imm_int(1, n->type);
            IrOp op = (n->kind == AST_POST_INC)
                    ? (ty_is_float(n->type) ? IR_FADD : IR_ADD)
                    : (ty_is_float(n->type) ? IR_FSUB : IR_SUB);
            VReg new_ = ir_binop(b, op, iop_vreg(old), one, n->loc);
            ir_store(b, addr, iop_vreg(new_), n->loc);
            return iop_vreg(old); /* post: return original value */
        }

        case AST_BINARY: {
            /* Short-circuit for && and || */
            if (n->binary.op == TOK_AMP_AMP) {
                IOp lv = gen_expr(ctx, n->binary.lhs);
                VReg res = ir_alloca(b, n->type, n->loc);
                IOp zero = iop_imm_int(0, n->type);
                ir_store(b, iop_vreg(res), zero, n->loc);
                i32 rhs_lbl  = ir_label_new(b, "land.rhs");
                i32 done_lbl = ir_label_new(b, "land.done");
                ir_jmpif(b, lv, rhs_lbl, done_lbl, n->loc);
                ir_label_def(b, rhs_lbl, n->loc);
                IOp rv = gen_expr(ctx, n->binary.rhs);
                ir_store(b, iop_vreg(res), rv, n->loc);
                ir_jmp(b, done_lbl, n->loc);
                ir_label_def(b, done_lbl, n->loc);
                VReg r = ir_load(b, iop_vreg(res), n->type, n->loc);
                return iop_vreg(r);
            }
            if (n->binary.op == TOK_PIPE_PIPE) {
                IOp lv = gen_expr(ctx, n->binary.lhs);
                VReg res = ir_alloca(b, n->type, n->loc);
                IOp one = iop_imm_int(1, n->type);
                ir_store(b, iop_vreg(res), one, n->loc);
                i32 rhs_lbl  = ir_label_new(b, "lor.rhs");
                i32 done_lbl = ir_label_new(b, "lor.done");
                ir_jmpif(b, lv, done_lbl, rhs_lbl, n->loc);
                ir_label_def(b, rhs_lbl, n->loc);
                IOp rv = gen_expr(ctx, n->binary.rhs);
                ir_store(b, iop_vreg(res), rv, n->loc);
                ir_jmp(b, done_lbl, n->loc);
                ir_label_def(b, done_lbl, n->loc);
                VReg r = ir_load(b, iop_vreg(res), n->type, n->loc);
                return iop_vreg(r);
            }
            IOp lv = gen_expr(ctx, n->binary.lhs);
            IOp rv = gen_expr(ctx, n->binary.rhs);
            Type *common = n->type;
            IrOp op = binop_for(n->binary.op, common);
            VReg r = ir_binop(b, op, lv, rv, n->loc);
            return iop_vreg(r);
        }

        case AST_ASSIGN: {
            IOp rval = gen_expr(ctx, n->assign.rhs);
            IOp addr = gen_lvalue(ctx, n->assign.lhs);
            ir_store(b, addr, rval, n->loc);
            return rval;
        }

        case AST_COMPOUND_ASSIGN: {
            IOp addr = gen_lvalue(ctx, n->assign.lhs);
            VReg old = ir_load(b, addr, n->assign.lhs->type, n->loc);
            IOp rv   = gen_expr(ctx, n->assign.rhs);
            /* Map compound op to base op */
            TokKind base;
            switch (n->assign.op) {
                case TOK_PLUS_EQ:   base=TOK_PLUS;   break;
                case TOK_MINUS_EQ:  base=TOK_MINUS;  break;
                case TOK_STAR_EQ:   base=TOK_STAR;   break;
                case TOK_SLASH_EQ:  base=TOK_SLASH;  break;
                case TOK_PERCENT_EQ:base=TOK_PERCENT; break;
                case TOK_AMP_EQ:    base=TOK_AMP;    break;
                case TOK_PIPE_EQ:   base=TOK_PIPE;   break;
                case TOK_CARET_EQ:  base=TOK_CARET;  break;
                case TOK_LSHIFT_EQ: base=TOK_LSHIFT; break;
                case TOK_RSHIFT_EQ: base=TOK_RSHIFT; break;
                default:            base=TOK_PLUS;   break;
            }
            IrOp op = binop_for(base, n->assign.lhs->type);
            VReg new_ = ir_binop(b, op, iop_vreg(old), rv, n->loc);
            ir_store(b, addr, iop_vreg(new_), n->loc);
            return iop_vreg(new_);
        }

        case AST_TERNARY: {
            IOp cond = gen_expr(ctx, n->ternary.cond);
            VReg res = ir_alloca(b, n->type, n->loc);
            i32 then_lbl = ir_label_new(b, "tern.then");
            i32 else_lbl = ir_label_new(b, "tern.else");
            i32 done_lbl = ir_label_new(b, "tern.done");
            ir_jmpif(b, cond, then_lbl, else_lbl, n->loc);
            ir_label_def(b, then_lbl, n->loc);
            IOp tv = gen_expr(ctx, n->ternary.then);
            ir_store(b, iop_vreg(res), tv, n->loc);
            ir_jmp(b, done_lbl, n->loc);
            ir_label_def(b, else_lbl, n->loc);
            IOp ev = gen_expr(ctx, n->ternary.else_);
            ir_store(b, iop_vreg(res), ev, n->loc);
            ir_jmp(b, done_lbl, n->loc);
            ir_label_def(b, done_lbl, n->loc);
            VReg r = ir_load(b, iop_vreg(res), n->type, n->loc);
            return iop_vreg(r);
        }

        case AST_CALL: {
            IOp callee = gen_expr(ctx, n->call.callee);
            Vec *args = vec_new();
            for (usize i = 0; i < n->call.args->len; i++) {
                AstNode *arg = vec_at(n->call.args, i);
                IOp *a = arena_alloc(ctx->arena, sizeof(IOp));
                *a = gen_expr(ctx, arg);
                vec_push(args, a);
            }
            VReg r = ir_call(b, callee, args, n->type, n->loc);
            if (r.id < 0) return iop_undef();
            return iop_vreg(r);
        }

        case AST_INDEX: {
            IOp addr = gen_lvalue(ctx, n);
            VReg val = ir_load(b, addr, n->type, n->loc);
            return iop_vreg(val);
        }

        case AST_FIELD:
        case AST_ARROW: {
            IOp addr = gen_lvalue(ctx, n);
            VReg val = ir_load(b, addr, n->type, n->loc);
            return iop_vreg(val);
        }

        case AST_CAST: {
            IOp src = gen_expr(ctx, n->cast.expr);
            Type *from = n->cast.expr->type;
            Type *to   = n->cast.to;
            if (ty_equal(from, to)) return src;
            IrOp op = cast_op_for(from, to);
            if (op == IR_MOV) return ir_mov(b, src, n->loc), src;
            VReg r = ir_cast(b, op, src, to, n->loc);
            return iop_vreg(r);
        }

        case AST_SIZEOF_EXPR: /* already folded to AST_INT_LIT by sema */
            return iop_imm_int(n->int_lit.val, n->type);

        default:
            diag_error(n->loc, "irgen: unhandled expr node %d", (int)n->kind);
            return iop_undef();
    }
}

/* =========================================================================
 * Statement generation
 * ========================================================================= */
static void gen_stmt(IrGenCtx *ctx, AstNode *n) {
    IrBuilder *b = ctx->b;
    if (!n) return;
    switch (n->kind) {
        case AST_BLOCK:
            for (usize i = 0; i < n->block.stmts->len; i++)
                gen_stmt(ctx, vec_at(n->block.stmts, i));
            break;

        case AST_LET_STMT:
        case AST_VAR_STMT: {
            Symbol *sym = n->sym;
            if (!sym) break;
            /* Allocate stack slot */
            VReg ptr = ir_alloca(b, sym->type, n->loc);
            sym->ir_reg = ptr.id;
            if (n->local.init) {
                IOp val = gen_expr(ctx, n->local.init);
                ir_store(b, iop_vreg(ptr), val, n->loc);
            }
            break;
        }

        case AST_ASSIGN:
        case AST_COMPOUND_ASSIGN:
            gen_expr(ctx, n);
            break;

        case AST_EXPR_STMT:
            gen_expr(ctx, n->expr_stmt.expr);
            break;

        case AST_IF: {
            IOp cond = gen_expr(ctx, n->if_.cond);
            i32 then_lbl = ir_label_new(b, "if.then");
            i32 else_lbl = ir_label_new(b, "if.else");
            i32 done_lbl = n->if_.else_ ? ir_label_new(b, "if.done") : else_lbl;
            ir_jmpif(b, cond, then_lbl, else_lbl, n->loc);
            ir_label_def(b, then_lbl, n->loc);
            gen_stmt(ctx, n->if_.then);
            if (n->if_.else_) {
                ir_jmp(b, done_lbl, n->loc);
                ir_label_def(b, else_lbl, n->loc);
                gen_stmt(ctx, n->if_.else_);
            }
            ir_label_def(b, done_lbl, n->loc);
            break;
        }

        case AST_WHILE: {
            i32 cond_lbl = ir_label_new(b, "while.cond");
            i32 body_lbl = ir_label_new(b, "while.body");
            i32 done_lbl = ir_label_new(b, "while.done");
            push_loop(ctx, done_lbl, cond_lbl);
            ir_jmp(b, cond_lbl, n->loc);
            ir_label_def(b, cond_lbl, n->loc);
            IOp cond = gen_expr(ctx, n->while_.cond);
            ir_jmpif(b, cond, body_lbl, done_lbl, n->loc);
            ir_label_def(b, body_lbl, n->loc);
            gen_stmt(ctx, n->while_.body);
            ir_jmp(b, cond_lbl, n->loc);
            ir_label_def(b, done_lbl, n->loc);
            pop_loop(ctx);
            break;
        }

        case AST_FOR: {
            i32 cond_lbl = ir_label_new(b, "for.cond");
            i32 body_lbl = ir_label_new(b, "for.body");
            i32 step_lbl = ir_label_new(b, "for.step");
            i32 done_lbl = ir_label_new(b, "for.done");
            push_loop(ctx, done_lbl, step_lbl);
            if (n->for_.init) gen_stmt(ctx, n->for_.init);
            ir_jmp(b, cond_lbl, n->loc);
            ir_label_def(b, cond_lbl, n->loc);
            if (n->for_.cond) {
                IOp cond = gen_expr(ctx, n->for_.cond);
                ir_jmpif(b, cond, body_lbl, done_lbl, n->loc);
            } else {
                ir_jmp(b, body_lbl, n->loc);
            }
            ir_label_def(b, body_lbl, n->loc);
            gen_stmt(ctx, n->for_.body);
            ir_label_def(b, step_lbl, n->loc);
            if (n->for_.step) gen_stmt(ctx, n->for_.step);
            ir_jmp(b, cond_lbl, n->loc);
            ir_label_def(b, done_lbl, n->loc);
            pop_loop(ctx);
            break;
        }

        case AST_RETURN: {
            if (n->ret.val) {
                IOp val = gen_expr(ctx, n->ret.val);
                ir_ret(b, val, n->loc);
            } else {
                ir_ret(b, iop_undef(), n->loc);
            }
            /* Create a new unreachable block for any code after return */
            IrBlock *dead = ir_block_new(b->fn, "after.ret");
            ir_set_block(b, dead);
            break;
        }

        case AST_BREAK: {
            assert(ctx->loop_sp > 0);
            ir_jmp(b, ctx->break_stack[ctx->loop_sp - 1], n->loc);
            IrBlock *dead = ir_block_new(b->fn, "after.break");
            ir_set_block(b, dead);
            break;
        }

        case AST_CONTINUE: {
            assert(ctx->loop_sp > 0);
            ir_jmp(b, ctx->cont_stack[ctx->loop_sp - 1], n->loc);
            IrBlock *dead = ir_block_new(b->fn, "after.cont");
            ir_set_block(b, dead);
            break;
        }

        default:
            diag_error(n->loc, "irgen: unhandled stmt node %d", (int)n->kind);
            break;
    }
}

/* =========================================================================
 * Function IR generation
 * ========================================================================= */
static void gen_fn(IrGenCtx *ctx, AstNode *decl) {
    IrBuilder *b = ctx->b;
    if (decl->fn.is_extern) return;

    Symbol *sym   = decl->sym;
    IrFunc *fn    = ir_func_new(b->mod, decl->fn.name, decl->type, sym);
    IrBlock *entry = ir_block_new(fn, "entry");
    b->fn  = fn;
    b->cur = entry;

    /* Allocate slots for parameters */
    for (usize i = 0; i < decl->fn.n_params; i++) {
        Param *par = &decl->fn.params[i];
        if (!par->sym) continue;
        /* Allocate a local slot and store the incoming param value there.
         * Params are supplied as VRegs with ids starting at 0. */
        VReg param_vreg = ir_alloc_vreg(b, par->type, par->name);
        VReg slot_ptr   = ir_alloca(b, par->type, par->loc);
        par->sym->ir_reg = slot_ptr.id;
        /* Emit a move from the incoming param vreg to the slot */
        IrInst *mv = ir_emit(b, IR_MOV, param_vreg,
                             iop_imm_int((i64)i, ty_i32(ctx->types)),
                             iop_undef(), iop_undef(), par->loc);
        (void)mv;
        ir_store(b, iop_vreg(slot_ptr), iop_vreg(param_vreg), par->loc);
    }

    /* Generate body */
    if (decl->fn.body)
        gen_stmt(ctx, decl->fn.body);

    /* Implicit void return at end */
    if (!b->cur->tail || b->cur->tail->op != IR_RET)
        ir_ret(b, iop_undef(), decl->loc);

    b->fn  = NULL;
    b->cur = NULL;
}

/* =========================================================================
 * Module entry point
 * ========================================================================= */
IrModule *irgen_program(IrGenCtx *ctx, AstNode *program) {
    IrBuilder *b = ctx->b;
    assert(program->kind == AST_PROGRAM);

    /* Register global variables in module */
    for (usize i = 0; i < program->program.decls->len; i++) {
        AstNode *d = vec_at(program->program.decls, i);
        if (d->kind == AST_GLOBAL_LET && d->sym) {
            d->sym->is_global = true;
            vec_push(b->mod->globals, d->sym);
        }
    }

    /* Generate functions */
    for (usize i = 0; i < program->program.decls->len; i++) {
        AstNode *d = vec_at(program->program.decls, i);
        if (d->kind == AST_FN_DECL) gen_fn(ctx, d);
    }

    return b->mod;
}
