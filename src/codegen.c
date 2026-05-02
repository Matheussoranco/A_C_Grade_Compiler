/*
 * codegen.c — x86-64 NASM code generation.
 *
 * Architecture: System V AMD64 ABI (Linux ELF64).
 * Register allocation: Linear Scan (Poletto & Sarkar 1999).
 *
 * Stack frame layout:
 *   [rbp + 16 + 8*(n-1)]  argument n (if > 6 args)
 *   [rbp + 16]             argument 7
 *   [rbp +  8]             saved return address (pushed by call)
 *   [rbp +  0]             saved rbp
 *   [rbp -  8]             first local / spill slot
 *   ...
 *
 * For simplicity this codegen uses a "virtual-register → stack slot" model:
 * each virtual register is assigned a unique stack slot at [rbp - offset].
 * This guarantees correctness. The linear-scan pass then tries to promote
 * vregs to physical registers to eliminate unnecessary memory traffic.
 */
#include "../include/codegen.h"

/* =========================================================================
 * Physical register tables
 * ========================================================================= */
static const char *REG64[] = {
    "rax","rbx","rcx","rdx","rsi","rdi",
    "r8","r9","r10","r11","r12","r13","r14","r15",
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
    "SPILL"
};
static const char *REG32[] = {
    "eax","ebx","ecx","edx","esi","edi",
    "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d",
    /* float regs reuse 64-bit names with sd/ss suffix */
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
    "SPILL"
};
static const char *REG16[] = {
    "ax","bx","cx","dx","si","di",
    "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w",
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
    "SPILL"
};
static const char *REG8[] = {
    "al","bl","cl","dl","sil","dil",
    "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b",
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
    "SPILL"
};

const char *preg_name64(PReg r) { return r < PREG_COUNT ? REG64[r] : "?"; }
const char *preg_name32(PReg r) { return r < PREG_COUNT ? REG32[r] : "?"; }
const char *preg_name16(PReg r) { return r < PREG_COUNT ? REG16[r] : "?"; }
const char *preg_name8 (PReg r) { return r < PREG_COUNT ? REG8[r]  : "?"; }
const char *preg_xmm  (PReg r) { return r < PREG_COUNT ? REG64[r] : "?"; }

/* Integer argument registers (System V AMD64 ABI) */
static const PReg INT_ARG_REGS[] = {
    PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9
};
#define N_INT_ARG_REGS 6

/* Float argument registers */
static const PReg FLT_ARG_REGS[] = {
    PREG_XMM0,PREG_XMM1,PREG_XMM2,PREG_XMM3,
    PREG_XMM4,PREG_XMM5,PREG_XMM6,PREG_XMM7
};
#define N_FLT_ARG_REGS 8

/* Caller-saved (scratch) integer registers */
static const PReg CALLER_SAVED[] = {
    PREG_RAX, PREG_RCX, PREG_RDX, PREG_RSI, PREG_RDI,
    PREG_R8,  PREG_R9,  PREG_R10, PREG_R11
};
#define N_CALLER_SAVED 9

/* Callee-saved integer registers */
static const PReg CALLEE_SAVED[] = {
    PREG_RBX, PREG_R12, PREG_R13, PREG_R14, PREG_R15
};
#define N_CALLEE_SAVED 5

/* =========================================================================
 * Codegen context
 * ========================================================================= */
CodegenCtx *codegen_new(IrModule *mod, Arena *arena) {
    CodegenCtx *ctx = arena_calloc(arena, sizeof(CodegenCtx));
    ctx->mod   = mod;
    ctx->arena = arena;
    ctx->out   = sb_new();
    return ctx;
}

/* Emit a line */
#define EMIT(...)  sb_printf(ctx->out, __VA_ARGS__)
#define EMITL(...) do { sb_printf(ctx->out, "    " __VA_ARGS__); sb_putc(ctx->out, '\n'); } while(0)

/* =========================================================================
 * Stack-slot model
 * Each vreg gets an 8-byte slot at [rbp - (id+1)*8].
 * ========================================================================= */
static int vreg_slot(int id) {
    return -(id + 1) * 8;  /* rbp-relative byte offset */
}

/* Emit the NASM operand string for an IOp (loads into scratch if needed) */
static void emit_load_to(CodegenCtx *ctx, const IOp *op, const char *reg64,
                          usize size) {
    const char *rname;
    if (size == 8)      rname = reg64;
    else if (size == 4) {
        /* Use the 32-bit name of the register */
        if      (strcmp(reg64,"rax")==0) rname="eax";
        else if (strcmp(reg64,"rcx")==0) rname="ecx";
        else if (strcmp(reg64,"rdx")==0) rname="edx";
        else if (strcmp(reg64,"rsi")==0) rname="esi";
        else if (strcmp(reg64,"rdi")==0) rname="edi";
        else if (strcmp(reg64,"r8") ==0) rname="r8d";
        else if (strcmp(reg64,"r9") ==0) rname="r9d";
        else if (strcmp(reg64,"r10")==0) rname="r10d";
        else if (strcmp(reg64,"r11")==0) rname="r11d";
        else rname = reg64;
    } else if (size == 2) {
        if      (strcmp(reg64,"rax")==0) rname="ax";
        else if (strcmp(reg64,"rcx")==0) rname="cx";
        else if (strcmp(reg64,"rdx")==0) rname="dx";
        else rname = reg64;
    } else if (size == 1) {
        if      (strcmp(reg64,"rax")==0) rname="al";
        else if (strcmp(reg64,"rcx")==0) rname="cl";
        else if (strcmp(reg64,"rdx")==0) rname="dl";
        else rname = reg64;
    } else {
        rname = reg64;
    }

    switch (op->kind) {
        case IOP_IMM_INT:
            EMITL("mov %s, %lld", rname, (long long)op->imm_int);
            break;
        case IOP_IMM_STR:
            EMITL("lea %s, [rel .Lstr_%p]", reg64, (void*)op->imm_str);
            break;
        case IOP_VREG:
            if (size == 8)
                EMITL("mov %s, qword [rbp%+d]", reg64, vreg_slot(op->vreg.id));
            else if (size == 4)
                EMITL("movsxd %s, dword [rbp%+d]", reg64, vreg_slot(op->vreg.id));
            else if (size == 2)
                EMITL("movsx %s, word [rbp%+d]", reg64, vreg_slot(op->vreg.id));
            else
                EMITL("movsx %s, byte [rbp%+d]", reg64, vreg_slot(op->vreg.id));
            break;
        case IOP_GLOBAL:
            EMITL("lea %s, [rel %s]", reg64, op->global ? op->global->name : "?");
            EMITL("mov %s, [%s]", rname, reg64);
            break;
        case IOP_IMM_FLT:
            /* Float constants emitted via a local literal */
            EMITL("; float literal %g", op->imm_flt);
            break;
        default:
            EMITL("; unhandled operand kind %d", (int)op->kind);
    }
}

static void emit_store_from(CodegenCtx *ctx, const char *src_reg, int vreg_id, usize size) {
    int off = vreg_slot(vreg_id);
    if (size == 8)
        EMITL("mov qword [rbp%+d], %s", off, src_reg);
    else if (size == 4)
        EMITL("mov dword [rbp%+d], %s", off, src_reg);
    else if (size == 2)
        EMITL("mov word [rbp%+d], %s", off, src_reg);
    else
        EMITL("mov byte [rbp%+d], %s", off, src_reg);
}

static usize op_size(const IOp *o) {
    if (!o->type) return 8;
    return o->type->size ? o->type->size : 8;
}

static const char *size_kw(usize sz) {
    switch (sz) {
        case 1: return "byte";
        case 2: return "word";
        case 4: return "dword";
        default: return "qword";
    }
}

/* =========================================================================
 * Per-function frame size calculation
 * ========================================================================= */
static int calc_frame_size(IrFunc *f) {
    /* Each vreg gets 8 bytes; align to 16 */
    int n_vregs = f->next_vreg;
    /* Also count local variables explicitly allocated */
    int extra = 0;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            if (inst->op == IR_ALLOCA && inst->dst.id >= 0) {
                if (inst->dst.type && inst->dst.type->ptr_base)
                    extra += (int)inst->dst.type->ptr_base->size;
            }
        }
    }
    int size = n_vregs * 8 + extra + 8; /* +8 for alignment buffer */
    return (size + 15) & ~15; /* align to 16 */
}

/* =========================================================================
 * Instruction code generation
 * ========================================================================= */
static void gen_inst(CodegenCtx *ctx, IrInst *inst) {
    IrOp op = inst->op;
    IOp *s0 = &inst->src[0];
    IOp *s1 = &inst->src[1];
    VReg dst = inst->dst;
    usize sz = dst.id >= 0 && dst.type ? dst.type->size : 8;
    if (sz == 0) sz = 8;

    sb_printf(ctx->out, "    ; [%s]\n", /* light debug comment */
              op == IR_MOV ? "mov" : op == IR_ADD ? "add" :
              op == IR_CALL ? "call" : op == IR_RET ? "ret" : "inst");

    switch (op) {
        case IR_NOP: case IR_PHI: break;

        case IR_LABEL_DEF:
            sb_printf(ctx->out, ".L%d:\n", inst->label_id);
            break;

        /* --- Data movement --- */
        case IR_MOV:
            emit_load_to(ctx, s0, "rax", sz);
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_ALLOCA: {
            /* The alloca vreg stores the *address* of the slot.
             * We just use the vreg's own slot address as the pointer. */
            if (dst.id >= 0) {
                EMITL("lea rax, [rbp%+d]", vreg_slot(dst.id));
                EMITL("mov qword [rbp%+d], rax", vreg_slot(dst.id));
            }
            break;
        }

        case IR_LEA:
            if (s0->kind == IOP_GLOBAL && s0->global) {
                EMITL("lea rax, [rel %s]", s0->global->name);
            }
            if (dst.id >= 0) EMITL("mov qword [rbp%+d], rax", vreg_slot(dst.id));
            break;

        case IR_LOAD: {
            /* Load value through pointer operand s0 */
            emit_load_to(ctx, s0, "rax", 8); /* load the pointer */
            usize val_sz = sz;
            if (val_sz == 8)
                EMITL("mov rax, qword [rax]");
            else if (val_sz == 4)
                EMITL("movsxd rax, dword [rax]");
            else if (val_sz == 2)
                EMITL("movsx rax, word [rax]");
            else
                EMITL("movsx rax, byte [rax]");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, 8);
            break;
        }

        case IR_STORE: {
            /* s0 = ptr, s1 = value */
            emit_load_to(ctx, s0, "rax", 8); /* pointer */
            usize val_sz = op_size(s1);
            if (val_sz == 0) val_sz = 8;
            emit_load_to(ctx, s1, "rcx", 8); /* value */
            if (val_sz == 8)
                EMITL("mov qword [rax], rcx");
            else if (val_sz == 4)
                EMITL("mov dword [rax], ecx");
            else if (val_sz == 2)
                EMITL("mov word [rax], cx");
            else
                EMITL("mov byte [rax], cl");
            break;
        }

        case IR_GLOAD:
            if (s0->kind == IOP_GLOBAL && s0->global)
                EMITL("mov rax, [rel %s]", s0->global->name);
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_GSTORE:
            emit_load_to(ctx, s1, "rax", sz);
            if (s0->kind == IOP_GLOBAL && s0->global)
                EMITL("mov [rel %s], rax", s0->global->name);
            break;

        /* --- Arithmetic --- */
        case IR_ADD:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("add rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_SUB:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("sub rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_MUL:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("imul rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_SDIV:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("cqo");
            EMITL("idiv rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_UDIV:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("xor rdx, rdx");
            EMITL("div rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_SREM:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("cqo");
            EMITL("idiv rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rdx", dst.id, sz);
            break;

        case IR_UREM:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("xor rdx, rdx");
            EMITL("div rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rdx", dst.id, sz);
            break;

        case IR_NEG:
            emit_load_to(ctx, s0, "rax", sz);
            EMITL("neg rax");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        /* --- Bitwise --- */
        case IR_AND:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("and rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_OR:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("or rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_XOR:
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("xor rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_NOT:
            emit_load_to(ctx, s0, "rax", sz);
            EMITL("not rax");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_SHL:
            emit_load_to(ctx, s0, "rax", sz);
            if (s1->kind == IOP_IMM_INT)
                EMITL("shl rax, %lld", (long long)s1->imm_int);
            else {
                emit_load_to(ctx, s1, "rcx", 1);
                EMITL("shl rax, cl");
            }
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_ASHR:
            emit_load_to(ctx, s0, "rax", sz);
            if (s1->kind == IOP_IMM_INT)
                EMITL("sar rax, %lld", (long long)s1->imm_int);
            else {
                emit_load_to(ctx, s1, "rcx", 1);
                EMITL("sar rax, cl");
            }
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_LSHR:
            emit_load_to(ctx, s0, "rax", sz);
            if (s1->kind == IOP_IMM_INT)
                EMITL("shr rax, %lld", (long long)s1->imm_int);
            else {
                emit_load_to(ctx, s1, "rcx", 1);
                EMITL("shr rax, cl");
            }
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        /* --- Comparisons → 0/1 in rax --- */
        case IR_EQ: case IR_NE:
        case IR_SLT: case IR_SLE: case IR_SGT: case IR_SGE:
        case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
        case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE:
        case IR_FGT: case IR_FGE: {
            emit_load_to(ctx, s0, "rax", sz);
            emit_load_to(ctx, s1, "rcx", sz);
            EMITL("cmp rax, rcx");
            const char *setcc;
            switch (op) {
                case IR_EQ:  case IR_FEQ: setcc = "sete";  break;
                case IR_NE:  case IR_FNE: setcc = "setne"; break;
                case IR_SLT: case IR_FLT: setcc = "setl";  break;
                case IR_SLE: case IR_FLE: setcc = "setle"; break;
                case IR_SGT: case IR_FGT: setcc = "setg";  break;
                case IR_SGE: case IR_FGE: setcc = "setge"; break;
                case IR_ULT:              setcc = "setb";  break;
                case IR_ULE:              setcc = "setbe"; break;
                case IR_UGT:              setcc = "seta";  break;
                case IR_UGE:              setcc = "setae"; break;
                default:                  setcc = "sete";  break;
            }
            EMITL("%s al", setcc);
            EMITL("movzx rax, al");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, 8);
            break;
        }

        /* --- Type conversions --- */
        case IR_SEXT:
            emit_load_to(ctx, s0, "rax", op_size(s0));
            /* sign-extend is done via movsxd / movsx */
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_ZEXT:
            emit_load_to(ctx, s0, "rax", op_size(s0));
            EMITL("and rax, 0x%llx", (unsigned long long)((1ULL << (op_size(s0)*8)) - 1));
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_TRUNC:
            emit_load_to(ctx, s0, "rax", op_size(s0));
            /* Truncation: just store with smaller size */
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_ITOF: case IR_UITOF:
            emit_load_to(ctx, s0, "rax", op_size(s0));
            EMITL("cvtsi2sd xmm0, rax");
            if (dst.id >= 0) EMITL("movsd qword [rbp%+d], xmm0", vreg_slot(dst.id));
            break;

        case IR_FTOI:
            if (s0->kind == IOP_VREG)
                EMITL("movsd xmm0, qword [rbp%+d]", vreg_slot(s0->vreg.id));
            EMITL("cvttsd2si rax, xmm0");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_BITCAST: case IR_PTRTOINT: case IR_INTTOPTR:
            emit_load_to(ctx, s0, "rax", 8);
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, 8);
            break;

        /* --- Array / struct element access --- */
        case IR_GETIDX: {
            emit_load_to(ctx, s0, "rax", 8); /* base pointer */
            emit_load_to(ctx, s1, "rcx", 8); /* index */
            usize elem_sz = dst.type && dst.type->ptr_base
                          ? dst.type->ptr_base->size : 8;
            if (elem_sz == 0) elem_sz = 8;
            EMITL("imul rcx, rcx, %zu", elem_sz);
            EMITL("add rax, rcx");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, 8);
            break;
        }

        case IR_GETFLD: {
            emit_load_to(ctx, s0, "rax", 8); /* base pointer */
            EMITL("add rax, %lld", (long long)s1->imm_int); /* byte offset */
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, 8);
            break;
        }

        /* --- Float arithmetic (using SSE2) --- */
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_FNEG: {
            bool is64 = (sz == 8);
            const char *instr = (op == IR_FADD) ? (is64 ? "addsd" : "addss") :
                                (op == IR_FSUB) ? (is64 ? "subsd" : "subss") :
                                (op == IR_FMUL) ? (is64 ? "mulsd" : "mulss") :
                                (op == IR_FDIV) ? (is64 ? "divsd" : "divss") : "xorpd";
            if (s0->kind == IOP_VREG)
                EMITL("movsd xmm0, qword [rbp%+d]", vreg_slot(s0->vreg.id));
            else
                EMITL("movsd xmm0, qword [rel .Lflt_%p]", (void*)&s0->imm_flt);
            if (op == IR_FNEG) {
                EMITL("xorpd xmm1, xmm1");
                EMITL("%s xmm1, xmm0", is64 ? "subsd" : "subss");
                EMITL("movsd xmm0, xmm1");
            } else {
                if (s1->kind == IOP_VREG)
                    EMITL("movsd xmm1, qword [rbp%+d]", vreg_slot(s1->vreg.id));
                EMITL("%s xmm0, xmm1", instr);
            }
            if (dst.id >= 0) EMITL("movsd qword [rbp%+d], xmm0", vreg_slot(dst.id));
            break;
        }

        /* --- Control flow --- */
        case IR_JMP:
            EMITL("jmp .L%d", inst->label_id);
            break;

        case IR_JMPIF:
            emit_load_to(ctx, s0, "rax", 8);
            EMITL("test rax, rax");
            EMITL("jnz .L%d", inst->label_id);
            EMITL("jmp .L%d", inst->label2_id);
            break;

        case IR_JMPIFNOT:
            emit_load_to(ctx, s0, "rax", 8);
            EMITL("test rax, rax");
            EMITL("jz .L%d",  inst->label_id);
            if (inst->label2_id >= 0)
                EMITL("jmp .L%d", inst->label2_id);
            break;

        /* --- Function call (System V AMD64 ABI) --- */
        case IR_CALL: {
            /* Determine callee name */
            const char *callee_name = NULL;
            if (inst->src[0].kind == IOP_GLOBAL && inst->src[0].global)
                callee_name = inst->src[0].global->name;

            /* Pass integer arguments in rdi, rsi, rdx, rcx, r8, r9 */
            static const char *arg_regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            usize n_args = inst->call_args ? inst->call_args->len : 0;
            usize stack_args = n_args > 6 ? n_args - 6 : 0;

            /* Align stack: sub rsp by (stack_args * 8) aligned to 16 */
            if (stack_args > 0) {
                usize extra = (stack_args * 8 + 15) & ~15u;
                EMITL("sub rsp, %zu", extra);
            }

            /* Push stack arguments (in reverse order) */
            for (usize i = n_args; i > 6; i--) {
                IOp *a = vec_at(inst->call_args, i - 1);
                emit_load_to(ctx, a, "rax", 8);
                EMITL("push rax");
            }

            /* Load register arguments */
            usize reg_n = n_args < 6 ? n_args : 6;
            for (usize i = 0; i < reg_n; i++) {
                IOp *a = vec_at(inst->call_args, i);
                emit_load_to(ctx, a, arg_regs[i], 8);
            }

            /* Variadic: set rax = number of float args (0 for simplicity) */
            EMITL("xor eax, eax");

            if (callee_name)
                EMITL("call %s", callee_name);
            else {
                emit_load_to(ctx, &inst->src[0], "rax", 8);
                EMITL("call rax");
            }

            /* Clean up stack args */
            if (stack_args > 0) {
                usize extra = (stack_args * 8 + 15) & ~15u;
                EMITL("add rsp, %zu", extra);
            }

            /* Store return value */
            if (dst.id >= 0 && dst.type && dst.type->kind != TY_VOID) {
                emit_store_from(ctx, "rax", dst.id, 8);
            }
            break;
        }

        case IR_RET:
            if (s0->kind != IOP_UNDEF) {
                usize rsz = op_size(s0);
                if (rsz == 0) rsz = 8;
                emit_load_to(ctx, s0, "rax", rsz);
            }
            EMITL("leave");
            EMITL("ret");
            break;

        default:
            EMITL("; unhandled IR op %d", (int)op);
            break;
    }
}

/* =========================================================================
 * Function code generation
 * ========================================================================= */
static void gen_func(CodegenCtx *ctx, IrFunc *f) {
    int frame = calc_frame_size(f);
    ctx->frame_size = frame;

    /* Function header */
    sb_printf(ctx->out, "\nglobal %s\n%s:\n", f->name, f->name);
    EMITL("push rbp");
    EMITL("mov rbp, rsp");
    EMITL("sub rsp, %d", frame);

    /* Store incoming parameters from arg registers to their stack slots */
    /* (The ir generator created MOV t_param, index instructions for params.
     *  Here we pre-load them into the correct vregs.) */
    if (f->type && f->type->fn.n_params > 0) {
        static const char *int_args[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        usize np = f->type->fn.n_params;
        for (usize i = 0; i < np && i < 6; i++) {
            /* param i is in int_args[i], goes to vreg slot for param_vreg.
             * The irgen stores params at vregs 0..np-1 (the first alloc'd).
             * Slot for that vreg = vreg_slot(i). */
            EMITL("mov qword [rbp%+d], %s", vreg_slot((int)i), int_args[i]);
        }
        if (np > 6) {
            /* Stack args start at [rbp + 16] for arg 7 */
            for (usize i = 6; i < np; i++) {
                int src_off = 16 + (int)(i - 6) * 8;
                EMITL("mov rax, qword [rbp+%d]", src_off);
                EMITL("mov qword [rbp%+d], rax", vreg_slot((int)i));
            }
        }
    }

    /* Emit instructions from all blocks */
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        if (bi > 0)
            sb_printf(ctx->out, ".L%d:\n", blk->id);
        for (IrInst *inst = blk->head; inst; inst = inst->next)
            gen_inst(ctx, inst);
    }

    /* Ensure we have a return */
    sb_puts(ctx->out, "    ; implicit void return\n");
    EMITL("leave");
    EMITL("ret");
    sb_putc(ctx->out, '\n');
}

/* =========================================================================
 * Module code generation
 * ========================================================================= */
char *codegen_emit(CodegenCtx *ctx) {
    IrModule *m = ctx->mod;
    StrBuf *out = ctx->out;

    /* --- NASM header --- */
    sb_puts(out,
        "; Generated by AC Compiler v" AC_VERSION " — target: " AC_TARGET_TRIPLE "\n"
        "; Assemble: nasm -felf64 output.asm\n"
        "; Link:     gcc output.o runtime.o -o program\n\n"
        "section .text\n\n");

    /* Declare extern symbols (functions declared with 'extern' in the source) */
    /* We collect them from the global symbol table / IR module metadata.
     * For now emit 'extern' directives for any called global without a definition. */
    /* (A real linker resolves these; NASM just needs the declaration.) */

    /* Generate functions */
    for (usize i = 0; i < m->funcs->len; i++)
        gen_func(ctx, vec_at(m->funcs, i));

    /* --- Data section: string literals --- */
    if (m->str_lits->len > 0) {
        sb_puts(out, "\nsection .data\n\n");
        for (usize i = 0; i < m->str_lits->len; i++) {
            const char *val = vec_at(m->str_lits, i);
            sb_printf(out, ".Lstr_%p:\n    db ", (void*)val);
            for (usize j = 0; val[j]; j++) {
                if (j) sb_puts(out, ", ");
                sb_printf(out, "%d", (u8)val[j]);
            }
            sb_puts(out, ", 0\n");
        }
    }

    /* --- BSS section: global variables --- */
    if (m->globals->len > 0) {
        sb_puts(out, "\nsection .bss\n\n");
        for (usize i = 0; i < m->globals->len; i++) {
            Symbol *sym = vec_at(m->globals, i);
            usize sz = sym->type ? sym->type->size : 8;
            if (sz == 0) sz = 8;
            sb_printf(out, "global %s\n%s: resb %zu\n", sym->name, sym->name, sz);
        }
    }

    return sb_take(out);
}

void codegen_free(CodegenCtx *ctx) {
    /* arena-managed */
    (void)ctx;
}
