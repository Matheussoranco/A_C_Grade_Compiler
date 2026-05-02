/*
 * semantic.h — Semantic analysis pass for the AC compiler.
 *
 * The semantic pass:
 *   1. Resolves all identifier references.
 *   2. Infers and checks types for every expression and statement.
 *   3. Validates control flow (break/continue within loops, return types).
 *   4. Completes struct type layouts.
 *   5. Performs constant folding for integer/bool expressions.
 *
 * All type annotations are stored directly on AstNode::type.
 * All symbol resolutions are stored on AstNode::sym.
 */
#pragma once
#include "common.h"
#include "ast.h"
#include "types.h"
#include "symbol.h"

typedef struct {
    Arena    *arena;
    TypeCtx  *types;
    SymTable *symtab;

    /* State for control-flow checks */
    int loop_depth;   /* >0 inside a while/for loop                  */
    Type *cur_fn_ret; /* return type of the currently-analyzed fn    */

    /* Collected string literals (for the IR to emit in .data section) */
    Vec *str_lits; /* Vec<const char*> */
} SemaCtx;

SemaCtx *sema_new(Arena *arena, TypeCtx *types);

/* Main entry point: analyzes the whole program.
 * Returns false if any errors were emitted. */
bool sema_analyze(SemaCtx *ctx, AstNode *program);
