/*
 * ir.c — IR module, builder, and dump.
 */
#include "../include/ir.h"

/* =========================================================================
 * Module / function / block allocation
 * ========================================================================= */
IrModule *ir_module_new(Arena *arena, TypeCtx *types) {
    IrModule *m = arena_calloc(arena, sizeof(IrModule));
    m->funcs    = vec_new();
    m->globals  = vec_new();
    m->str_lits = vec_new();
    m->arena    = arena;
    (void)types;
    return m;
}

IrFunc *ir_func_new(IrModule *m, const char *name, Type *fn_type, Symbol *sym) {
    IrFunc *f    = arena_calloc(m->arena, sizeof(IrFunc));
    f->name      = name;
    f->type      = fn_type;
    f->blocks    = vec_new();
    f->vregs     = vec_new();
    f->locals    = vec_new();
    f->next_vreg = 0;
    f->next_label = 0;
    f->fn_sym    = sym;
    vec_push(m->funcs, f);
    return f;
}

IrBlock *ir_block_new(IrFunc *fn, const char *name) {
    IrBlock *b = calloc(1, sizeof(IrBlock));
    b->id      = fn->next_label++;
    b->name    = name;
    b->preds   = vec_new();
    vec_push(fn->blocks, b);
    return b;
}

void ir_set_block(IrBuilder *bldr, IrBlock *blk) {
    bldr->cur = blk;
}

IrBuilder *ir_builder_new(IrModule *m, Arena *arena, TypeCtx *types) {
    IrBuilder *b = arena_calloc(arena, sizeof(IrBuilder));
    b->mod   = m;
    b->arena = arena;
    b->types = types;
    return b;
}

/* =========================================================================
 * Virtual register allocation
 * ========================================================================= */
VReg ir_alloc_vreg(IrBuilder *b, Type *t, const char *name) {
    VReg *r = arena_calloc(b->arena, sizeof(VReg));
    r->id   = b->fn ? b->fn->next_vreg++ : 0;
    r->type = t;
    r->name = name;
    if (b->fn) vec_push(b->fn->vregs, r);
    return *r;
}

/* =========================================================================
 * Operand constructors
 * ========================================================================= */
IOp iop_vreg(VReg r)             { IOp o={0}; o.kind=IOP_VREG;    o.vreg=r;     o.type=r.type;          return o; }
IOp iop_imm_int(i64 v, Type *t)  { IOp o={0}; o.kind=IOP_IMM_INT; o.imm_int=v;  o.type=t;               return o; }
IOp iop_imm_flt(f64 v, Type *t)  { IOp o={0}; o.kind=IOP_IMM_FLT; o.imm_flt=v;  o.type=t;               return o; }
IOp iop_imm_str(const char *s, Type *t) { IOp o={0}; o.kind=IOP_IMM_STR; o.imm_str=s; o.type=t;         return o; }
IOp iop_label(i32 id)            { IOp o={0}; o.kind=IOP_LABEL;   o.label_id=id;                         return o; }
IOp iop_global(Symbol *s)        { IOp o={0}; o.kind=IOP_GLOBAL;  o.global=s;   o.type=s?s->type:NULL;   return o; }
IOp iop_undef(void)              { IOp o={0}; o.kind=IOP_UNDEF;                                           return o; }

/* =========================================================================
 * Instruction emission
 * ========================================================================= */
static VReg VREG_NONE = {-1, NULL, NULL};

IrInst *ir_emit(IrBuilder *b, IrOp op, VReg dst,
                IOp s0, IOp s1, IOp s2, SrcLoc loc) {
    assert(b->cur && "no current block");
    IrInst *inst = calloc(1, sizeof(IrInst));
    inst->op     = op;
    inst->loc    = loc;
    inst->dst    = dst;
    inst->src[0] = s0;
    inst->src[1] = s1;
    inst->src[2] = s2;
    inst->prev   = b->cur->tail;
    inst->next   = NULL;
    if (b->cur->tail) b->cur->tail->next = inst;
    else              b->cur->head = inst;
    b->cur->tail = inst;
    b->cur->n_insts++;
    return inst;
}

VReg ir_mov(IrBuilder *b, IOp src, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, src.type, NULL);
    ir_emit(b, IR_MOV, dst, src, iop_undef(), iop_undef(), loc);
    return dst;
}

VReg ir_binop(IrBuilder *b, IrOp op, IOp l, IOp r, SrcLoc loc) {
    Type *t = l.type;
    VReg dst = ir_alloc_vreg(b, t, NULL);
    ir_emit(b, op, dst, l, r, iop_undef(), loc);
    return dst;
}

VReg ir_unop(IrBuilder *b, IrOp op, IOp src, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, src.type, NULL);
    ir_emit(b, op, dst, src, iop_undef(), iop_undef(), loc);
    return dst;
}

VReg ir_call(IrBuilder *b, IOp callee, Vec *args, Type *ret_type, SrcLoc loc) {
    VReg dst = ret_type && ret_type->kind != TY_VOID
               ? ir_alloc_vreg(b, ret_type, NULL)
               : VREG_NONE;
    IrInst *inst = ir_emit(b, IR_CALL, dst, callee, iop_undef(), iop_undef(), loc);
    inst->call_args = args;
    return dst;
}

void ir_ret(IrBuilder *b, IOp val, SrcLoc loc) {
    ir_emit(b, IR_RET, VREG_NONE, val, iop_undef(), iop_undef(), loc);
}

void ir_jmp(IrBuilder *b, i32 label, SrcLoc loc) {
    IrInst *inst = ir_emit(b, IR_JMP, VREG_NONE, iop_undef(), iop_undef(), iop_undef(), loc);
    inst->label_id = label;
}

void ir_jmpif(IrBuilder *b, IOp cond, i32 then_lbl, i32 else_lbl, SrcLoc loc) {
    IrInst *inst = ir_emit(b, IR_JMPIF, VREG_NONE, cond, iop_undef(), iop_undef(), loc);
    inst->label_id  = then_lbl;
    inst->label2_id = else_lbl;
}

VReg ir_load(IrBuilder *b, IOp ptr, Type *t, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, t, NULL);
    ir_emit(b, IR_LOAD, dst, ptr, iop_undef(), iop_undef(), loc);
    return dst;
}

void ir_store(IrBuilder *b, IOp ptr, IOp val, SrcLoc loc) {
    ir_emit(b, IR_STORE, VREG_NONE, ptr, val, iop_undef(), loc);
}

VReg ir_lea(IrBuilder *b, Symbol *sym, SrcLoc loc) {
    Type *t = ty_ptr(b->types, sym->type);
    VReg dst = ir_alloc_vreg(b, t, NULL);
    ir_emit(b, IR_LEA, dst, iop_global(sym), iop_undef(), iop_undef(), loc);
    return dst;
}

VReg ir_alloca(IrBuilder *b, Type *t, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, ty_ptr(b->types, t), NULL);
    ir_emit(b, IR_ALLOCA, dst, iop_undef(), iop_undef(), iop_undef(), loc);
    return dst;
}

VReg ir_getidx(IrBuilder *b, IOp base, IOp idx, Type *elem_t, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, ty_ptr(b->types, elem_t), NULL);
    ir_emit(b, IR_GETIDX, dst, base, idx, iop_undef(), loc);
    return dst;
}

VReg ir_getfld(IrBuilder *b, IOp base, usize byte_off, Type *fld_t, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, ty_ptr(b->types, fld_t), NULL);
    IOp off  = iop_imm_int((i64)byte_off, ty_u64(b->types));
    ir_emit(b, IR_GETFLD, dst, base, off, iop_undef(), loc);
    return dst;
}

VReg ir_cast(IrBuilder *b, IrOp op, IOp src, Type *to, SrcLoc loc) {
    VReg dst = ir_alloc_vreg(b, to, NULL);
    ir_emit(b, op, dst, src, iop_undef(), iop_undef(), loc);
    return dst;
}

i32 ir_label_new(IrBuilder *b, const char *name) {
    IrBlock *blk = ir_block_new(b->fn, name);
    return blk->id;
}

void ir_label_def(IrBuilder *b, i32 lbl, SrcLoc loc) {
    IrInst *inst = ir_emit(b, IR_LABEL_DEF, VREG_NONE,
                           iop_undef(), iop_undef(), iop_undef(), loc);
    inst->label_id = lbl;
    /* Also switch current block if the label corresponds to a real block */
    if (b->fn) {
        for (usize i = 0; i < b->fn->blocks->len; i++) {
            IrBlock *blk = vec_at(b->fn->blocks, i);
            if (blk->id == lbl) { b->cur = blk; return; }
        }
    }
}

/* =========================================================================
 * String literal interning
 * ========================================================================= */
const char *ir_str_label(IrModule *m, const char *val, Arena *arena) {
    /* Use index in str_lits as suffix */
    for (usize i = 0; i < m->str_lits->len; i++)
        if (strcmp(vec_at(m->str_lits, i), val) == 0)
            return arena_strdup(arena, val); /* key not label; caller uses idx */
    vec_push(m->str_lits, (void*)val);
    return val;
}

/* =========================================================================
 * IR dump
 * ========================================================================= */
static void dump_iop(const IOp *o, FILE *out) {
    switch (o->kind) {
        case IOP_VREG:    fprintf(out, "%%t%d",  o->vreg.id);  break;
        case IOP_IMM_INT: fprintf(out, "%lld",   (long long)o->imm_int); break;
        case IOP_IMM_FLT: fprintf(out, "%g",     o->imm_flt);  break;
        case IOP_IMM_STR: fprintf(out, "\"%s\"", o->imm_str ? o->imm_str : ""); break;
        case IOP_LABEL:   fprintf(out, "L%d",    o->label_id); break;
        case IOP_GLOBAL:  fprintf(out, "@%s",    o->global ? o->global->name : "?"); break;
        case IOP_UNDEF:   fputs("undef", out);   break;
    }
}

static const char *irop_name(IrOp op) {
    switch (op) {
        case IR_NOP:    return "nop";
        case IR_MOV:    return "mov";
        case IR_LOAD:   return "load";
        case IR_STORE:  return "store";
        case IR_LEA:    return "lea";
        case IR_GLOAD:  return "gload";
        case IR_GSTORE: return "gstore";
        case IR_ALLOCA: return "alloca";
        case IR_ADD:    return "add";  case IR_SUB:  return "sub";
        case IR_MUL:    return "mul";  case IR_SDIV: return "sdiv";
        case IR_UDIV:   return "udiv"; case IR_SREM: return "srem";
        case IR_UREM:   return "urem"; case IR_NEG:  return "neg";
        case IR_AND:    return "and";  case IR_OR:   return "or";
        case IR_XOR:    return "xor";  case IR_NOT:  return "not";
        case IR_SHL:    return "shl";  case IR_LSHR: return "lshr";
        case IR_ASHR:   return "ashr";
        case IR_EQ:     return "eq";   case IR_NE:   return "ne";
        case IR_SLT:    return "slt";  case IR_SLE:  return "sle";
        case IR_SGT:    return "sgt";  case IR_SGE:  return "sge";
        case IR_ULT:    return "ult";  case IR_ULE:  return "ule";
        case IR_UGT:    return "ugt";  case IR_UGE:  return "uge";
        case IR_FADD:   return "fadd"; case IR_FSUB: return "fsub";
        case IR_FMUL:   return "fmul"; case IR_FDIV: return "fdiv";
        case IR_FNEG:   return "fneg";
        case IR_SEXT:   return "sext"; case IR_ZEXT: return "zext";
        case IR_TRUNC:  return "trunc";
        case IR_ITOF:   return "itof"; case IR_UITOF:return "uitof";
        case IR_FTOI:   return "ftoi"; case IR_BITCAST:return "bitcast";
        case IR_PTRTOINT:return "ptrtoint"; case IR_INTTOPTR:return "inttoptr";
        case IR_FPEXT:  return "fpext"; case IR_FPTRUNC:return "fptrunc";
        case IR_GETIDX: return "getidx";case IR_GETFLD:return "getfld";
        case IR_JMP:    return "jmp";  case IR_JMPIF:return "jmpif";
        case IR_JMPIFNOT:return "jmpifnot";
        case IR_CALL:   return "call"; case IR_RET:  return "ret";
        case IR_PHI:    return "phi";  case IR_LABEL_DEF:return "label";
        default:        return "?";
    }
}

void ir_dump_func(const IrFunc *f, FILE *out) {
    fprintf(out, "fn %s:\n", f->name);
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        fprintf(out, "  .block%d:\n", blk->id);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            fputs("    ", out);
            if (inst->dst.id >= 0)
                fprintf(out, "%%t%d = ", inst->dst.id);
            fputs(irop_name(inst->op), out);
            fputc(' ', out);
            if (inst->op == IR_CALL && inst->call_args) {
                dump_iop(&inst->src[0], out);
                fputs("(", out);
                for (usize i = 0; i < inst->call_args->len; i++) {
                    if (i) fputs(", ", out);
                    IOp *a = vec_at(inst->call_args, i);
                    dump_iop(a, out);
                }
                fputs(")", out);
            } else {
                for (int i = 0; i < 3; i++) {
                    if (inst->src[i].kind == IOP_UNDEF) break;
                    if (i) fputs(", ", out);
                    dump_iop(&inst->src[i], out);
                }
                if (inst->op == IR_JMP || inst->op == IR_LABEL_DEF)
                    fprintf(out, " L%d", inst->label_id);
                if (inst->op == IR_JMPIF)
                    fprintf(out, " L%d else L%d", inst->label_id, inst->label2_id);
            }
            fputc('\n', out);
        }
    }
}

void ir_dump_module(const IrModule *m, FILE *out) {
    fprintf(out, "; IR module — %zu functions\n\n", m->funcs->len);
    for (usize i = 0; i < m->funcs->len; i++)
        ir_dump_func(vec_at(m->funcs, i), out);
}
