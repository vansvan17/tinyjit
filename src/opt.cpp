// Optimization passes over SSA.
//
// Because every value has exactly one definition, most passes are simple
// worklist-free sweeps: fold an instruction whose operands are constants,
// delete an instruction nobody uses, delete a block nobody reaches. Copy
// propagation does not appear as a separate pass: the SSA builder never
// emits copies (see ssa_builder.cpp), and a phi whose inputs are all the same
// value is replaced by that value here and in the builder.
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_map>
#include <unordered_set>
#include "passes.h"
#include "value.h"

namespace {

void morph_const(Inst* i, Value v) {
  i->op = Op::Const;
  i->ops.clear();
  i->k = (int64_t)v;
}

bool is_const(const Inst* i) { return i->op == Op::Const; }

}  // namespace

void compute_known_int(Function& f) {
  std::vector<Inst*> phis;
  for (Block* b : f.blocks)
    for (Inst* i : b->insts) {
      switch (i->op) {
        case Op::Const: i->known_int = is_int((Value)i->k); break;
        case Op::Param: case Op::Call: case Op::Car: case Op::Cdr: case Op::Cons:
          i->known_int = false;
          break;
        case Op::Phi: i->known_int = true; phis.push_back(i); break;  // optimistic
        default: i->known_int = !is_term(i->op); break;  // arithmetic, compares, predicates
      }
    }
  // Iterate to the greatest fixpoint: a phi is int if all of its inputs are.
  bool changed = true;
  while (changed) {
    changed = false;
    for (Inst* p : phis) {
      if (!p->known_int) continue;
      for (Inst* op : p->ops)
        if (!op->known_int) { p->known_int = false; changed = true; break; }
    }
  }
}

static std::vector<Block*> reverse_postorder(const Function& f) {
  std::vector<Block*> post;
  std::unordered_set<const Block*> seen{f.entry()};
  std::vector<std::pair<Block*, size_t>> stack{{f.entry(), 0}};
  while (!stack.empty()) {
    auto& [b, k] = stack.back();
    auto ss = b->succs();
    if (k < ss.size()) {
      Block* s = ss[k++];
      if (seen.insert(s).second) stack.push_back({s, 0});
    } else {
      post.push_back(b);
      stack.pop_back();
    }
  }
  return {post.rbegin(), post.rend()};
}

std::unordered_map<const Block*, Block*> dominators(const Function& f) {
  auto rpo = reverse_postorder(f);
  std::unordered_map<const Block*, int> num;
  for (size_t i = 0; i < rpo.size(); i++) num[rpo[i]] = (int)i;
  std::vector<int> idom(rpo.size(), -1);
  idom[0] = 0;
  auto intersect = [&](int a, int b) {
    while (a != b) {
      while (a > b) a = idom[a];
      while (b > a) b = idom[b];
    }
    return a;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < rpo.size(); i++) {
      int nd = -1;
      for (Block* p : rpo[i]->preds) {
        auto it = num.find(p);
        if (it == num.end() || idom[it->second] < 0) continue;  // unreachable or not yet processed
        nd = nd < 0 ? it->second : intersect(it->second, nd);
      }
      if (nd != idom[i]) {
        idom[i] = nd;
        changed = true;
      }
    }
  }
  std::unordered_map<const Block*, Block*> out;
  for (size_t i = 0; i < rpo.size(); i++) out[rpo[i]] = rpo[idom[i]];
  return out;
}

static bool traps_unless_int(Op op) {
  switch (op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Neg:
      return true;
    default:
      return false;
  }
}

void prove_int_operands(Function& f) {
  auto idom = dominators(f);
  std::unordered_map<const Block*, std::vector<Block*>> kids;
  for (Block* b : f.blocks)
    if (b != f.entry() && idom.count(b)) kids[idom[b]].push_back(b);
  std::unordered_set<const Inst*> proven;
  std::function<void(Block*)> visit = [&](Block* b) {
    std::vector<const Inst*> added;
    for (Inst* i : b->insts) {
      i->int_ops = 0;
      for (size_t k = 0; k < i->ops.size() && k < 8; k++)
        if (i->ops[k]->known_int || proven.count(i->ops[k])) i->int_ops |= (uint8_t)(1u << k);
      // Phi operands flow in from predecessors, not from this point.
      if (i->op == Op::Phi) i->int_ops = 0;
      if (traps_unless_int(i->op))
        for (Inst* o : i->ops)
          if (proven.insert(o).second) added.push_back(o);
    }
    for (Block* c : kids[b]) visit(c);
    for (const Inst* o : added) proven.erase(o);
  };
  visit(f.entry());
}

bool fold_constants(Function& f, OptStats& st) {
  bool changed = false;
  for (Block* b : f.blocks) {
    for (Inst* i : b->insts) {
      if (i->op == Op::Phi || i->op == Op::Const) continue;
      Value out;
      if (is_binop(i->op)) {
        if ((i->op == Op::Eq || i->op == Op::Ne) && i->ops[0] == i->ops[1]) {
          morph_const(i, mk_int(i->op == Op::Eq));
        } else if (is_const(i->ops[0]) && is_const(i->ops[1])) {
          BinOp bo = (BinOp)((int)i->op - (int)Op::Add);
          // Leave trapping operations alone so the error still happens at run time.
          if (eval_binop(bo, (Value)i->ops[0]->k, (Value)i->ops[1]->k, out) != Trap::None) continue;
          morph_const(i, out);
        } else {
          continue;
        }
      } else if (i->op == Op::Neg && is_const(i->ops[0])) {
        if (eval_neg((Value)i->ops[0]->k, out) != Trap::None) continue;
        morph_const(i, out);
      } else if (i->op == Op::Not && is_const(i->ops[0])) {
        morph_const(i, eval_not((Value)i->ops[0]->k));
      } else if (i->op == Op::IsNil && is_const(i->ops[0])) {
        morph_const(i, mk_int((Value)i->ops[0]->k == NIL));
      } else if (i->op == Op::IsPair && is_const(i->ops[0])) {
        morph_const(i, mk_int(0));  // constants are never pairs
      } else {
        continue;
      }
      st.folded++;
      changed = true;
    }
    // A phi whose inputs are equal constants becomes that constant. It has to
    // move below the phi group, since non-phis cannot precede phis.
    for (int k = 0; k < b->num_phis();) {
      Inst* p = b->insts[k];
      bool ok = !p->ops.empty();
      int64_t v = 0;
      bool have = false;
      for (Inst* op : p->ops) {
        if (op == p) continue;
        if (!is_const(op) || (have && op->k != v)) { ok = false; break; }
        v = op->k;
        have = true;
      }
      if (ok && have) {
        b->insts.erase(b->insts.begin() + k);
        morph_const(p, (Value)v);
        b->insts.insert(b->insts.begin() + b->num_phis(), p);
        st.folded++;
        changed = true;
      } else {
        k++;
      }
    }
  }
  return changed;
}

static bool remove_unreachable(Function& f, OptStats& st) {
  std::unordered_set<Block*> seen;
  std::vector<Block*> stack{f.entry()};
  seen.insert(f.entry());
  while (!stack.empty()) {
    Block* b = stack.back();
    stack.pop_back();
    for (Block* s : b->succs())
      if (seen.insert(s).second) stack.push_back(s);
  }
  if (seen.size() == f.blocks.size()) return false;
  for (Block* b : f.blocks) {
    if (seen.count(b)) continue;
    for (Block* s : b->succs()) {
      int idx;
      while ((idx = pred_index(s, b)) >= 0) remove_pred(s, idx);
    }
    b->removed = true;
    for (Inst* i : b->insts) i->removed = true;
    st.blocks_removed++;
  }
  f.blocks.erase(std::remove_if(f.blocks.begin(), f.blocks.end(), [](Block* b) { return b->removed; }),
                 f.blocks.end());
  return true;
}

bool simplify_cfg(Function& f, OptStats& st) {
  bool any = false;
  for (bool changed = true; changed;) {
    changed = false;
    // 1. Branches on constants and branches whose arms agree become jumps.
    for (Block* b : f.blocks) {
      Inst* t = b->term();
      if (t->op != Op::Br) continue;
      Block* keep;
      Block* drop;
      if (t->t == t->f) {
        keep = t->t;
        drop = t->t;  // one of the two identical edges goes away
      } else if (is_const(t->ops[0])) {
        bool taken = t->ops[0]->k != 0;
        keep = taken ? t->t : t->f;
        drop = taken ? t->f : t->t;
      } else {
        continue;
      }
      int idx = pred_index(drop, b);
      if (keep == drop) {
        // Remove the second occurrence so the first keeps its phi operands.
        for (size_t j = idx + 1; j < drop->preds.size(); j++)
          if (drop->preds[j] == b) { idx = (int)j; break; }
      }
      remove_pred(drop, idx);
      t->op = Op::Jmp;
      t->ops.clear();
      t->t = keep;
      t->f = nullptr;
      changed = true;
    }
    // 2. Unreachable blocks.
    if (remove_unreachable(f, st)) changed = true;
    // 3. Trivial phis.
    for (Block* b : f.blocks) {
      for (int k = 0; k < b->num_phis();) {
        Inst* p = b->insts[k];
        Inst* same = nullptr;
        bool trivial = true;
        for (Inst* op : p->ops) {
          if (op == p || op == same) continue;
          if (same) { trivial = false; break; }
          same = op;
        }
        if (trivial && same) {
          replace_all_uses(f, p, same);
          remove_inst(p);
          changed = true;
        } else {
          k++;
        }
      }
    }
    // 4. Merge a block into its only predecessor when that predecessor
    //    jumps straight to it.
    for (size_t bi = 0; bi < f.blocks.size(); bi++) {
      Block* b = f.blocks[bi];
      if (b->removed) continue;
      Inst* t = b->term();
      if (t->op != Op::Jmp) continue;
      Block* c = t->t;
      if (c == b || c == f.entry() || c->preds.size() != 1) continue;
      while (c->num_phis() > 0) {
        Inst* p = c->insts[0];
        replace_all_uses(f, p, p->ops[0]);
        remove_inst(p);
      }
      remove_inst(t);
      for (Inst* i : c->insts) {
        i->bb = b;
        b->insts.push_back(i);
      }
      c->insts.clear();
      for (Block* s : b->succs())
        for (Block*& p : s->preds)
          if (p == c) p = b;
      c->removed = true;
      st.blocks_removed++;
      changed = true;
      bi--;  // b may now be able to absorb its new successor too
    }
    f.blocks.erase(std::remove_if(f.blocks.begin(), f.blocks.end(), [](Block* b) { return b->removed; }),
                   f.blocks.end());
    any |= changed;
  }
  return any;
}

static bool has_side_effect(const Inst* i) {
  auto both_int = [&] { return i->ops[0]->known_int && i->ops[1]->known_int; };
  switch (i->op) {
    case Op::Call: case Op::Print: case Op::Jmp: case Op::Br: case Op::Ret:
    case Op::Car: case Op::Cdr:
      return true;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
      return !both_int();  // may raise a type error
    case Op::Neg:
      return !i->ops[0]->known_int;
    case Op::Div: case Op::Mod:
      return !(i->ops[0]->known_int && is_const(i->ops[1]) && is_int((Value)i->ops[1]->k) &&
               i->ops[1]->k != 0);
    default:
      return false;  // const, param, phi, eq, ne, not, is_pair, is_nil, cons
  }
}

bool dce(Function& f, OptStats& st) {
  compute_known_int(f);
  std::unordered_set<Inst*> live;
  std::vector<Inst*> work;
  for (Block* b : f.blocks)
    for (Inst* i : b->insts)
      if (has_side_effect(i) && live.insert(i).second) work.push_back(i);
  while (!work.empty()) {
    Inst* i = work.back();
    work.pop_back();
    for (Inst* op : i->ops)
      if (live.insert(op).second) work.push_back(op);
  }
  bool changed = false;
  for (Block* b : f.blocks) {
    auto& v = b->insts;
    size_t before = v.size();
    for (Inst* i : v)
      if (!live.count(i)) i->removed = true;
    v.erase(std::remove_if(v.begin(), v.end(), [](Inst* i) { return i->removed; }), v.end());
    if (v.size() != before) {
      st.dce_removed += (int)(before - v.size());
      changed = true;
    }
  }
  return changed;
}

void optimize_function(Function& f, int level, OptStats& st) {
  if (level <= 0) {
    // Even at -O0, drop code after `return` so the backends never see it.
    remove_unreachable(f, st);
    verify(f);
    return;
  }
  for (int round = 0; round < 10; round++) {
    bool changed = false;
    changed |= fold_constants(f, st);
    changed |= simplify_cfg(f, st);
    changed |= dce(f, st);
    verify(f);
    if (!changed) break;
  }
  compute_known_int(f);
}

// ---------------------------------------------------------------- inlining

namespace {

// Replace `call` (in caller F) with a copy of G's body.
//
//   B: ... %r = call G(a, b) ...rest      B: ...  jmp G.entry'
//                                  =>     G.entry' ... G's blocks, params -> a, b
//                                         each `ret v` -> jmp Cont
//                                         Cont: %r = phi(v...)  ...rest
void inline_call(Function& F, Inst* call, const Function& G) {
  Block* B = call->bb;
  size_t at = std::find(B->insts.begin(), B->insts.end(), call) - B->insts.begin();

  Block* cont = F.new_block();
  cont->sealed = true;
  for (size_t k = at + 1; k < B->insts.size(); k++) {
    B->insts[k]->bb = cont;
    cont->insts.push_back(B->insts[k]);
  }
  B->insts.resize(at);  // drops the call too; it is replaced below
  for (Block* s : cont->succs())
    for (Block*& p : s->preds)
      if (p == B) p = cont;

  std::unordered_map<const Block*, Block*> bm;
  std::unordered_map<const Inst*, Inst*> im;
  for (Block* gb : G.blocks) bm[gb] = F.new_block();
  for (Block* gb : G.blocks)
    for (Inst* gi : gb->insts) {
      Inst* ni = F.make(gi->op);
      ni->k = gi->k;
      ni->bb = bm[gb];
      bm[gb]->insts.push_back(ni);
      im[gi] = ni;
    }
  std::vector<std::pair<Block*, Inst*>> rets;
  std::vector<Inst*> params;
  for (Block* gb : G.blocks) {
    Block* nb = bm[gb];
    for (Block* p : gb->preds) nb->preds.push_back(bm[p]);
    for (Inst* gi : gb->insts) {
      Inst* ni = im[gi];
      for (Inst* op : gi->ops) ni->ops.push_back(im[op]);
      if (gi->t) ni->t = bm[gi->t];
      if (gi->f) ni->f = bm[gi->f];
      if (ni->op == Op::Param) params.push_back(ni);
      if (ni->op == Op::Ret) {
        rets.push_back({nb, ni->ops[0]});
        ni->op = Op::Jmp;
        ni->ops.clear();
        ni->t = cont;
        cont->preds.push_back(nb);
      }
    }
  }
  for (Inst* p : params) {
    replace_all_uses(F, p, call->ops[p->k]);
    for (auto& r : rets)
      if (r.second == p) r.second = call->ops[p->k];
    remove_inst(p);
  }
  Block* gentry = bm[G.entry()];
  gentry->preds.push_back(B);
  Inst* j = F.make(Op::Jmp);
  j->t = gentry;
  j->bb = B;
  B->insts.push_back(j);

  Inst* result;
  if (rets.size() == 1) {
    result = rets[0].second;
  } else if (rets.empty()) {
    result = F.make(Op::Const);  // callee never returns; cont is unreachable
    result->k = (int64_t)mk_int(0);
    insert_at(cont, 0, result);
  } else {
    result = F.make(Op::Phi);
    for (auto& r : rets) result->ops.push_back(r.second);
    insert_at(cont, 0, result);
  }
  call->removed = true;
  replace_all_uses(F, call, result);

  // new_block() appended the clones and cont at the end. Move them right
  // after B so dumps read top to bottom: B, callee body, cont, rest.
  std::vector<Block*> moved;
  for (Block* gb : G.blocks) moved.push_back(bm[gb]);
  moved.push_back(cont);
  std::unordered_set<Block*> mv(moved.begin(), moved.end());
  F.blocks.erase(std::remove_if(F.blocks.begin(), F.blocks.end(), [&](Block* b) { return mv.count(b) > 0; }),
                 F.blocks.end());
  auto pos = std::find(F.blocks.begin(), F.blocks.end(), B) + 1;
  F.blocks.insert(pos, moved.begin(), moved.end());
}

}  // namespace

void inline_module(Module& m, int threshold, OptStats& st) {
  if (threshold <= 0) return;
  // Callees first (postorder over the call graph).
  std::vector<int> order;
  std::vector<int> state(m.fns.size(), 0);
  std::function<void(int)> visit = [&](int fi) {
    state[fi] = 1;
    for (Block* b : m.fns[fi]->blocks)
      for (Inst* i : b->insts)
        if (i->op == Op::Call && state[i->k] == 0) visit((int)i->k);
    order.push_back(fi);
  };
  for (size_t i = 0; i < m.fns.size(); i++)
    if (!state[i]) visit((int)i);

  const size_t kMaxCallerSize = 20000;
  for (int fi : order) {
    Function& F = *m.fns[fi];
    std::vector<Inst*> calls;
    for (Block* b : F.blocks)
      for (Inst* i : b->insts)
        if (i->op == Op::Call && i->k != fi) calls.push_back(i);
    bool did = false;
    for (Inst* c : calls) {
      const Function& G = *m.fns[c->k];
      if ((int)G.size() > threshold || F.size() + G.size() > kMaxCallerSize) continue;
      inline_call(F, c, G);
      st.inlined++;
      did = true;
    }
    if (did) {
      verify(F);
      optimize_function(F, 1, st);
    }
  }
}
