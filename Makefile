# Makefile — AC Compiler (acc)
#
# Targets:
#   make            — build the compiler (./acc)
#   make runtime    — build the runtime library (runtime/runtime.o)
#   make examples   — compile all .ac examples to .asm (requires acc in PATH)
#   make clean      — remove build artifacts
#   make test       — run smoke tests
#
# Requirements: GCC ≥ 9, NASM ≥ 2.14 (for example assembly)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter \
            -Isrc -Iinclude -g -O2 \
            -D_GNU_SOURCE
LDFLAGS := -lm

TARGET  := acc

SRCS := src/common.c   \
        src/lexer.c     \
        src/types.c     \
        src/symbol.c    \
        src/ast.c       \
        src/parser.c    \
        src/semantic.c  \
        src/ir.c        \
        src/irgen.c     \
        src/optimizer.c \
        src/codegen.c   \
        src/main.c

OBJS := $(SRCS:.c=.o)

# ── Main compiler ──────────────────────────────────────────────────────────
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Runtime library ────────────────────────────────────────────────────────
.PHONY: runtime
runtime: runtime/runtime.o

runtime/runtime.o: runtime/runtime.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Example compilation ────────────────────────────────────────────────────
EXAMPLES := examples/hello.ac examples/fibonacci.ac examples/primes.ac

.PHONY: examples
examples: $(TARGET) runtime/runtime.o
	@for f in $(EXAMPLES); do \
	    base=$$(basename $$f .ac); \
	    echo "Compiling $$f → build/$$base.asm"; \
	    mkdir -p build; \
	    ./$(TARGET) $$f -o build/$$base.asm -O1 || true; \
	done

# Assemble and link a specific example (Linux only, requires nasm):
# make run-hello
.PHONY: run-hello
run-hello: $(TARGET) runtime/runtime.o
	mkdir -p build
	./$(TARGET) examples/hello.ac -o build/hello.asm -O1
	nasm -felf64 build/hello.asm -o build/hello.o
	$(CC) build/hello.o runtime/runtime.o -o build/hello $(LDFLAGS)
	./build/hello

.PHONY: run-fibonacci
run-fibonacci: $(TARGET) runtime/runtime.o
	mkdir -p build
	./$(TARGET) examples/fibonacci.ac -o build/fibonacci.asm -O1
	nasm -felf64 build/fibonacci.asm -o build/fibonacci.o
	$(CC) build/fibonacci.o runtime/runtime.o -o build/fibonacci $(LDFLAGS)
	./build/fibonacci

.PHONY: run-primes
run-primes: $(TARGET) runtime/runtime.o
	mkdir -p build
	./$(TARGET) examples/primes.ac -o build/primes.asm -O1
	nasm -felf64 build/primes.asm -o build/primes.o
	$(CC) build/primes.o runtime/runtime.o -o build/primes $(LDFLAGS)
	./build/primes

# ── Debugging targets ──────────────────────────────────────────────────────
.PHONY: dump-ast
dump-ast: $(TARGET)
	./$(TARGET) examples/fibonacci.ac -dump-ast -O0 2>&1 | head -60

.PHONY: dump-ir
dump-ir: $(TARGET)
	./$(TARGET) examples/fibonacci.ac -dump-ir -O0 2>&1 | head -80

.PHONY: dump-toks
dump-toks: $(TARGET)
	./$(TARGET) examples/hello.ac -dump-toks 2>&1 | head -40

# ── Tests ───────────────────────────────────────────────────────────────────
.PHONY: test
test: $(TARGET)
	@echo "=== Running smoke tests ==="
	@echo "--- Token dump of hello.ac ---"
	./$(TARGET) examples/hello.ac -dump-toks | head -10
	@echo "--- AST dump of fibonacci.ac ---"
	./$(TARGET) examples/fibonacci.ac -dump-ast 2>&1 | head -20
	@echo "--- Compiling all examples ---"
	@for f in $(EXAMPLES); do \
	    base=$$(basename $$f .ac); \
	    mkdir -p build; \
	    ./$(TARGET) $$f -o build/$$base.asm -O1 && echo "  OK: $$f" || echo "  FAIL: $$f"; \
	done
	@echo "=== Tests complete ==="

# ── Clean ──────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET) runtime/runtime.o
	rm -rf build/
	@echo "Cleaned."

# ── Dependency tracking ────────────────────────────────────────────────────
-include $(OBJS:.o=.d)

src/%.d: src/%.c
	@$(CC) $(CFLAGS) -MM -MF $@ -MT $(@:.d=.o) $< 2>/dev/null || true
