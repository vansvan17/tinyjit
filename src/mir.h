// MIR: the machine-level IR both backends consume.
//
// It is what SSA becomes after two things happen:
//   1. Instruction selection. Constant operands that fit in 32 bits are folded
//      into the instruction (add r, imm), and a compare that feeds only a
//      branch is fused into it (BrCmp). Both backends then have fewer values
//      to keep in registers.
//   2. Out-of-SSA. Critical edges are split, then each phi p = phi(a, b)
//      becomes a copy `p = a` at the end of the first predecessor and `p = b`
//      at the end of the second. The copies on one edge happen "at once" (a
//      parallel copy), so they are ordered to avoid overwriting a value that
//      another copy still reads, with a temp to break cycles (the "swap
//      problem"). coalesce_moves() then merges most copies away.
//
// After this, a vreg may have several definitions, so MIR is not SSA.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "ir.h"

enum class MOp : uint8_t {
  Param, Const, Mov,
  Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne,
  Neg, Not, Call, Print, Cons, Car, Cdr, IsPair, IsNil,
  Jmp, Br, BrCmp, Ret,
};

const char* mop_name(MOp o);
inline bool mop_is_cmp(MOp o) { return o >= MOp::Lt && o <= MOp::Ne; }
inline bool mop_is_ordering(MOp o) { return o >= MOp::Lt && o <= MOp::Ge; }
MOp invert_cmp(MOp cc);  // !(a < b) == (a >= b), etc.
MOp swap_cmp(MOp cc);    // (a < b) == (b > a), etc.

struct MIns {
  MOp op;
  MOp cc = MOp::Eq;  // BrCmp condition
  int dst = -1;
  int a = -1;
  int b = -1;
  bool b_imm = false;  // b is the tagged constant in `imm`
  int64_t imm = 0;     // Const value, Param index, Call callee, or b immediate
  bool a_int = false;  // operand known to be an int: backends may skip the guard
  bool b_int = false;
  std::vector<int> args;
  int t = -1, f = -1;  // successor block indices
  int pos = 0;         // linear position, set by the register allocator
};

struct MBlock {
  std::vector<MIns> ins;
  std::vector<int> preds, succs;
};

struct MFunc {
  std::string name;
  int index = 0;
  int nparams = 0;
  int nvregs = 0;
  std::vector<MBlock> blocks;  // reverse postorder, entry first
  std::vector<int> param_vreg; // -1 if the parameter is never used
  bool has_cons = false;       // allocates: the JIT refuses these (no GC maps)
  int max_args = 0;
  std::set<int> callees;
};

// Splits critical edges in `f` (the only change it makes to the SSA).
MFunc lower_to_mir(Function& f, bool fuse_and_imm);
std::string print_mfunc(const MFunc& f);

template <class F>
void for_each_use(const MIns& m, F&& fn) {
  if (m.a >= 0) fn(m.a);
  if (m.b >= 0 && !m.b_imm) fn(m.b);
  for (int x : m.args) fn(x);
}
