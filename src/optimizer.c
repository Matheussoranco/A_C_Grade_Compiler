/*
 * optimizer.c — IR optimization passes.
 *
 * Each pass iterates over instructions within a function and returns the
 * number of transformations made. The pipeline runs until fixed-point.
 */
#include "../include/optimizer.h"

/* =========================================================================
 * Constant folding
 * Folds binary/unary operations on integer immediate operands.
 * ========================================================================= */
int opt_const_fold(IrFunc *f) {
    int changes = 0;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            IOp *l = &inst->src[0];
            IOp *r = &inst->src[1];
            if (l->kind != IOP_IMM_INT) continue;
            if (inst->dst.id < 0)      continue;
            i64 lv = l->imm_int;
            /* Unary */
            if (inst->op == IR_NEG && r->kind == IOP_UNDEF) {
                inst->op = IR_MOV;
                inst->src[0] = iop_imm_int(-lv, inst->dst.type);
                changes++;
                continue;
            }
            if (inst->op == IR_NOT && r->kind == IOP_UNDEF) {
                inst->op = IR_MOV;
                inst->src[0] = iop_imm_int(~lv, inst->dst.type);
                changes++;
                continue;
            }
            if (r->kind != IOP_IMM_INT) continue;
            i64 rv = r->imm_int;
            i64 result = 0;
            bool fold = true;
            switch (inst->op) {
                case IR_ADD:  result = lv + rv; break;
                case IR_SUB:  result = lv - rv; break;
                case IR_MUL:  result = lv * rv; break;
                case IR_SDIV: if (rv == 0) { fold=false; break; } result = lv / rv; break;
                case IR_UDIV: if (rv == 0) { fold=false; break; }
                              result = (i64)((u64)lv / (u64)rv); break;
                case IR_SREM: if (rv == 0) { fold=false; break; } result = lv % rv; break;
                case IR_AND:  result = lv & rv; break;
                case IR_OR:   result = lv | rv; break;
                case IR_XOR:  result = lv ^ rv; break;
                case IR_SHL:  result = (rv >= 0 && rv < 64) ? (lv << rv) : 0; break;
                case IR_ASHR: result = (rv >= 0 && rv < 64) ? (lv >> rv) : 0; break;
                case IR_LSHR: result = (rv >= 0 && rv < 64) ? (i64)((u64)lv >> rv) : 0; break;
                case IR_EQ:   result = lv == rv; break;
                case IR_NE:   result = lv != rv; break;
                case IR_SLT:  result = lv < rv;  break;
                case IR_SLE:  result = lv <= rv; break;
                case IR_SGT:  result = lv > rv;  break;
                case IR_SGE:  result = lv >= rv; break;
                case IR_ULT:  result = (u64)lv < (u64)rv;  break;
                case IR_ULE:  result = (u64)lv <= (u64)rv; break;
                case IR_UGT:  result = (u64)lv > (u64)rv;  break;
                case IR_UGE:  result = (u64)lv >= (u64)rv; break;
                default: fold = false; break;
            }
            if (fold) {
                inst->op = IR_MOV;
                inst->src[0] = iop_imm_int(result, inst->dst.type);
                inst->src[1] = iop_undef();
                changes++;
            }
        }
    }
    return changes;
}

/* =========================================================================
 * Copy propagation
 * Replaces uses of t1 where t1 = t2 (MOV from vreg to vreg) with t2.
 * ========================================================================= */
int opt_copy_prop(IrFunc *f) {
    int changes = 0;
    /* Build copy map: vreg_id → source IOp for MOV instructions */
    int n = f->next_vreg;
    if (n <= 0) return 0;
    IOp *copies = calloc((usize)n, sizeof(IOp));
    for (int i = 0; i < n; i++) copies[i].kind = IOP_UNDEF;

    /* First pass: collect copies */
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            if (inst->op == IR_MOV && inst->dst.id >= 0 &&
                inst->src[0].kind == IOP_VREG) {
                copies[inst->dst.id] = inst->src[0];
            }
        }
    }

    /* Second pass: replace uses */
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            /* Don't touch the copy instruction's src itself to avoid cycles */
            if (inst->op == IR_MOV) continue;
            for (int si = 0; si < 3; si++) {
                IOp *s = &inst->src[si];
                if (s->kind == IOP_VREG && s->vreg.id >= 0 &&
                    s->vreg.id < n &&
                    copies[s->vreg.id].kind == IOP_VREG) {
                    *s = copies[s->vreg.id];
                    changes++;
                }
            }
            if (inst->call_args) {
                for (usize ai = 0; ai < inst->call_args->len; ai++) {
                    IOp *a = vec_at(inst->call_args, ai);
                    if (a->kind == IOP_VREG && a->vreg.id >= 0 &&
                        a->vreg.id < n &&
                        copies[a->vreg.id].kind == IOP_VREG) {
                        *a = copies[a->vreg.id];
                        changes++;
                    }
                }
            }
        }
    }
    free(copies);
    return changes;
}

/* =========================================================================
 * Dead code elimination
 * Removes instructions whose result is never used and have no side effects.
 * ========================================================================= */
static bool has_side_effects(IrOp op) {
    switch (op) {
        case IR_STORE: case IR_GSTORE: case IR_CALL:
        case IR_RET:   case IR_JMP:    case IR_JMPIF:
        case IR_JMPIFNOT: case IR_LABEL_DEF:
            return true;
        default:
            return false;
    }
}

int opt_dce(IrFunc *f) {
    int changes = 0;
    int n = f->next_vreg;
    if (n <= 0) return 0;

    /* Count uses of each vreg */
    int *use_count = calloc((usize)n, sizeof(int));

    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            for (int si = 0; si < 3; si++) {
                IOp *s = &inst->src[si];
                if (s->kind == IOP_VREG && s->vreg.id >= 0 && s->vreg.id < n)
                    use_count[s->vreg.id]++;
            }
            if (inst->call_args) {
                for (usize ai = 0; ai < inst->call_args->len; ai++) {
                    IOp *a = vec_at(inst->call_args, ai);
                    if (a->kind == IOP_VREG && a->vreg.id >= 0 && a->vreg.id < n)
                        use_count[a->vreg.id]++;
                }
            }
        }
    }

    /* Remove instructions with unused results and no side effects */
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        IrInst *inst = blk->head;
        while (inst) {
            IrInst *next = inst->next;
            if (inst->dst.id >= 0 && inst->dst.id < n &&
                use_count[inst->dst.id] == 0 &&
                !has_side_effects(inst->op)) {
                /* Remove from doubly-linked list */
                if (inst->prev) inst->prev->next = inst->next;
                else            blk->head        = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else            blk->tail        = inst->prev;
                blk->n_insts--;
                free(inst);
                changes++;
            }
            inst = next;
        }
    }
    free(use_count);
    return changes;
}

/* =========================================================================
 * Strength reduction
 * x * 2^n → x << n,  x / 2^n → x >> n  (for unsigned/positive)
 * x * 0   → 0,       x + 0   → x,  etc.
 * ========================================================================= */
static int log2_exact(u64 v) {
    if (v == 0 || (v & (v - 1)) != 0) return -1;
    int n = 0; while (v > 1) { v >>= 1; n++; } return n;
}

int opt_strength_reduce(IrFunc *f) {
    int changes = 0;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            if (inst->dst.id < 0) continue;
            IOp *l = &inst->src[0], *r = &inst->src[1];

            /* x * 1 → x */
            if (inst->op == IR_MUL && r->kind == IOP_IMM_INT && r->imm_int == 1) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* x * 0 → 0 */
            else if (inst->op == IR_MUL && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[0] = *r; inst->src[1] = iop_undef(); changes++;
            }
            /* x * 2^n → x << n */
            else if (inst->op == IR_MUL && r->kind == IOP_IMM_INT) {
                int s = log2_exact((u64)r->imm_int);
                if (s > 0) {
                    inst->op = IR_SHL;
                    inst->src[1] = iop_imm_int(s, r->type);
                    changes++;
                }
            }
            /* x + 0 → x */
            else if (inst->op == IR_ADD && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* x - 0 → x */
            else if (inst->op == IR_SUB && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* x >> 0 or x << 0 → x */
            else if ((inst->op == IR_SHL || inst->op == IR_ASHR || inst->op == IR_LSHR)
                     && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* x & 0 → 0 */
            else if (inst->op == IR_AND && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV;
                inst->src[0] = iop_imm_int(0, inst->dst.type);
                inst->src[1] = iop_undef(); changes++;
            }
            /* x | 0 → x */
            else if (inst->op == IR_OR && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* x ^ 0 → x */
            else if (inst->op == IR_XOR && r->kind == IOP_IMM_INT && r->imm_int == 0) {
                inst->op = IR_MOV; inst->src[1] = iop_undef(); changes++;
            }
            /* unsigned division by power-of-2 → logical right shift */
            else if (inst->op == IR_UDIV && r->kind == IOP_IMM_INT) {
                int s = log2_exact((u64)r->imm_int);
                if (s > 0) {
                    inst->op = IR_LSHR;
                    inst->src[1] = iop_imm_int(s, r->type);
                    changes++;
                }
            }
            (void)l;
        }
    }
    return changes;
}

/* =========================================================================
 * Peephole: local pattern matching
 * - Remove consecutive MOV t1, t2; MOV t2, t1 pairs (only if t1 unused after)
 * ========================================================================= */
int opt_peephole(IrFunc *f) {
    int changes = 0;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst && inst->next; ) {
            IrInst *next = inst->next;
            /* NOP elimination */
            if (inst->op == IR_NOP) {
                if (inst->prev) inst->prev->next = inst->next;
                else blk->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else blk->tail = inst->prev;
                blk->n_insts--;
                free(inst);
                inst = next;
                changes++;
                continue;
            }
            /* MOV t, t  → remove */
            if (inst->op == IR_MOV && inst->dst.id >= 0 &&
                inst->src[0].kind == IOP_VREG &&
                inst->src[0].vreg.id == inst->dst.id) {
                if (inst->prev) inst->prev->next = inst->next;
                else blk->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else blk->tail = inst->prev;
                blk->n_insts--;
                free(inst);
                inst = next;
                changes++;
                continue;
            }
            inst = inst->next;
        }
    }
    return changes;
}

/* =========================================================================
 * Pipeline driver
 * ========================================================================= */
int opt_run_all(IrModule *m) {
    int total = 0;
    for (usize i = 0; i < m->funcs->len; i++) {
        IrFunc *f = vec_at(m->funcs, i);
        int changed;
        do {
            changed  = opt_const_fold(f);
            changed += opt_copy_prop(f);
            changed += opt_strength_reduce(f);
            changed += opt_dce(f);
            changed += opt_peephole(f);
            total   += changed;
        } while (changed > 0);
    }
    return total;
}
