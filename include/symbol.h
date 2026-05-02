/*
 * symbol.h — Symbol table for the AC compiler.
 *
 * Symbols are organized in a linked chain of scopes (global → function →
 * block → inner block). Each scope owns a hash map of name → Symbol*.
 * Looking up a name walks the chain from innermost to outermost.
 */
#pragma once
#include "common.h"
#include "types.h"

/* Forward */
typedef struct AstNode AstNode;

/* =========================================================================
 * Symbol kinds
 * ========================================================================= */
typedef enum {
    SYM_VAR,      /* local or global variable              */
    SYM_PARAM,    /* function parameter                    */
    SYM_FN,       /* function declaration                  */
    SYM_STRUCT,   /* struct type definition                */
    SYM_CONST,    /* compile-time constant (immutable let) */
} SymKind;

/* =========================================================================
 * Symbol
 * ========================================================================= */
typedef struct Symbol {
    SymKind     kind;
    const char *name;
    Type       *type;
    SrcLoc      decl_loc;
    AstNode    *decl_node;  /* back-pointer to the declaring AST node     */
    bool        is_const;   /* let binding (immutable)                    */

    /* Code-generation annotations (filled by IR/codegen pass) */
    int  ir_reg;    /* virtual register index assigned to this sym (-1 = unset) */
    int  stack_off; /* byte offset from rbp (negative for locals); 0 = unset   */
    bool is_global; /* lives in the .data / .bss section                        */
    const char *asm_label; /* for global variables / string literals            */
} Symbol;

/* =========================================================================
 * Scope — a single lexical scope level.
 * ========================================================================= */
typedef struct Scope {
    struct Scope *parent;
    HMap         *syms;
    bool          is_fn_scope; /* true for the outermost scope of a function    */
} Scope;

/* =========================================================================
 * Symbol table — manages the scope chain.
 * ========================================================================= */
typedef struct {
    Scope  *current;
    Scope  *global;
    Arena  *arena;
} SymTable;

/* =========================================================================
 * Public API
 * ========================================================================= */
SymTable *symtab_new(Arena *arena);
void      symtab_free(SymTable *st);

void      symtab_push_scope(SymTable *st, bool is_fn_scope);
void      symtab_pop_scope(SymTable *st);

/* Declare a symbol in the current scope.
 * Returns NULL and reports an error if already defined in this scope. */
Symbol   *symtab_declare(SymTable *st, SymKind kind, const char *name,
                          Type *type, SrcLoc loc);

/* Look up a name; walks outward from current scope. Returns NULL if not found. */
Symbol   *symtab_lookup(SymTable *st, const char *name);

/* Look up only in current scope (for redeclaration checks). */
Symbol   *symtab_lookup_local(SymTable *st, const char *name);

/* Look up in global scope. */
Symbol   *symtab_lookup_global(SymTable *st, const char *name);

/* Helpers to build a Symbol directly (arena-allocated). */
Symbol   *sym_new(Arena *arena, SymKind kind, const char *name, Type *type, SrcLoc loc);
