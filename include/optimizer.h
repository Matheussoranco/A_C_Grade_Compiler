/*
 * optimizer.h — IR optimization passes.
 *
 * Passes are applied in order to the IrModule before code generation.
 * Each pass is idempotent and can be run multiple times; the pipeline
 * iterates until no changes are made (fixed-point).
 *
 * Implemented passes:
 *   - Constant folding     (fold compile-time-known arithmetic)
 *   - Copy propagation     (replace uses of copies with their sources)
 *   - Dead code elimination (remove instructions with no live result)
 *   - Strength reduction   (x*2 → x<<1, x/power-of-2 → x>>n, etc.)
 *   - Peephole             (local pattern matching on 2–3 instruction windows)
 */
#pragma once
#include "common.h"
#include "ir.h"

/* Run all optimization passes until fixed-point; returns total changes made. */
int opt_run_all(IrModule *m);

/* Individual passes — each returns the number of changes made. */
int opt_const_fold(IrFunc *f);
int opt_copy_prop(IrFunc *f);
int opt_dce(IrFunc *f);
int opt_strength_reduce(IrFunc *f);
int opt_peephole(IrFunc *f);
