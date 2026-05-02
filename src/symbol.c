/*
 * symbol.c — Scoped symbol table implementation.
 */
#include "../include/symbol.h"

/* =========================================================================
 * Scope helpers
 * ========================================================================= */
static Scope *scope_new(Arena *arena, Scope *parent, bool is_fn) {
    Scope *s     = arena_calloc(arena, sizeof(Scope));
    s->parent    = parent;
    s->syms      = hmap_new();
    s->is_fn_scope = is_fn;
    return s;
}

/* =========================================================================
 * Symbol table
 * ========================================================================= */
SymTable *symtab_new(Arena *arena) {
    SymTable *st = arena_calloc(arena, sizeof(SymTable));
    st->arena    = arena;
    st->global   = scope_new(arena, NULL, false);
    st->current  = st->global;
    return st;
}

void symtab_free(SymTable *st) {
    /* Scopes are arena-managed; hmap_free the HMaps */
    Scope *s = st->current;
    while (s) {
        hmap_free(s->syms);
        s = s->parent;
    }
}

void symtab_push_scope(SymTable *st, bool is_fn_scope) {
    Scope *s = scope_new(st->arena, st->current, is_fn_scope);
    st->current = s;
}

void symtab_pop_scope(SymTable *st) {
    assert(st->current != st->global && "cannot pop global scope");
    hmap_free(st->current->syms);
    st->current = st->current->parent;
}

Symbol *symtab_declare(SymTable *st, SymKind kind, const char *name,
                        Type *type, SrcLoc loc) {
    if (hmap_has(st->current->syms, name)) {
        diag_error(loc, "redeclaration of '%s'", name);
        Symbol *prev = hmap_get(st->current->syms, name);
        if (prev) diag_note(prev->decl_loc, "previously declared here");
        return NULL;
    }
    Symbol *sym = sym_new(st->arena, kind, name, type, loc);
    hmap_put(st->current->syms, name, sym);
    return sym;
}

Symbol *symtab_lookup(SymTable *st, const char *name) {
    for (Scope *s = st->current; s; s = s->parent) {
        Symbol *sym = hmap_get(s->syms, name);
        if (sym) return sym;
    }
    return NULL;
}

Symbol *symtab_lookup_local(SymTable *st, const char *name) {
    return hmap_get(st->current->syms, name);
}

Symbol *symtab_lookup_global(SymTable *st, const char *name) {
    return hmap_get(st->global->syms, name);
}

Symbol *sym_new(Arena *arena, SymKind kind, const char *name, Type *type, SrcLoc loc) {
    Symbol *sym   = arena_calloc(arena, sizeof(Symbol));
    sym->kind     = kind;
    sym->name     = name; /* assumed arena/static lifetime */
    sym->type     = type;
    sym->decl_loc = loc;
    sym->ir_reg   = -1;
    sym->stack_off = 0;
    sym->is_global = false;
    return sym;
}
