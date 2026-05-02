/*
 * irgen.h — AST → TAC IR lowering pass.
 *
 * Walks the semantically annotated AST and emits IR instructions through
 * the IrBuilder interface.  All type information and symbol links must be
 * filled in by the semantic pass before calling irgen_program().
 */
#pragma once
#include "common.h"
#include "ast.h"
#include "ir.h"
#include "symbol.h"

typedef struct {
    IrBuilder *b;
    Arena     *arena;
    TypeCtx   *types;
    /* Stack of (break_label, continue_label) pairs for nested loops */
    i32 *break_stack;
    i32 *cont_stack;
    int  loop_sp;
    int  loop_cap;
} IrGenCtx;

IrGenCtx  *irgen_new(IrBuilder *b, Arena *arena, TypeCtx *types);
IrModule  *irgen_program(IrGenCtx *ctx, AstNode *program);
