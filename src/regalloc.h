// Linear scan register allocation (Poletto & Sarkar, TOPLAS 1999).
//
// Each vreg gets one live interval [start, end] over a linear numbering of
// the MIR instructions (blocks in reverse postorder). Intervals are visited
// by start; a vreg whose interval ended is expired and its register freed.
// When registers run out, the interval that ends furthest away is spilled to
// a stack slot for its whole lifetime.
//
// The same code serves both backends:
//   VM:  ~250 virtual registers, never spills, params pinned to r0..rN.
//   JIT: 9 x86-64 registers. Intervals that live across a call may only use
//        callee-saved registers (rbx, r12-r15); the rest prefer the
//        caller-saved ones (rsi, rdi, r8, r9) so the prologue saves less.
#pragma once
#include <string>
#include <vector>
#include "mir.h"

struct RAConfig {
  std::vector<int> order;          // allocatable registers, in preference order
  std::vector<bool> callee_saved;  // indexed by register number
  bool vm = false;                 // pin params to registers 0..n-1, no spilling
  std::vector<int> arg_regs;       // JIT: prefer these for call arguments / params
};

struct RAResult {
  std::vector<int> reg;    // per vreg: register, or -1
  std::vector<int> slot;   // per vreg: spill slot, or -1
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

// Aggressive copy coalescing (Chaitin style). For each `mov d, s`, if d and
// s never hold different values at the same time (they do not interfere),
// rename them to one vreg and delete the move. Returns the number of moves
// removed. Run before linear_scan.
int coalesce_moves(MFunc& f);
std::string print_intervals(const MFunc& f, const RAResult& r);
