# AC Compiler

A complete, multi-pass optimizing compiler for the **AC** systems-programming language, targeting **x86-64 Linux** (ELF64 / System V AMD64 ABI). Written in portable C11 with zero external dependencies beyond libc.

---

## Language Overview

AC is a statically-typed, expression-oriented language designed for low-level systems programming. It compiles directly to NASM assembly with no intermediate runtime other than a thin libc-compatible runtime shim.

### Type System

| Type | Width | Description |
|------|-------|-------------|
| `i8`…`i64` | 8–64 bit | Signed integers |
| `u8`…`u64` | 8–64 bit | Unsigned integers |
| `f32`, `f64` | 32, 64 bit | IEEE 754 floating-point |
| `bool` | 8 bit | Boolean (`true`/`false`) |
| `char` | 8 bit | Alias for `u8`, used for text |
| `void` | — | No value |
| `*T` | 64 bit | Pointer to T |
| `[N]T` | N×sizeof(T) | Fixed-size array |
| `struct S { … }` | computed | Aggregate value type |

### Syntax Showcase

```ac
// extern declarations for libc interop
extern fn printf(fmt: *char, ...) -> i32;
extern fn malloc(size: u64) -> *void;

// Struct definition with field access
struct Vec2 {
    x: f64,
    y: f64,
}

fn dot(a: *Vec2, b: *Vec2) -> f64 {
    return a->x * b->x + a->y * b->y;
}

// Recursive function
fn factorial(n: i64) -> i64 {
    if (n <= 1) { return 1; }
    return n * factorial(n - 1);
}

// Iterative with for loop
fn sum_to(n: i64) -> i64 {
    var acc: i64 = 0;
    for (let i: i64 = 0; i <= n; i = i + 1) {
        acc += i;
    }
    return acc;
}

fn main() -> i32 {
    // Type inference with 'let' (immutable) and 'var' (mutable)
    let msg: *char = "Hello, AC!\n";
    printf(msg);

    var v: Vec2;
    v.x = 3.0;
    v.y = 4.0;

    // Cast and sizeof
    let n: i32 = sizeof(Vec2) as i32;
    printf("sizeof(Vec2) = %d\n", n);

    // Ternary expression
    let result: i64 = (sum_to(100) > 5000) ? 1 : 0;
    printf("sum_to(100) > 5000: %lld\n", result);

    return 0;
}
```

### Operator Precedence (high → low)

| Level | Operators |
|-------|-----------|
| Postfix | `f()` `a[i]` `.field` `->field` `++` `--` |
| Prefix | `!` `~` `-` `&` `*` `++` `--` |
| Cast | `as` |
| Multiplicative | `*` `/` `%` |
| Additive | `+` `-` |
| Shift | `<<` `>>` |
| Comparison | `<` `>` `<=` `>=` |
| Equality | `==` `!=` |
| Bitwise AND | `&` |
| Bitwise XOR | `^` |
| Bitwise OR | `\|` |
| Logical AND | `&&` (short-circuit) |
| Logical OR | `\|\|` (short-circuit) |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |

---

## Compiler Architecture

```
Source (.ac)
    │
    ▼
┌─────────┐    Tokens     ┌──────────┐    AST      ┌──────────────┐
│  Lexer  │ ────────────▶ │  Parser  │ ──────────▶ │   Semantic   │
└─────────┘               └──────────┘             │   Analysis   │
                                                    └──────┬───────┘
                                                           │ annotated AST
                                                    ┌──────▼───────┐
                                                    │   IR Gen     │
                                                    │  (TAC/SSA)   │
                                                    └──────┬───────┘
                                                           │ IrModule
                                                    ┌──────▼───────┐
                                                    │  Optimizer   │
                                                    └──────┬───────┘
                                                           │ optimized IR
                                                    ┌──────▼───────┐
                                                    │   Codegen    │
                                                    │  (x86-64)    │
                                                    └──────┬───────┘
                                                           │
                                                    Output (.asm)
```

### Pass Descriptions

#### 1. Lexer (`src/lexer.c`)
- Single-pass, O(n) tokenizer with O(1) multi-character look-ahead (4-token ring buffer)
- Supports: integer literals (decimal, `0x` hex, `0b` binary, `0o` octal), float literals with exponents, character/string literals with full escape-sequence processing, `_`-separated digit groups
- Tracks precise source locations (file, line, column) for every token

#### 2. Parser (`src/parser.c`)
- Recursive-descent parser for declarations and statements
- **Pratt / precedence-climbing** algorithm for expressions — O(n) expression parsing with correct associativity and no ambiguity
- Error recovery: on syntax error, synchronizes to next statement boundary to enable multiple errors per compilation

#### 3. Semantic Analysis (`src/semantic.c`)
- **Four-pass design**:
  1. Struct collection — register all struct types and compute field layouts
  2. Function/global signature collection — build the symbol table entry with full type
  3. Global initializer checking
  4. Function body type checking — fully recursive
- Type inference for `let`/`var` bindings (bidirectional: annotation → infer)
- Usual Arithmetic Conversions (C11 §6.3.1.8) for binary operators
- Detects: undeclared identifiers, type mismatches, mutation of `let` bindings, `break`/`continue` outside loops, return-type mismatches, wrong argument counts
- Constant folds `sizeof(T)` to an integer at this pass

#### 4. IR (`src/ir.h`, `src/ir.c`)
Three-Address Code (TAC) intermediate representation:
- **Virtual registers** (VRegs): typed, unlimited, uniquely numbered per function
- **Typed immediate operands**: `i64`, `f64`, string labels, globals
- **Explicit control flow**: `IR_JMP`, `IR_JMPIF`, `IR_JMPIFNOT`, `IR_LABEL_DEF`
- **Phi-node placeholder** (`IR_PHI`) reserved for a future full-SSA conversion
- Doubly-linked instruction list per basic block; O(1) insertion/deletion
- Short-circuit evaluation of `&&`/`||` via conditional jumps

#### 5. IR Generation (`src/irgen.c`)
- Walk annotated AST, emit IR instructions through `IrBuilder`
- Each expression produces an `IOp` (immediate or VReg)
- Lvalue nodes produce a **pointer VReg**; callers emit `IR_LOAD`/`IR_STORE` through it
- All local variables are `IR_ALLOCA`-allocated stack objects (like LLVM's `alloca`)
- Parameters receive dedicated stack slots; incoming register values are stored at function entry

#### 6. Optimizer (`src/optimizer.c`)
Iterates passes to fixed-point:

| Pass | Description |
|------|-------------|
| **Constant Folding** | Folds `IMM op IMM → IMM` for all arithmetic, comparison, and bitwise ops |
| **Copy Propagation** | Replaces VReg uses where `t1 = t2` (MOV from register) with the source |
| **Dead Code Elimination** | Removes instructions with no live result and no side effects |
| **Strength Reduction** | `x*2^n → x<<n`, `x/2^n → x>>n`, `x*0→0`, `x+0→x`, etc. |
| **Peephole** | Eliminates `MOV t, t` identity moves and `NOP` instructions |

#### 7. Code Generator (`src/codegen.c`)
- Target: **x86-64 NASM** syntax, ELF64 format
- **System V AMD64 ABI** calling convention:
  - Integer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` (first 6); rest on stack
  - Float arguments: `xmm0`–`xmm7`
  - Return value: `rax` (integer), `xmm0` (float)
  - Stack 16-byte aligned at call sites
- **Stack-slot model**: every VReg assigned to `[rbp - 8*(id+1)]`; correct for any number of vregs
- Arithmetic: direct x86-64 integer instructions (`add`, `sub`, `imul`, `idiv`, `cqo`)
- Floating-point: SSE2 `movsd`/`addsd`/`mulsd`/`cvtsi2sd` etc.
- Comparisons: `cmp` + `setcc` → zero-extended to 64 bits
- Structs: `lea` + byte-offset addition for field addresses

---

## Building the Compiler

### Prerequisites
- **GCC ≥ 9** (or clang ≥ 10)
- GNU Make

```bash
git clone <repo>
cd A_C_Grade_Compiler
make
```

The compiler binary `acc` is created in the project root.

### Compiler Options

```
Usage: acc <source.ac> [options]

  -o <file>      Output assembly file (default: <source>.asm)
  -O0            Disable all optimizations
  -O1            Standard optimizations (default)
  -O2            Run optimization passes twice
  -dump-ast      Print AST to stderr after parsing
  -dump-ir       Print IR to stderr before and after optimization
  -dump-toks     Dump token stream to stdout and exit
  -v             Verbose progress messages
  --version      Print compiler version
  --help         Print usage
```

---

## Compiling an AC Program

```bash
# 1. Compile AC source to NASM assembly
./acc examples/fibonacci.ac -o fibonacci.asm

# 2. Assemble with NASM (Linux ELF64)
nasm -felf64 fibonacci.asm -o fibonacci.o

# 3. Build runtime and link
gcc -c runtime/runtime.c -o runtime.o
gcc fibonacci.o runtime.o -o fibonacci -lm

# 4. Run
./fibonacci
```

Or use the provided Makefile targets:
```bash
make run-fibonacci
make run-hello
make run-primes
```

---

## Examples

| File | Demonstrates |
|------|-------------|
| `examples/hello.ac` | `extern fn`, `printf`, basic program structure |
| `examples/fibonacci.ac` | Recursion, while loops, local variables, type casting |
| `examples/primes.ac` | Nested loops, boolean returns, compound conditions |
| `examples/structs.ac` | Struct declaration, field access (`.` and `->`), pointer-to-struct |

---

## Grammar (Formal)

```
program     := decl*

decl        := fn_decl | struct_decl | global_let | extern_fn

fn_decl     := 'fn' IDENT '(' params ')' ['->' type] block
extern_fn   := 'extern' 'fn' IDENT '(' params [',' '...'] ')' ['->' type] ';'
struct_decl := 'struct' IDENT '{' (IDENT ':' type ',')* '}'
global_let  := 'let' IDENT [':' type] '=' expr ';'

params      := ε | param (',' param)*
param       := IDENT ':' type

type        := '*' type
             | '[' INT_LIT ']' type
             | 'void' | 'bool' | 'char'
             | 'i8' | 'i16' | 'i32' | 'i64'
             | 'u8' | 'u16' | 'u32' | 'u64'
             | 'f32' | 'f64'
             | IDENT

block       := '{' stmt* '}'

stmt        := 'let' IDENT [':' type] '=' expr ';'
             | 'var' IDENT [':' type] ['=' expr] ';'
             | 'if' '(' expr ')' block ['else' (block | stmt)]
             | 'while' '(' expr ')' block
             | 'for' '(' stmt expr ';' expr ')' block
             | 'return' [expr] ';'
             | 'break' ';'
             | 'continue' ';'
             | block
             | expr ';'

expr        := IDENT | INT_LIT | FLOAT_LIT | BOOL_LIT | CHAR_LIT | STR_LIT | 'null'
             | '(' expr ')'
             | expr '(' [expr (',' expr)*] ')'   -- call
             | expr '[' expr ']'                  -- index
             | expr '.' IDENT                     -- field
             | expr '->' IDENT                    -- pointer member
             | expr ('++' | '--')                 -- postfix
             | ('++' | '--') expr                 -- prefix
             | ('!' | '-' | '~') expr             -- unary
             | '&' expr                            -- address-of
             | '*' expr                            -- dereference
             | expr 'as' type                      -- cast
             | 'sizeof' '(' type ')'
             | expr binop expr
             | expr '?' expr ':' expr              -- ternary

binop       := '+' | '-' | '*' | '/' | '%'
             | '&' | '|' | '^' | '<<' | '>>'
             | '&&' | '||'
             | '==' | '!=' | '<' | '>' | '<=' | '>='
             | '=' | '+=' | '-=' | '*=' | '/=' | '%='
             | '&=' | '|=' | '^=' | '<<=' | '>>='
```

---

## Project Structure

```
A_C_Grade_Compiler/
├── include/          Header files (interface definitions)
│   ├── common.h      Arena allocator, hash map, vector, string builder, diagnostics
│   ├── lexer.h       Token types and lexer interface
│   ├── ast.h         AST node definitions and constructors
│   ├── types.h       Type system (primitives, pointers, arrays, structs, fns)
│   ├── symbol.h      Scoped symbol table
│   ├── semantic.h    Semantic analysis context
│   ├── ir.h          TAC IR definitions and builder API
│   ├── irgen.h       AST → IR lowering
│   ├── optimizer.h   Optimization pass declarations
│   └── codegen.h     x86-64 NASM code generation
│
├── src/              Implementation files
│   ├── common.c      Utilities (arena, vec, hmap, strbuf, diagnostics)
│   ├── lexer.c       Tokenizer
│   ├── types.c       Type registry and predicates
│   ├── symbol.c      Symbol table
│   ├── ast.c         AST node constructors and pretty-printer
│   ├── parser.c      Recursive-descent + Pratt parser
│   ├── semantic.c    Type checker and name resolver
│   ├── ir.c          IR module builder and dump
│   ├── irgen.c       AST → TAC IR lowering
│   ├── optimizer.c   Constant folding, DCE, copy-prop, strength reduction
│   ├── codegen.c     x86-64 NASM code emitter
│   └── main.c        CLI driver and pipeline orchestration
│
├── runtime/
│   └── runtime.c     AC runtime library (I/O, math, memory helpers)
│
├── examples/
│   ├── hello.ac      Hello World
│   ├── fibonacci.ac  Recursive + iterative Fibonacci
│   ├── primes.ac     Sieve / prime counting
│   └── structs.ac    Struct definitions and pointer member access
│
├── lexer.c           Original lexer (preserved; superseded by src/lexer.c)
├── Makefile
└── README.md
```

---

## Design Decisions

### Why Three-Address Code?
TAC is the minimal representation that exposes data flow without encoding register constraints. This makes the optimization passes simple and compositional. The structure is also a direct precursor to SSA form — adding φ-nodes and the dominance-frontier algorithm would convert it fully.

### Why Stack-Slot Variables?
The `alloca`-for-every-variable strategy (borrowed from LLVM's `-O0` codegen) simplifies correctness dramatically: every variable has exactly one canonical storage location. The optimizer's copy-propagation pass eliminates most of the resulting redundant loads.

### Why Linear Scan Register Allocation?
Graph coloring produces near-optimal allocations but is NP-hard in the general case and complex to implement correctly. Linear scan (Poletto & Sarkar 1999) operates in O(n log n) on sorted live intervals and produces code within 10–15% of optimal on typical workloads — the same algorithm used by HotSpot JIT and early LLVM.

### Why NASM?
NASM has unambiguous, explicit syntax (no AT&T `%reg` prefix confusion), excellent documentation, and produces standard ELF objects compatible with any GNU linker. The 64-bit `default rel` addressing mode avoids GOT trampolines for small programs.

---

## License

GNU Affero General Public License v3 (AGPL-3.0). See `LICENSE` for details.
