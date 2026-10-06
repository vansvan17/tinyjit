#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

enum class Op : uint8_t {
  Const, Param,
  Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne,
  Neg, Not, Phi, Call, Print, Cons, Car, Cdr, IsPair, IsNil,
  Jmp, Br, Ret,
};

const char* op_name(Op o);
inline bool is_term(Op o) { return o == Op::Jmp || o == Op::Br || o == Op::Ret; }
inline bool is_binop(Op o) { return o >= Op::Add && o <= Op::Ne; }
inline bool is_cmp(Op o) { return o >= Op::Lt && o <= Op::Ne; }

struct Block;

struct Inst {
  int id = 0;
  Op op = Op::Const;
  Block* bb = nullptr;
  std::vector<Inst*> ops;
  int64_t k = 0;
  Block* t = nullptr;
  Block* f = nullptr;
  bool known_int = false;
  uint8_t int_ops = 0;
  bool removed = false;
};

struct Block {
  int id = 0;
  std::vector<Inst*> insts;
  std::vector<Block*> preds;
  bool sealed = false;
  bool removed = false;

  Inst* term() const { return insts.empty() ? nullptr : insts.back(); }
  std::vector<Block*> succs() const;
  int num_phis() const;
};

struct Function {
  std::string name;
  int index = 0;
  int nparams = 0;
  std::vector<std::unique_ptr<Inst>> ipool;
  std::vector<std::unique_ptr<Block>> bpool;
  std::vector<Block*> blocks;
  int next_inst = 0;

  Block* entry() const { return blocks[0]; }
  Inst* make(Op op);
  Block* new_block();
  size_t size() const;
};

struct Module {
  std::vector<std::unique_ptr<Function>> fns;
  std::unordered_map<std::string, int> index;
};

std::string print_function(const Function& f);
std::string print_module(const Module& m);
void verify(const Function& f);

void replace_all_uses(Function& f, Inst* from, Inst* to);
void remove_inst(Inst* i);
void remove_pred(Block* b, int idx);
int pred_index(const Block* b, const Block* p);
void insert_at(Block* b, size_t idx, Inst* i);

struct Program;
Module build_ssa(const Program& p);
