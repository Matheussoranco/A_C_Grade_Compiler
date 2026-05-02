/*
 * main.c — AC Compiler driver.
 *
 * Pipeline:
 *   source file → Lexer → Parser → AST → Semantic → IR → Optimizer → Codegen → .asm
 *
 * Usage:
 *   acc <source.ac> [-o output.asm] [-O0|-O1|-O2] [-dump-ast] [-dump-ir] [-v]
 */
#include "../include/common.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/types.h"
#include "../include/symbol.h"
#include "../include/semantic.h"
#include "../include/ir.h"
#include "../include/irgen.h"
#include "../include/optimizer.h"
#include "../include/codegen.h"

/* Need the parse_program declaration */
AstNode *parse_program(Lexer *lex, Arena *arena, TypeCtx *types);

/* =========================================================================
 * Read whole file into a malloc'd buffer
 * ========================================================================= */
static char *read_file(const char *path, usize *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s': ", path); perror(""); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((usize)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    usize n = fread(buf, 1, (usize)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* =========================================================================
 * Main
 * ========================================================================= */
int main(int argc, char **argv) {
    const char *input_file  = NULL;
    const char *output_file = NULL;
    int opt_level  = 1;
    bool dump_ast  = false;
    bool dump_ir   = false;
    bool verbose   = false;
    bool dump_toks = false;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (strcmp(a, "-o")        == 0 && i+1 < argc) output_file = argv[++i];
        else if (strcmp(a, "-O0")       == 0) opt_level = 0;
        else if (strcmp(a, "-O1")       == 0) opt_level = 1;
        else if (strcmp(a, "-O2")       == 0) opt_level = 2;
        else if (strcmp(a, "-dump-ast") == 0) dump_ast  = true;
        else if (strcmp(a, "-dump-ir")  == 0) dump_ir   = true;
        else if (strcmp(a, "-dump-toks")== 0) dump_toks = true;
        else if (strcmp(a, "-v")        == 0) verbose   = true;
        else if (strcmp(a, "--version") == 0) {
            printf("acc " AC_VERSION " (" AC_TARGET_TRIPLE ")\n");
            return 0;
        } else if (strcmp(a, "--help")  == 0) {
            printf("Usage: acc <source.ac> [options]\n"
                   "  -o <file>     output assembly file (default: out.asm)\n"
                   "  -O0/-O1/-O2   optimization level\n"
                   "  -dump-ast     print AST to stderr\n"
                   "  -dump-ir      print IR to stderr\n"
                   "  -dump-toks    dump token stream and exit\n"
                   "  -v            verbose progress\n"
                   "  --version     print compiler version\n"
                   "  --help        print this message\n");
            return 0;
        } else if (a[0] != '-') {
            input_file = a;
        } else {
            fprintf(stderr, "acc: unknown option '%s' (--help for usage)\n", a);
            return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "acc: no input file\n");
        return 1;
    }

    if (!output_file) {
        /* Derive output name from input: foo.ac → foo.asm */
        usize len = strlen(input_file);
        char *out = malloc(len + 5);
        memcpy(out, input_file, len);
        /* Strip .ac extension if present */
        if (len > 3 && strcmp(out + len - 3, ".ac") == 0) out[len - 3] = '\0';
        strcat(out, ".asm");
        output_file = out;
    }

    /* --- Read source file --- */
    usize src_len = 0;
    char *src = read_file(input_file, &src_len);
    if (!src) return 1;

    if (verbose)
        fprintf(stderr, "acc: compiling '%s' (%zu bytes)\n", input_file, src_len);

    /* --- Create shared arena --- */
    Arena *arena = arena_new();

    /* --- Lexer --- */
    Lexer *lexer = lexer_new(src, src_len, input_file, arena);

    if (dump_toks) {
        Token t;
        do {
            t = lexer_next(lexer);
            tok_dump(&t, stdout);
        } while (t.kind != TOK_EOF);
        arena_free(arena);
        free(src);
        return 0;
    }

    /* --- Type context --- */
    TypeCtx *types = typectx_new(arena);

    /* --- Parse --- */
    if (verbose) fprintf(stderr, "acc: parsing...\n");
    AstNode *program = parse_program(lexer, arena, types);

    if (g_had_error) {
        fprintf(stderr, "acc: %d error(s), compilation aborted\n", g_error_count);
        arena_free(arena); free(src);
        return 1;
    }

    if (dump_ast) {
        fprintf(stderr, "=== AST ===\n");
        ast_dump(program, 0, stderr);
    }

    /* --- Semantic analysis --- */
    if (verbose) fprintf(stderr, "acc: semantic analysis...\n");
    SemaCtx *sema = sema_new(arena, types);
    if (!sema_analyze(sema, program)) {
        fprintf(stderr, "acc: %d error(s), compilation aborted\n", g_error_count);
        arena_free(arena); free(src);
        return 1;
    }

    /* --- IR generation --- */
    if (verbose) fprintf(stderr, "acc: lowering to IR...\n");
    IrModule *ir_mod = ir_module_new(arena, types);
    IrBuilder *builder = ir_builder_new(ir_mod, arena, types);
    IrGenCtx *irgen_ctx = irgen_new(builder, arena, types);
    irgen_program(irgen_ctx, program);

    /* Copy string literals from sema context into IR module */
    for (usize i = 0; i < sema->str_lits->len; i++)
        vec_push(ir_mod->str_lits, vec_at(sema->str_lits, i));

    if (dump_ir) {
        fprintf(stderr, "=== IR (pre-optimization) ===\n");
        ir_dump_module(ir_mod, stderr);
    }

    /* --- Optimization --- */
    if (opt_level > 0) {
        if (verbose) fprintf(stderr, "acc: running optimizations (level %d)...\n", opt_level);
        int n_changes = opt_run_all(ir_mod);
        if (verbose) fprintf(stderr, "acc: %d optimization(s) applied\n", n_changes);
        if (opt_level >= 2) {
            /* Run twice for O2 */
            n_changes += opt_run_all(ir_mod);
            if (verbose) fprintf(stderr, "acc: %d total optimization(s)\n", n_changes);
        }
    }

    if (dump_ir && opt_level > 0) {
        fprintf(stderr, "=== IR (post-optimization) ===\n");
        ir_dump_module(ir_mod, stderr);
    }

    /* --- Code generation --- */
    if (verbose) fprintf(stderr, "acc: generating x86-64 assembly...\n");
    CodegenCtx *cg = codegen_new(ir_mod, arena);
    char *asm_out = codegen_emit(cg);

    if (g_had_error) {
        fprintf(stderr, "acc: %d error(s) during code generation\n", g_error_count);
        free(asm_out);
        arena_free(arena);
        free(src);
        return 1;
    }

    /* --- Write output --- */
    FILE *outf = fopen(output_file, "w");
    if (!outf) {
        fprintf(stderr, "acc: cannot write to '%s': ", output_file);
        perror("");
        free(asm_out);
        arena_free(arena);
        free(src);
        return 1;
    }
    fputs(asm_out, outf);
    fclose(outf);
    free(asm_out);

    if (verbose || true) {
        fprintf(stderr, "acc: output written to '%s'\n", output_file);
        if (g_warn_count > 0)
            fprintf(stderr, "acc: %d warning(s)\n", g_warn_count);
    }

    /* --- Cleanup --- */
    arena_free(arena);
    free(src);
    return 0;
}
