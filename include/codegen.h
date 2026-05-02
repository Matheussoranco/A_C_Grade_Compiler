/*
 * codegen.h — x86-64 NASM assembly code generation.
 *
 * Architecture: System V AMD64 ABI (Linux ELF64).
 *
 * Register allocation strategy: Linear Scan (Poletto & Sarkar, 1999).
 *   - Compute live intervals over virtual registers.
 *   - Allocate physical registers in a single left-to-right sweep.
 *   - Spill to the stack frame when all registers are occupied.
 *
 * Generated output is NASM syntax targeting Linux ELF64.
 * Link with: nasm -felf64 output.asm && gcc output.o runtime.o -o prog
 */
#pragma once
#include "common.h"
#include "ir.h"

/* =========================================================================
 * Physical register descriptor
 * ========================================================================= */
typedef enum {
    /* 64-bit general-purpose */
    PREG_RAX = 0,
    PREG_RBX, PREG_RCX, PREG_RDX,
    PREG_RSI, PREG_RDI,
    PREG_R8,  PREG_R9,  PREG_R10, PREG_R11,
    PREG_R12, PREG_R13, PREG_R14, PREG_R15,
    /* Float (SSE) */
    PREG_XMM0, PREG_XMM1, PREG_XMM2, PREG_XMM3,
    PREG_XMM4, PREG_XMM5, PREG_XMM6, PREG_XMM7,
    PREG_XMM8, PREG_XMM9, PREG_XMM10,PREG_XMM11,
    PREG_XMM12,PREG_XMM13,PREG_XMM14,PREG_XMM15,
    PREG_SPILL, /* sentinel: no register assigned, lives on stack          */
    PREG_COUNT,
} PReg;

/* Name strings indexed by PReg for each operand size */
const char *preg_name64(PReg r);
const char *preg_name32(PReg r);
const char *preg_name16(PReg r);
const char *preg_name8(PReg r);
const char *preg_xmm(PReg r);   /* for SSE registers */

/* =========================================================================
 * Live interval (for linear scan)
 * ========================================================================= */
typedef struct {
    i32   vreg_id;
    i32   start;   /* instruction index of first definition               */
    i32   end;     /* instruction index of last use (exclusive)           */
    PReg  preg;    /* assigned physical register (PREG_SPILL if spilled)  */
    i32   spill_off; /* rbp-relative offset if spilled                    */
} LiveInterval;

/* =========================================================================
 * Code generation context
 * ========================================================================= */
typedef struct {
    IrModule   *mod;
    Arena      *arena;
    StrBuf     *out;       /* output assembly text                          */
    int         str_lit_idx; /* counter for .Lstr_N labels                 */
    /* Per-function state (reset for each function) */
    LiveInterval *intervals; /* array indexed by vreg_id                   */
    i32          n_intervals;
    i32          frame_size; /* total bytes for locals + spills (aligned)  */
    i32          spill_base; /* next available spill slot offset from rbp  */
    /* Callee-saved registers actually used — must be push/pop'd */
    bool used_callee_saved[PREG_COUNT];
} CodegenCtx;

/* =========================================================================
 * Public API
 * ========================================================================= */
CodegenCtx *codegen_new(IrModule *mod, Arena *arena);

/* Generate full NASM assembly for the module.
 * Returns a malloc'd null-terminated string with the complete .asm output. */
char *codegen_emit(CodegenCtx *ctx);

void codegen_free(CodegenCtx *ctx);
