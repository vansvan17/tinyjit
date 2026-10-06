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
MOp invert_cmp(MOp cc);
MOp swap_cmp(MOp cc);

struct MIns {
  MOp op;
  MOp cc = MOp::Eq;
  int dst = -1;
  int a = -1;
  int b = -1;
  bool b_imm = false;
  int64_t imm = 0;
  bool a_int = false;
  bool b_int = false;
  std::vector<int> args;
  int t = -1, f = -1;
  int pos = 0;
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
  std::vector<MBlock> blocks;
  std::vector<int> param_vreg;
  bool has_cons = false;
  int max_args = 0;
  std::set<int> callees;
};

MFunc lower_to_mir(Function& f, bool fuse_and_imm);
std::string print_mfunc(const MFunc& f);

template <class F>
void for_each_use(const MIns& m, F&& fn) {
  if (m.a >= 0) fn(m.a);
  if (m.b >= 0 && !m.b_imm) fn(m.b);
  for (int x : m.args) fn(x);
}
