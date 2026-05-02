/*
 * ir.h — Three-Address Code (TAC) Intermediate Representation.
 *
 * The IR sits between the semantic AST and the backend code generator.
 * Each function is represented as a list of basic blocks; each block
 * is a list of TAC instructions with explicit virtual registers and
 * typed operands.
 *
 * Design:
 *   - Virtual registers (VReg) are SSA-lite: each has a unique id and type.
 *   - Immediates carry a type for accurate backend lowering.
 *   - Labels are just integers; the IrFunc owns a name table.
 *   - Phi nodes are reserved for a future full-SSA conversion.
 */
#pragma once
#include "common.h"
#include "types.h"
#include "symbol.h"

/* =========================================================================
 * Virtual register
 * ========================================================================= */
typedef struct {
    i32         id;    /* unique id within a function; -1 = invalid        */
    Type       *type;
    const char *name;  /* optional debug name                              */
} VReg;

#define VREG_INVALID ((VReg){-1, NULL, NULL})

/* =========================================================================
 * IR operand — a value that can appear as an instruction operand.
 * ========================================================================= */
typedef enum {
    IOP_VREG,       /* virtual register                                    */
    IOP_IMM_INT,    /* integer immediate                                   */
    IOP_IMM_FLT,    /* float immediate                                     */
    IOP_IMM_STR,    /* string literal label                                */
    IOP_LABEL,      /* a branch target (block label id)                    */
    IOP_GLOBAL,     /* global variable symbol                              */
    IOP_UNDEF,      /* undefined value (for uninitialized)                 */
} IOpKind;

typedef struct {
    IOpKind kind;
    Type   *type;
    union {
        VReg        vreg;
        i64         imm_int;
        f64         imm_flt;
        const char *imm_str;  /* string literal name / value               */
        i32         label_id; /* basic block label                         */
        Symbol     *global;
    };
} IOp;

/* =========================================================================
 * IR opcodes
 * ========================================================================= */
typedef enum {
    IR_NOP,

    /* Data movement */
    IR_MOV,         /* dst = src                                           */
    IR_LOAD,        /* dst = *ptr         (pointer dereference)            */
    IR_STORE,       /* *ptr = val                                          */
    IR_LEA,         /* dst = &var         (address of symbol)              */
    IR_GLOAD,       /* dst = global       (load from global)               */
    IR_GSTORE,      /* global = val       (store to global)                */
    IR_ALLOCA,      /* dst = alloca(n)    (stack allocation, returns ptr)  */

    /* Arithmetic */
    IR_ADD,  IR_SUB,  IR_MUL,  IR_SDIV, IR_UDIV,
    IR_SREM, IR_UREM,
    IR_NEG,

    /* Bitwise */
    IR_AND, IR_OR, IR_XOR, IR_NOT,
    IR_SHL, IR_LSHR, IR_ASHR,

    /* Comparison — produce i32 0/1 */
    IR_EQ, IR_NE,
    IR_SLT, IR_SLE, IR_SGT, IR_SGE,  /* signed   */
    IR_ULT, IR_ULE, IR_UGT, IR_UGE,  /* unsigned */

    /* Float arithmetic */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG,
    IR_FEQ,  IR_FNE,  IR_FLT,  IR_FLE,  IR_FGT,  IR_FGE,

    /* Type conversions */
    IR_SEXT,    /* sign-extend smaller int → larger int                    */
    IR_ZEXT,    /* zero-extend smaller int → larger int                    */
    IR_TRUNC,   /* truncate larger int → smaller int                       */
    IR_ITOF,    /* signed int → float                                      */
    IR_UITOF,   /* unsigned int → float                                    */
    IR_FTOI,    /* float → signed int (truncate)                           */
    IR_BITCAST, /* reinterpret bits (ptr ↔ int of same size)               */
    IR_PTRTOINT, IR_INTTOPTR,
    IR_FPEXT,   /* f32 → f64                                               */
    IR_FPTRUNC, /* f64 → f32                                               */

    /* Array / struct member access */
    IR_GETIDX,  /* dst = base[idx]  (element pointer)                      */
    IR_GETFLD,  /* dst = &base->field_offset  (field pointer)              */

    /* Control flow */
    IR_JMP,         /* unconditional jump to label                         */
    IR_JMPIF,       /* if cond (i32 != 0) jump to label, else fall-through */
    IR_JMPIFNOT,    /* if cond == 0 jump to label, else fall-through        */

    /* Function call */
    IR_CALL,        /* dst = callee(args...)                               */
    IR_RET,         /* return [val]                                        */

    /* Pseudo — used by optimiser, removed before codegen */
    IR_PHI,         /* SSA phi node: dst = phi([(val, label)...])          */
    IR_LABEL_DEF,   /* marker for a label inside the instruction stream    */
} IrOp;

/* =========================================================================
 * IR instruction
 * ========================================================================= */
typedef struct IrInst IrInst;
struct IrInst {
    IrOp     op;
    SrcLoc   loc;
    VReg     dst;         /* destination; id == -1 means no result          */
    IOp      src[3];      /* operands (up to 3; unused slots are IOP_UNDEF) */
    /* For IR_CALL: src[0] = callee, src[1..n-1] = args */
    Vec     *call_args;   /* Vec<IOp*> — used only for IR_CALL              */
    i32      label_id;    /* for IR_LABEL_DEF / IR_JMP / IR_JMPIF           */
    i32      label2_id;   /* for IR_JMPIF: fall-through label               */
    IrInst  *next;
    IrInst  *prev;
};

/* =========================================================================
 * Basic block
 * ========================================================================= */
typedef struct IrBlock {
    i32      id;
    const char *name;   /* optional: "entry", "if.then", etc.              */
    IrInst  *head;      /* doubly-linked instruction list                  */
    IrInst  *tail;
    usize    n_insts;
    /* Control flow graph */
    struct IrBlock *succ[2];  /* up to 2 successors (branch / fall-through) */
    Vec            *preds;    /* Vec<IrBlock*>                               */
    /* Liveness (filled by liveness analysis) */
    u64     *live_in;   /* bit set over VReg ids                           */
    u64     *live_out;
} IrBlock;

/* =========================================================================
 * IR function
 * ========================================================================= */
typedef struct IrFunc {
    const char *name;
    Type       *type;        /* TY_FN                                       */
    Vec        *blocks;      /* Vec<IrBlock*>, blocks[0] = entry            */
    i32         next_vreg;   /* next virtual register id to allocate        */
    i32         next_label;  /* next basic block id                         */
    Vec        *vregs;       /* Vec<VReg*>, indexed by vreg id              */
    /* Stack frame info (filled by codegen) */
    i32         frame_size;  /* bytes allocated on stack                    */
    Vec        *locals;      /* Vec<Symbol*> of local variables             */
    Symbol     *fn_sym;
} IrFunc;

/* =========================================================================
 * IR module — the whole program
 * ========================================================================= */
typedef struct {
    Vec    *funcs;     /* Vec<IrFunc*>                                      */
    Vec    *globals;   /* Vec<Symbol*> for global variables                 */
    Vec    *str_lits;  /* Vec<const char*> string literal values            */
    Arena  *arena;
} IrModule;

/* =========================================================================
 * Builder API
 * ========================================================================= */
typedef struct {
    IrModule  *mod;
    IrFunc    *fn;
    IrBlock   *cur;   /* insertion point                                   */
    Arena     *arena;
    TypeCtx   *types;
} IrBuilder;

IrModule  *ir_module_new(Arena *arena, TypeCtx *types);
IrFunc    *ir_func_new(IrModule *m, const char *name, Type *fn_type, Symbol *sym);
IrBlock   *ir_block_new(IrFunc *fn, const char *name);
void       ir_set_block(IrBuilder *b, IrBlock *blk);
IrBuilder *ir_builder_new(IrModule *m, Arena *arena, TypeCtx *types);

/* Emit an instruction into the current block */
IrInst *ir_emit(IrBuilder *b, IrOp op, VReg dst, IOp s0, IOp s1, IOp s2, SrcLoc loc);

/* Convenience emitters */
VReg ir_alloc_vreg(IrBuilder *b, Type *t, const char *name);

/* Common instruction builders */
VReg ir_mov(IrBuilder *b, IOp src, SrcLoc loc);
VReg ir_binop(IrBuilder *b, IrOp op, IOp l, IOp r, SrcLoc loc);
VReg ir_unop(IrBuilder *b, IrOp op, IOp src, SrcLoc loc);
VReg ir_call(IrBuilder *b, IOp callee, Vec *args, Type *ret_type, SrcLoc loc);
void ir_ret(IrBuilder *b, IOp val, SrcLoc loc);
void ir_jmp(IrBuilder *b, i32 label, SrcLoc loc);
void ir_jmpif(IrBuilder *b, IOp cond, i32 then_lbl, i32 else_lbl, SrcLoc loc);
VReg ir_load(IrBuilder *b, IOp ptr, Type *t, SrcLoc loc);
void ir_store(IrBuilder *b, IOp ptr, IOp val, SrcLoc loc);
VReg ir_lea(IrBuilder *b, Symbol *sym, SrcLoc loc);
VReg ir_alloca(IrBuilder *b, Type *t, SrcLoc loc);
VReg ir_getidx(IrBuilder *b, IOp base, IOp idx, Type *elem_t, SrcLoc loc);
VReg ir_getfld(IrBuilder *b, IOp base, usize byte_off, Type *fld_t, SrcLoc loc);
VReg ir_cast(IrBuilder *b, IrOp op, IOp src, Type *to, SrcLoc loc);
i32  ir_label_new(IrBuilder *b, const char *name);
void ir_label_def(IrBuilder *b, i32 lbl, SrcLoc loc);

/* Operand constructors */
IOp iop_vreg(VReg r);
IOp iop_imm_int(i64 v, Type *t);
IOp iop_imm_flt(f64 v, Type *t);
IOp iop_imm_str(const char *s, Type *t);
IOp iop_label(i32 id);
IOp iop_global(Symbol *s);
IOp iop_undef(void);

/* String literal registration */
const char *ir_str_label(IrModule *m, const char *val, Arena *arena);

/* IR dump for debugging */
void ir_dump_module(const IrModule *m, FILE *out);
void ir_dump_func(const IrFunc *f, FILE *out);
