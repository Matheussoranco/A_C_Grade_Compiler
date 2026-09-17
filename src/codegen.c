/*
 * codegen.c — x86-64 NASM code generation.
 *
 * Architecture: System V AMD64 ABI (Linux ELF64).
 * Register allocation: stack-slot model (no register promotion yet).
 *
 * Stack frame layout:
 *   [rbp + 16 + 8*(n-1)]  argument n (if > 6 args)
 *   [rbp + 16]             argument 7
 *   [rbp +  8]             saved return address (pushed by call)
 *   [rbp +  0]             saved rbp
 *   [rbp -  8]             first local / spill slot
 *   ...
 *
 * This codegen uses a "virtual-register → stack slot" model: each virtual
 * register is assigned a unique stack slot at [rbp - offset]. This guarantees
 * correctness for any number of vregs. A linear-scan pass to promote vregs to
 * physical registers is designed for (see LiveInterval in codegen.h) but not
 * yet implemented, so at present every vreg lives on the stack.
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
static const char *reg_small(const char *reg64, usize size) {
    if (size == 8) return reg64;
    if (size == 4) {
        if      (strcmp(reg64,"rax")==0) return "eax";
        else if (strcmp(reg64,"rbx")==0) return "ebx";
        else if (strcmp(reg64,"rcx")==0) return "ecx";
        else if (strcmp(reg64,"rdx")==0) return "edx";
        else if (strcmp(reg64,"rsi")==0) return "esi";
        else if (strcmp(reg64,"rdi")==0) return "edi";
        else if (strcmp(reg64,"r8") ==0) return "r8d";
        else if (strcmp(reg64,"r9") ==0) return "r9d";
        else if (strcmp(reg64,"r10")==0) return "r10d";
        else if (strcmp(reg64,"r11")==0) return "r11d";
        else if (strcmp(reg64,"r12")==0) return "r12d";
        else if (strcmp(reg64,"r13")==0) return "r13d";
        else if (strcmp(reg64,"r14")==0) return "r14d";
        else if (strcmp(reg64,"r15")==0) return "r15d";
        else return reg64;
    }
    if (size == 2) {
        if      (strcmp(reg64,"rax")==0) return "ax";
        else if (strcmp(reg64,"rbx")==0) return "bx";
        else if (strcmp(reg64,"rcx")==0) return "cx";
        else if (strcmp(reg64,"rdx")==0) return "dx";
        else if (strcmp(reg64,"rsi")==0) return "si";
        else if (strcmp(reg64,"rdi")==0) return "di";
        else if (strcmp(reg64,"r8") ==0) return "r8w";
        else if (strcmp(reg64,"r9") ==0) return "r9w";
        else if (strcmp(reg64,"r10")==0) return "r10w";
        else if (strcmp(reg64,"r11")==0) return "r11w";
        else if (strcmp(reg64,"r12")==0) return "r12w";
        else if (strcmp(reg64,"r13")==0) return "r13w";
        else if (strcmp(reg64,"r14")==0) return "r14w";
        else if (strcmp(reg64,"r15")==0) return "r15w";
        else return reg64;
    }
    if (size == 1) {
        if      (strcmp(reg64,"rax")==0) return "al";
        else if (strcmp(reg64,"rbx")==0) return "bl";
        else if (strcmp(reg64,"rcx")==0) return "cl";
        else if (strcmp(reg64,"rdx")==0) return "dl";
        else if (strcmp(reg64,"rsi")==0) return "sil";
        else if (strcmp(reg64,"rdi")==0) return "dil";
        else if (strcmp(reg64,"r8") ==0) return "r8b";
        else if (strcmp(reg64,"r9") ==0) return "r9b";
        else if (strcmp(reg64,"r10")==0) return "r10b";
        else if (strcmp(reg64,"r11")==0) return "r11b";
        else if (strcmp(reg64,"r12")==0) return "r12b";
        else if (strcmp(reg64,"r13")==0) return "r13b";
        else if (strcmp(reg64,"r14")==0) return "r14b";
        else if (strcmp(reg64,"r15")==0) return "r15b";
        else return reg64;
    }
    return reg64;
}

static bool is_xmm_reg(const char *r) {
    return r && strncmp(r, "xmm", 3) == 0;
}

static void emit_load_to(CodegenCtx *ctx, const IOp *op, const char *reg64,
                          usize size) {
    const char *rname = reg_small(reg64, size);
    /* Fallback via rax for narrow loads into an unmapped register name:
     * load into rax first, then move to the target. Covers any future
     * caller passing a register not in reg_small (e.g. r10/r11 aliases). */
    bool unmapped_narrow = (size == 1 || size == 2) && !is_xmm_reg(reg64) &&
                           strcmp(rname, reg64) == 0 && strcmp(reg64, "rax") != 0;

    switch (op->kind) {
        case IOP_IMM_INT:
            EMITL("mov %s, %lld", rname, (long long)op->imm_int);
            break;
        case IOP_IMM_STR:
            EMITL("lea %s, [rel .Lstr_%p]", reg64, (void*)op->imm_str);
            break;
        case IOP_VREG:
            if (unmapped_narrow) {
                /* Fallback: load narrow value into rax, then mov to target. */
                if (size == 2)
                    EMITL("movsx rax, word [rbp%+d]", vreg_slot(op->vreg.id));
                else
                    EMITL("movsx rax, byte [rbp%+d]", vreg_slot(op->vreg.id));
                EMITL("mov %s, rax", reg64);
            } else if (size == 8)
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
        case IOP_IMM_FLT: {
            /* Float imediato: materializa bits em rax e move para o destino
             * (reg64 GPR ou xmm). Sem .rodata extra. */
            uint64_t bits = 0;
            memcpy(&bits, &op->imm_flt, sizeof bits);
            EMITL("mov rax, 0x%llx ; float literal %g", (unsigned long long)bits, op->imm_flt);
            if (is_xmm_reg(reg64))
                EMITL("movq %s, rax", reg64);
            else if (strcmp(reg64, "rax") != 0)
                EMITL("mov %s, rax", reg64);
            break;
        }
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
/* Dedicated backing storage for IR_ALLOCA: each alloca vreg gets its own
 * region sized by the pointee type (aligned to 8), placed below the vreg
 * spill area. Table built per-function in gen_func. */
static int *g_alloca_off = NULL;
static int  g_alloca_cap = 0;
static int  g_alloca_bump = 0;
static int  g_alloca_base = 0;

static usize alloca_slot_size(VReg dst) {
    usize sz = 8;
    if (dst.type && dst.type->ptr_base && dst.type->ptr_base->size)
        sz = dst.type->ptr_base->size;
    if (sz == 0) sz = 8;
    sz = (sz + 7u) & ~7u; /* align slot to 8 */
    return sz;
}

static int calc_frame_size(IrFunc *f) {
    /* Each vreg gets 8 bytes; align to 16 */
    int n_vregs = f->next_vreg;
    /* Also count local variables explicitly allocated, using the real
     * (8-aligned) pointee size so structs/arrays don't overflow 8B slots. */
    int extra = 0;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            if (inst->op == IR_ALLOCA && inst->dst.id >= 0) {
                extra += (int)alloca_slot_size(inst->dst);
            }
        }
    }
    int size = n_vregs * 8 + extra + 8; /* +8 for alignment buffer */
    return (size + 15) & ~15; /* align to 16 */
}

/* Build per-function alloca offset table. Must be called after frame size
 * is known and before emitting instructions of `f`. Offsets are positive
 * magnitudes: storage for vreg `id` lives at [rbp - off]. */
static void build_alloca_table(IrFunc *f) {
    free(g_alloca_off);
    g_alloca_off = NULL;
    g_alloca_cap = 0;
    g_alloca_bump = 0;
    g_alloca_base = f->next_vreg * 8 + 8;
    if (f->next_vreg <= 0) return;
    g_alloca_cap = f->next_vreg;
    g_alloca_off = calloc((usize)g_alloca_cap, sizeof(int));
    if (!g_alloca_off) { g_alloca_cap = 0; return; }
    int cur = g_alloca_base;
    for (usize bi = 0; bi < f->blocks->len; bi++) {
        IrBlock *blk = vec_at(f->blocks, bi);
        for (IrInst *inst = blk->head; inst; inst = inst->next) {
            if (inst->op == IR_ALLOCA && inst->dst.id >= 0 &&
                inst->dst.id < g_alloca_cap) {
                usize sz = alloca_slot_size(inst->dst);
                /* Align each region start to 8 relative to rbp. */
                cur = (int)(((usize)cur + 7u) & ~7u);
                /* Storage occupies [rbp-cur-sz, rbp-cur); pointer = bottom. */
                g_alloca_off[inst->dst.id] = cur + (int)sz;
                cur += (int)sz;
            }
        }
    }
    g_alloca_bump = cur;
    (void)g_alloca_bump;
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
            /* The alloca vreg stores the *address* of a dedicated slot sized
             * by the real pointee type (see build_alloca_table), not the
             * vreg's own 8B spill slot. */
            if (dst.id >= 0) {
                int off = 0;
                if (g_alloca_off && dst.id < g_alloca_cap && g_alloca_off[dst.id] != 0)
                    off = g_alloca_off[dst.id];
                else
                    off = -(vreg_slot(dst.id)); /* fallback: own slot */
                EMITL("lea rax, [rbp-%d]", off);
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
            bool is_flt = (op == IR_FEQ || op == IR_FNE || op == IR_FLT ||
                           op == IR_FLE || op == IR_FGT || op == IR_FGE);
            if (is_flt) {
                /* Comparação float: ucomisd/qword para f64, ucomiss/dword
                 * para f32 (largura do operando-fonte, não do dst i1).
                 * NaN: EQ exige ordered (NP) + equal (Z). */
                bool f64 = (op_size(s0) != 4);
                const char *fmov = f64 ? "movsd" : "movss";
                const char *fmem = f64 ? "qword" : "dword";
                if (s0->kind == IOP_VREG)
                    EMITL("%s xmm0, %s [rbp%+d]", fmov, fmem, vreg_slot(s0->vreg.id));
                else if (s0->kind == IOP_IMM_FLT) {
                    if (f64) {
                        uint64_t b = 0; memcpy(&b, &s0->imm_flt, sizeof b);
                        EMITL("mov rax, 0x%llx", (unsigned long long)b);
                        EMITL("movq xmm0, rax");
                    } else {
                        float f = (float)s0->imm_flt;
                        uint32_t b = 0; memcpy(&b, &f, sizeof b);
                        EMITL("mov eax, 0x%x", b);
                        EMITL("movd xmm0, eax");
                    }
                } else
                    emit_load_to(ctx, s0, "rax", 8), EMITL("movq xmm0, rax");
                if (s1->kind == IOP_VREG)
                    EMITL("%s xmm1, %s [rbp%+d]", fmov, fmem, vreg_slot(s1->vreg.id));
                else if (s1->kind == IOP_IMM_FLT) {
                    if (f64) {
                        uint64_t b = 0; memcpy(&b, &s1->imm_flt, sizeof b);
                        EMITL("mov rax, 0x%llx", (unsigned long long)b);
                        EMITL("movq xmm1, rax");
                    } else {
                        float f = (float)s1->imm_flt;
                        uint32_t b = 0; memcpy(&b, &f, sizeof b);
                        EMITL("mov eax, 0x%x", b);
                        EMITL("movd xmm1, eax");
                    }
                } else
                    emit_load_to(ctx, s1, "rax", 8), EMITL("movq xmm1, rax");
                EMITL(f64 ? "ucomisd xmm0, xmm1" : "ucomiss xmm0, xmm1");
                const char *setcc;
                switch (op) {
                    case IR_FEQ: setcc = "sete";  break;
                    case IR_FNE: setcc = "setne"; break;
                    case IR_FLT: setcc = "setb";  break;
                    case IR_FLE: setcc = "setbe"; break;
                    case IR_FGT: setcc = "seta";  break;
                    case IR_FGE: setcc = "setae"; break;
                    default:    setcc = "sete";  break;
                }
                if (op == IR_FEQ) {
                    /* NaN != NaN: exige ordered (NP) + equal (Z). */
                    EMITL("setnp ah");
                    EMITL("%s al", setcc);
                    EMITL("and al, ah");
                } else {
                    EMITL("%s al", setcc);
                }
                EMITL("movzx rax, al");
            } else {
                emit_load_to(ctx, s0, "rax", sz);
                emit_load_to(ctx, s1, "rcx", sz);
                EMITL("cmp rax, rcx");
                const char *setcc;
                switch (op) {
                    case IR_EQ:  setcc = "sete";  break;
                    case IR_NE:  setcc = "setne"; break;
                    case IR_SLT: setcc = "setl";  break;
                    case IR_SLE: setcc = "setle"; break;
                    case IR_SGT: setcc = "setg";  break;
                    case IR_SGE: setcc = "setge"; break;
                    case IR_ULT: setcc = "setb";  break;
                    case IR_ULE: setcc = "setbe"; break;
                    case IR_UGT: setcc = "seta";  break;
                    case IR_UGE: setcc = "setae"; break;
                    default:     setcc = "sete";  break;
                }
                EMITL("%s al", setcc);
                EMITL("movzx rax, al");
            }
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
            /* n==8 (64 bits) não precisa de máscara; shift de 64 é UB. */
            if (op_size(s0) < 8)
                EMITL("and rax, 0x%llx", (unsigned long long)((1ULL << (op_size(s0)*8)) - 1));
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_TRUNC:
            emit_load_to(ctx, s0, "rax", op_size(s0));
            /* Truncation: just store with smaller size */
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;

        case IR_ITOF: case IR_UITOF: {
            /* Integer -> float: cvtsi2sd for f64, cvtsi2ss for f32. */
            bool dst64 = (sz == 8);
            emit_load_to(ctx, s0, "rax", op_size(s0));
            if (dst64) {
                EMITL("cvtsi2sd xmm0, rax");
                if (dst.id >= 0) EMITL("movsd qword [rbp%+d], xmm0", vreg_slot(dst.id));
            } else {
                EMITL("cvtsi2ss xmm0, rax");
                if (dst.id >= 0) EMITL("movss dword [rbp%+d], xmm0", vreg_slot(dst.id));
            }
            break;
        }

        case IR_FTOI: {
            /* Float -> int: width comes from the source operand, not dst. */
            bool src64 = (op_size(s0) != 4);
            if (s0->kind == IOP_VREG) {
                if (src64)
                    EMITL("movsd xmm0, qword [rbp%+d]", vreg_slot(s0->vreg.id));
                else
                    EMITL("movss xmm0, dword [rbp%+d]", vreg_slot(s0->vreg.id));
            } else if (s0->kind == IOP_IMM_FLT) {
                if (src64) {
                    uint64_t b = 0; memcpy(&b, &s0->imm_flt, sizeof b);
                    EMITL("mov rax, 0x%llx", (unsigned long long)b);
                    EMITL("movq xmm0, rax");
                } else {
                    float f = (float)s0->imm_flt;
                    uint32_t b = 0; memcpy(&b, &f, sizeof b);
                    EMITL("mov eax, 0x%x", b);
                    EMITL("movd xmm0, eax");
                }
            } else {
                emit_load_to(ctx, s0, "rax", 8);
                EMITL("movq xmm0, rax");
            }
            EMITL(src64 ? "cvttsd2si rax, xmm0" : "cvttss2si rax, xmm0");
            if (dst.id >= 0) emit_store_from(ctx, "rax", dst.id, sz);
            break;
        }

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

        /* --- Float arithmetic (using SSE2) ---
         * f64 (sz==8) uses movsd/qword + *sd; f32 (sz==4) uses movss/dword
         * + *ss. f32 immediates are rounded from the IR double to a 32-bit
         * float and materialised via eax/movd (not movq, which would carry
         * a 64-bit pattern). There is no implicit promotion: values stay
         * in their own width; cross-width conversion happens only in
         * IR_ITOF/IR_FTOI and explicit cvtss2sd/cvtsd2ss nodes. */
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_FNEG: {
            bool is64 = (sz == 8);
            const char *instr = (op == IR_FADD) ? (is64 ? "addsd" : "addss") :
                                (op == IR_FSUB) ? (is64 ? "subsd" : "subss") :
                                (op == IR_FMUL) ? (is64 ? "mulsd" : "mulss") :
                                (op == IR_FDIV) ? (is64 ? "divsd" : "divss") : "xorpd";
            const char *mov = is64 ? "movsd" : "movss";
            const char *mem = is64 ? "qword" : "dword";
            if (s0->kind == IOP_VREG)
                EMITL("%s xmm0, %s [rbp%+d]", mov, mem, vreg_slot(s0->vreg.id));
            else if (s0->kind == IOP_IMM_FLT) {
                if (is64) {
                    uint64_t b = 0; memcpy(&b, &s0->imm_flt, sizeof b);
                    EMITL("mov rax, 0x%llx", (unsigned long long)b);
                    EMITL("movq xmm0, rax");
                } else {
                    float f = (float)s0->imm_flt;
                    uint32_t b = 0; memcpy(&b, &f, sizeof b);
                    EMITL("mov eax, 0x%x ; float literal %g", b, (double)f);
                    EMITL("movd xmm0, eax");
                }
            } else {
                emit_load_to(ctx, s0, "rax", 8);
                EMITL("movq xmm0, rax");
                if (!is64) EMITL("cvtss2sd xmm0, xmm0 ; narrow GPR spill to f32");
            }
            if (op == IR_FNEG) {
                if (is64) {
                    EMITL("xorpd xmm1, xmm1");
                    EMITL("subsd xmm1, xmm0");
                    EMITL("movsd xmm0, xmm1");
                } else {
                    EMITL("xorps xmm1, xmm1");
                    EMITL("subss xmm1, xmm0");
                    EMITL("movss xmm0, xmm1");
                }
            } else {
                if (s1->kind == IOP_VREG)
                    EMITL("%s xmm1, %s [rbp%+d]", mov, mem, vreg_slot(s1->vreg.id));
                else if (s1->kind == IOP_IMM_FLT) {
                    if (is64) {
                        uint64_t b = 0; memcpy(&b, &s1->imm_flt, sizeof b);
                        EMITL("mov rax, 0x%llx", (unsigned long long)b);
                        EMITL("movq xmm1, rax");
                    } else {
                        float f = (float)s1->imm_flt;
                        uint32_t b = 0; memcpy(&b, &f, sizeof b);
                        EMITL("mov eax, 0x%x ; float literal %g", b, (double)f);
                        EMITL("movd xmm1, eax");
                    }
                } else {
                    emit_load_to(ctx, s1, "rax", 8);
                    EMITL("movq xmm1, rax");
                    if (!is64) EMITL("cvtss2sd xmm1, xmm1 ; narrow GPR spill to f32");
                }
                EMITL("%s xmm0, xmm1", instr);
            }
            if (dst.id >= 0) EMITL("%s %s [rbp%+d], xmm0", mov, mem, vreg_slot(dst.id));
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

            /* System V AMD64: int args in rdi,rsi,rdx,rcx,r8,r9; f64/f32
             * args in xmm0-7; overflow (either class) on stack in order. */
            static const char *int_regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            static const char *flt_regs[] = {"xmm0","xmm1","xmm2","xmm3",
                                             "xmm4","xmm5","xmm6","xmm7"};
            usize n_args = inst->call_args ? inst->call_args->len : 0;

            /* First pass: classify each arg as reg or stack. */
            int int_idx = 0, flt_idx = 0;
            usize n_stack = 0;
            for (usize i = 0; i < n_args; i++) {
                IOp *a = vec_at(inst->call_args, i);
                bool is_flt = a->type && ty_is_float(a->type);
                if (is_flt) {
                    if (flt_idx < 8) flt_idx++;
                    else n_stack++;
                } else {
                    if (int_idx < 6) int_idx++;
                    else n_stack++;
                }
            }
            int n_flt_regs = flt_idx; /* vector regs consumed (for AL) */

            /* Reserva única alinhada a 16 e grava com mov (sem push após sub,
             * que duplicava a alocação e quebrava o alinhamento).
             * SysV AMD64: rsp deve estar 16-alinhado ANTES do call (após o
             * call o callee observa rsp%16==8 pelo return address). No prólogo
             * fizemos `push rbp` (8B: 8->0) + `sub rsp, frame` com frame
             * 16-alinhado (calc_frame_size), logo na entrada do corpo rsp%16==0.
             * `extra` abaixo também é 16-alinhado, preservando rsp%16==0 até o call. */
            usize extra = 0;
            if (n_stack > 0) {
                extra = (n_stack * 8 + 15) & ~15u;
                EMITL("sub rsp, %zu", extra);
            }

            /* Second pass: emit moves. Stack args in order via mov. */
            int di = 0, fi = 0;
            usize stack_pos = 0;
            for (usize i = 0; i < n_args; i++) {
                IOp *a = vec_at(inst->call_args, i);
                bool is_flt = a->type && ty_is_float(a->type);
                if (is_flt) {
                    if (fi < 8) {
                        const char *xmm = flt_regs[fi++];
                        if (a->kind == IOP_VREG)
                            EMITL("movsd %s, qword [rbp%+d]", xmm, vreg_slot(a->vreg.id));
                        else if (a->kind == IOP_IMM_FLT) {
                            uint64_t b = 0; memcpy(&b, &a->imm_flt, sizeof b);
                            EMITL("mov rax, 0x%llx", (unsigned long long)b);
                            EMITL("movq %s, rax", xmm);
                        } else {
                            emit_load_to(ctx, a, "rax", 8);
                            EMITL("movq %s, rax", xmm);
                        }
                    } else {
                        /* Float overflow: store 8-byte bits on stack. */
                        if (a->kind == IOP_VREG)
                            EMITL("movsd xmm15, qword [rbp%+d]", vreg_slot(a->vreg.id));
                        else if (a->kind == IOP_IMM_FLT) {
                            uint64_t b = 0; memcpy(&b, &a->imm_flt, sizeof b);
                            EMITL("mov rax, 0x%llx", (unsigned long long)b);
                            EMITL("movq xmm15, rax");
                        } else {
                            emit_load_to(ctx, a, "rax", 8);
                            EMITL("movq xmm15, rax");
                        }
                        EMITL("movq rax, xmm15");
                        EMITL("mov [rsp+%zu], rax", stack_pos * 8);
                        stack_pos++;
                    }
                } else {
                    if (di < 6) {
                        emit_load_to(ctx, a, int_regs[di++], 8);
                    } else {
                        emit_load_to(ctx, a, "rax", 8);
                        EMITL("mov [rsp+%zu], rax", stack_pos * 8);
                        stack_pos++;
                    }
                }
            }

            /* Variadic (System V): AL = number of vector regs used. */
            if (n_flt_regs == 0)
                EMITL("xor eax, eax");
            else
                EMITL("mov eax, %d", n_flt_regs);

            if (callee_name)
                EMITL("call %s", callee_name);
            else {
                emit_load_to(ctx, &inst->src[0], "rax", 8);
                EMITL("call rax");
            }

            /* Clean up stack args */
            if (extra > 0) {
                EMITL("add rsp, %zu", extra);
            }

            /* Store return value: float returns arrive in xmm0, int in rax. */
            if (dst.id >= 0 && dst.type && dst.type->kind != TY_VOID) {
                if (ty_is_float(dst.type))
                    EMITL("movsd qword [rbp%+d], xmm0", vreg_slot(dst.id));
                else
                    emit_store_from(ctx, "rax", dst.id, 8);
            }
            break;
        }

        case IR_RET: {
            if (s0->kind != IOP_UNDEF) {
                bool ret_flt = s0->type && ty_is_float(s0->type);
                if (ret_flt) {
                    if (s0->kind == IOP_VREG)
                        EMITL("movsd xmm0, qword [rbp%+d]", vreg_slot(s0->vreg.id));
                    else if (s0->kind == IOP_IMM_FLT) {
                        uint64_t b = 0; memcpy(&b, &s0->imm_flt, sizeof b);
                        EMITL("mov rax, 0x%llx", (unsigned long long)b);
                        EMITL("movq xmm0, rax");
                    } else {
                        emit_load_to(ctx, s0, "rax", 8);
                        EMITL("movq xmm0, rax");
                    }
                } else {
                    usize rsz = op_size(s0);
                    if (rsz == 0) rsz = 8;
                    emit_load_to(ctx, s0, "rax", rsz);
                }
            }
            EMITL("leave");
            EMITL("ret");
            break;
        }

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
    build_alloca_table(f);

    /* Function header */
    sb_printf(ctx->out, "\nglobal %s\n%s:\n", f->name, f->name);
    EMITL("push rbp");
    EMITL("mov rbp, rsp");
    EMITL("sub rsp, %d", frame);

    /* Store incoming parameters from arg registers to their stack slots */
    /* (The ir generator created MOV t_param, index instructions for params.
     *  Here we pre-load them into the correct vregs.)
     * System V: ints in rdi,rsi,rdx,rcx,r8,r9; floats in xmm0-7;
     * overflow (either class) on stack at [rbp+16], [rbp+24], ... in order. */
    if (f->type && f->type->fn.n_params > 0) {
        static const char *int_args[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        static const char *flt_args[] = {"xmm0","xmm1","xmm2","xmm3",
                                         "xmm4","xmm5","xmm6","xmm7"};
        usize np = f->type->fn.n_params;
        int di = 0, fi = 0;
        int stack_n = 0;
        for (usize i = 0; i < np; i++) {
            Type *pt = f->type->fn.params ? f->type->fn.params[i] : NULL;
            bool is_flt = pt && ty_is_float(pt);
            if (is_flt) {
                if (fi < 8) {
                    EMITL("movsd qword [rbp%+d], %s", vreg_slot((int)i), flt_args[fi++]);
                } else {
                    int src_off = 16 + stack_n * 8;
                    EMITL("mov rax, qword [rbp+%d]", src_off);
                    EMITL("mov qword [rbp%+d], rax", vreg_slot((int)i));
                    stack_n++;
                }
            } else {
                if (di < 6) {
                    /* param i is in int_args[di], goes to vreg slot for param_vreg.
                     * The irgen stores params at vregs 0..np-1 (the first alloc'd).
                     * Slot for that vreg = vreg_slot(i). */
                    EMITL("mov qword [rbp%+d], %s", vreg_slot((int)i), int_args[di++]);
                } else {
                    /* Stack args start at [rbp + 16] for first overflow arg */
                    int src_off = 16 + stack_n * 8;
                    EMITL("mov rax, qword [rbp+%d]", src_off);
                    EMITL("mov qword [rbp%+d], rax", vreg_slot((int)i));
                    stack_n++;
                }
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

    /* Declare extern symbols (functions declared with 'extern' in the source).
     * irgen skips codegen for `extern fn` (irgen.c: gen_fn returns early), so
     * m->funcs holds only defined functions. Any IR_CALL callee that names a
     * global not defined in this module is an external (e.g. libc printf) and
     * NASM requires an `extern <nome>` declaration, otherwise `call printf`
     * fails to assemble/link. */
    {
        const char *seen[1024];
        usize n_seen = 0;
        for (usize i = 0; i < m->funcs->len; i++) {
            IrFunc *f = vec_at(m->funcs, i);
            for (usize bi = 0; bi < f->blocks->len; bi++) {
                IrBlock *blk = vec_at(f->blocks, bi);
                for (IrInst *inst = blk->head; inst; inst = inst->next) {
                    if (inst->op != IR_CALL) continue;
                    if (inst->src[0].kind != IOP_GLOBAL || !inst->src[0].global ||
                        !inst->src[0].global->name)
                        continue;
                    const char *name = inst->src[0].global->name;
                    /* Skip if defined locally in this module. */
                    bool defined = false;
                    for (usize k = 0; k < m->funcs->len; k++) {
                        IrFunc *g = vec_at(m->funcs, k);
                        if (g->name && strcmp(g->name, name) == 0) { defined = true; break; }
                    }
                    if (defined) continue;
                    /* Deduplicate. */
                    bool dup = false;
                    for (usize k = 0; k < n_seen; k++) {
                        if (strcmp(seen[k], name) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    if (n_seen < sizeof(seen) / sizeof(seen[0]))
                        seen[n_seen++] = name;
                    sb_printf(out, "extern %s\n", name);
                }
            }
        }
        if (n_seen > 0) sb_putc(out, '\n');
    }

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
