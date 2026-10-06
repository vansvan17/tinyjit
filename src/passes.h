#pragma once
#include <unordered_map>
#include "ir.h"

struct OptStats {
  int folded = 0;
  int dce_removed = 0;
  int blocks_removed = 0;
  int inlined = 0;
};

void compute_known_int(Function& f);

void prove_int_operands(Function& f);

std::unordered_map<const Block*, Block*> dominators(const Function& f);

bool fold_constants(Function& f, OptStats& st);
bool simplify_cfg(Function& f, OptStats& st);
bool dce(Function& f, OptStats& st);

void optimize_function(Function& f, int level, OptStats& st);

void inline_module(Module& m, int threshold, OptStats& st);
