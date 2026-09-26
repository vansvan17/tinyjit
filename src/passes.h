#pragma once
#include <unordered_map>
#include "ir.h"

struct OptStats {
  int folded = 0;
  int dce_removed = 0;
  int blocks_removed = 0;
  int inlined = 0;
};

// Marks values that are provably tagged ints. Used by DCE (int arithmetic
// cannot trap) and by the backends (the JIT drops type guards).
void compute_known_int(Function& f);

// Guard elimination. An instruction like `x + 1` or `x < n` traps unless x is
// an int, so once it has executed, x is known to be an int in every block it
// dominates. Walk the dominator tree carrying that set and record, per
// instruction, which operands are proven ints (Inst::int_ops). The backends
// skip the type guard for those operands.
void prove_int_operands(Function& f);

// Immediate dominators (Cooper, Harvey, Kennedy, "A Simple, Fast Dominance
// Algorithm", 2001). Entry maps to itself.
std::unordered_map<const Block*, Block*> dominators(const Function& f);

bool fold_constants(Function& f, OptStats& st);
bool simplify_cfg(Function& f, OptStats& st);
bool dce(Function& f, OptStats& st);

void optimize_function(Function& f, int level, OptStats& st);

// Inline calls to callees whose size is <= threshold instructions.
// Functions are visited callees-first so inlined bodies are already optimized.
void inline_module(Module& m, int threshold, OptStats& st);
