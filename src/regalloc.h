#pragma once
#include <string>
#include <vector>
#include "mir.h"

struct RAConfig {
  std::vector<int> order;
  std::vector<bool> callee_saved;
  bool vm = false;
  std::vector<int> arg_regs;
};

struct RAResult {
  std::vector<int> reg;
  std::vector<int> slot;
  std::vector<int> start, end;
  int nslots = 0;
  int max_reg = -1;
  std::vector<int> used_callee_saved;
  int spilled = 0;
  int moves_coalesced = 0;
  bool ok = true;
  std::string error;
};

RAResult linear_scan(MFunc& f, const RAConfig& cfg);

int coalesce_moves(MFunc& f);
std::string print_intervals(const MFunc& f, const RAResult& r);
