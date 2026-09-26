#include "ir.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include "runtime.h"
#include "value.h"

const char* op_name(Op o) {
  static const char* n[] = {"const", "param", "add", "sub", "mul", "div", "mod", "lt",
                            "le", "gt", "ge", "eq", "ne", "neg", "not", "phi", "call",
                            "print", "cons", "car", "cdr", "is_pair", "is_nil",
                            "jmp", "br", "ret"};
  return n[(int)o];
}

std::vector<Block*> Block::succs() const {
  Inst* t = term();
  if (!t) return {};
  if (t->op == Op::Jmp) return {t->t};
  if (t->op == Op::Br) return {t->t, t->f};
  return {};
}

int Block::num_phis() const {
  int n = 0;
  while (n < (int)insts.size() && insts[n]->op == Op::Phi) n++;
  return n;
}

Inst* Function::make(Op op) {
  ipool.push_back(std::make_unique<Inst>());
  Inst* i = ipool.back().get();
  i->id = next_inst++;
  i->op = op;
  return i;
}

Block* Function::new_block() {
  bpool.push_back(std::make_unique<Block>());
  Block* b = bpool.back().get();
  b->id = (int)bpool.size() - 1;
  blocks.push_back(b);
  return b;
}

size_t Function::size() const {
  size_t n = 0;
  for (Block* b : blocks)
    for (Inst* i : b->insts)
      if (i->op != Op::Phi && !is_term(i->op)) n++;
  return n;
}

void replace_all_uses(Function& f, Inst* from, Inst* to) {
  for (Block* b : f.blocks)
    for (Inst* i : b->insts)
      for (Inst*& op : i->ops)
        if (op == from) op = to;
}

void remove_inst(Inst* i) {
  auto& v = i->bb->insts;
  v.erase(std::find(v.begin(), v.end(), i));
  i->removed = true;
}

void remove_pred(Block* b, int idx) {
  b->preds.erase(b->preds.begin() + idx);
  for (Inst* i : b->insts) {
    if (i->op != Op::Phi) break;
    i->ops.erase(i->ops.begin() + idx);
  }
}

int pred_index(const Block* b, const Block* p) {
  for (size_t i = 0; i < b->preds.size(); i++)
    if (b->preds[i] == p) return (int)i;
  return -1;
}

void insert_at(Block* b, size_t idx, Inst* i) {
  i->bb = b;
  b->insts.insert(b->insts.begin() + idx, i);
}

static std::string val_str(Value v) {
  if (is_int(v)) return std::to_string(as_int(v));
  if (v == NIL) return "nil";
  return "<ptr>";
}

std::string print_function(const Function& f) {
  std::ostringstream o;
  o << "fn " << f.name << "(" << f.nparams << " params):\n";
  for (Block* b : f.blocks) {
    o << "  b" << b->id << ":";
    if (!b->preds.empty()) {
      o << "  ; preds";
      for (Block* p : b->preds) o << " b" << p->id;
    }
    o << "\n";
    for (Inst* i : b->insts) {
      o << "    ";
      if (!is_term(i->op) && i->op != Op::Print) o << "%" << i->id << " = ";
      o << op_name(i->op);
      if (i->op == Op::Const) o << " " << val_str((Value)i->k);
      if (i->op == Op::Param) o << " " << i->k;
      if (i->op == Op::Call) o << " @" << i->k;
      for (size_t j = 0; j < i->ops.size(); j++) {
        o << (j ? ", " : " ") << "%" << i->ops[j]->id;
        if (i->op == Op::Phi) o << " [b" << b->preds[j]->id << "]";
      }
      if (i->op == Op::Jmp) o << " b" << i->t->id;
      if (i->op == Op::Br) o << ", b" << i->t->id << ", b" << i->f->id;
      if (i->known_int && !is_term(i->op)) o << "   ; int";
      o << "\n";
    }
  }
  return o.str();
}

std::string print_module(const Module& m) {
  std::string s;
  for (auto& f : m.fns) {
    s += "; @" + std::to_string(f->index) + "\n" + print_function(*f) + "\n";
  }
  return s;
}

void verify(const Function& f) {
  auto fail = [&](const std::string& msg) {
    fprintf(stderr, "IR verification failed in %s: %s\n%s", f.name.c_str(), msg.c_str(),
            print_function(f).c_str());
    abort();
  };
  std::set<const Block*> live(f.blocks.begin(), f.blocks.end());
  std::set<const Inst*> defined;
  for (Block* b : f.blocks)
    for (Inst* i : b->insts) defined.insert(i);
  if (!f.blocks.empty() && !f.entry()->preds.empty()) fail("entry block has predecessors");
  for (Block* b : f.blocks) {
    if (b->insts.empty() || !is_term(b->term()->op)) fail("b" + std::to_string(b->id) + " has no terminator");
    bool phis = true;
    for (size_t k = 0; k < b->insts.size(); k++) {
      Inst* i = b->insts[k];
      if (i->bb != b) fail("%" + std::to_string(i->id) + " has wrong parent block");
      if (is_term(i->op) && k + 1 != b->insts.size()) fail("terminator in middle of block");
      if (i->op == Op::Phi) {
        if (!phis) fail("phi after non-phi");
        if (i->ops.size() != b->preds.size()) fail("phi %" + std::to_string(i->id) + " operand count mismatch");
      } else {
        phis = false;
      }
      for (Inst* op : i->ops)
        if (!defined.count(op)) fail("%" + std::to_string(i->id) + " uses removed value %" + std::to_string(op->id));
    }
    for (Block* s : b->succs()) {
      if (!live.count(s)) fail("successor not in function");
      if (pred_index(s, b) < 0) fail("b" + std::to_string(s->id) + " missing pred b" + std::to_string(b->id));
    }
    for (Block* p : b->preds) {
      if (!live.count(p)) fail("b" + std::to_string(b->id) + " has dead pred b" + std::to_string(p->id));
      auto ss = p->succs();
      if (std::find(ss.begin(), ss.end(), b) == ss.end()) fail("pred edge without succ edge");
    }
  }
}
