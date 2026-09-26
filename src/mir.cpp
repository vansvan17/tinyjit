#include "mir.h"
#include <algorithm>
#include <climits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "value.h"

const char* mop_name(MOp o) {
  static const char* n[] = {"param", "const", "mov", "add", "sub", "mul", "div", "mod",
                            "lt", "le", "gt", "ge", "eq", "ne", "neg", "not", "call",
                            "print", "cons", "car", "cdr", "is_pair", "is_nil",
                            "jmp", "br", "br_cmp", "ret"};
  return n[(int)o];
}

MOp invert_cmp(MOp cc) {
  switch (cc) {
    case MOp::Lt: return MOp::Ge;
    case MOp::Ge: return MOp::Lt;
    case MOp::Le: return MOp::Gt;
    case MOp::Gt: return MOp::Le;
    case MOp::Eq: return MOp::Ne;
    default: return MOp::Eq;
  }
}

MOp swap_cmp(MOp cc) {
  switch (cc) {
    case MOp::Lt: return MOp::Gt;
    case MOp::Gt: return MOp::Lt;
    case MOp::Le: return MOp::Ge;
    case MOp::Ge: return MOp::Le;
    default: return cc;
  }
}

static MOp to_mop(Op o) {
  switch (o) {
    case Op::Add: return MOp::Add;
    case Op::Sub: return MOp::Sub;
    case Op::Mul: return MOp::Mul;
    case Op::Div: return MOp::Div;
    case Op::Mod: return MOp::Mod;
    case Op::Lt: return MOp::Lt;
    case Op::Le: return MOp::Le;
    case Op::Gt: return MOp::Gt;
    case Op::Ge: return MOp::Ge;
    case Op::Eq: return MOp::Eq;
    case Op::Ne: return MOp::Ne;
    case Op::Neg: return MOp::Neg;
    case Op::Not: return MOp::Not;
    case Op::Car: return MOp::Car;
    case Op::Cdr: return MOp::Cdr;
    case Op::IsPair: return MOp::IsPair;
    case Op::IsNil: return MOp::IsNil;
    default: return MOp::Mov;
  }
}

// Is operand `v` of instruction `i` known to be an int at that point?
static bool int_operand(const Inst* i, const Inst* v) {
  if (v->known_int) return true;
  for (size_t k = 0; k < i->ops.size() && k < 8; k++)
    if (i->ops[k] == v && (i->int_ops >> k & 1)) return true;
  return false;
}

// An edge B->S is critical when B has several successors and S several
// predecessors. Phi copies for S have to run on exactly that edge, so such an
// edge gets its own block to hold them. (Leaving the copies at the end of B
// would run them on B's other edge too: the "lost copy" problem.)
static void split_critical_edges(Function& f) {
  std::vector<Block*> blocks = f.blocks;
  for (Block* b : blocks) {
    Inst* t = b->term();
    if (t->op != Op::Br || t->t == t->f) continue;
    for (Block** target : {&t->t, &t->f}) {
      Block* s = *target;
      // Split even when s has one predecessor: copies placed before b's branch
      // could otherwise clobber a value the branch itself reads.
      if (s->num_phis() == 0) continue;
      Block* e = f.new_block();
      e->sealed = true;
      e->preds.push_back(b);
      Inst* j = f.make(Op::Jmp);
      j->t = s;
      j->bb = e;
      e->insts.push_back(j);
      s->preds[pred_index(s, b)] = e;
      *target = e;
    }
  }
}

// Emit a parallel copy {dst_i <- src_i} as a sequence of moves. A move can go
// once no other pending move still needs to read its destination; when only
// cycles are left (a <- b, b <- a), one value is parked in a fresh vreg.
static void sequentialize(std::vector<std::pair<int, int>> copies, MFunc& m, std::vector<MIns>& out) {
  copies.erase(std::remove_if(copies.begin(), copies.end(), [](auto& c) { return c.first == c.second; }),
               copies.end());
  while (!copies.empty()) {
    bool progress = false;
    for (size_t i = 0; i < copies.size() && !progress; i++) {
      int d = copies[i].first;
      bool blocked = false;
      for (size_t j = 0; j < copies.size(); j++)
        if (j != i && copies[j].second == d) blocked = true;
      if (!blocked) {
        MIns x{MOp::Mov};
        x.dst = d;
        x.a = copies[i].second;
        out.push_back(x);
        copies.erase(copies.begin() + i);
        progress = true;
      }
    }
    if (!progress) {
      int d = copies[0].first;
      int t = m.nvregs++;
      MIns x{MOp::Mov};
      x.dst = t;
      x.a = d;
      out.push_back(x);
      for (auto& c : copies)
        if (c.second == d) c.second = t;
    }
  }
}

MFunc lower_to_mir(Function& f, bool fuse_and_imm) {
  split_critical_edges(f);
  MFunc m;
  m.name = f.name;
  m.index = f.index;
  m.nparams = f.nparams;
  m.param_vreg.assign(f.nparams, -1);

  // Reverse postorder: every block comes after its dominator, loop bodies
  // after their headers. Linear scan relies on this order being sensible.
  // Successors are visited last-first so that `br c, then, else` lays out
  // `then` right after the branch, and a loop body right after its header.
  std::vector<Block*> post;
  std::unordered_set<const Block*> seen{f.entry()};
  std::vector<std::pair<Block*, size_t>> stack{{f.entry(), 0}};
  while (!stack.empty()) {
    auto& [b, k] = stack.back();
    auto ss = b->succs();
    if (k < ss.size()) {
      Block* s = ss[ss.size() - 1 - k++];
      if (seen.insert(s).second) stack.push_back({s, 0});
    } else {
      post.push_back(b);
      stack.pop_back();
    }
  }
  std::vector<Block*> rpo(post.rbegin(), post.rend());
  std::unordered_map<const Block*, int> bidx;
  for (size_t i = 0; i < rpo.size(); i++) bidx[rpo[i]] = (int)i;

  std::unordered_map<const Inst*, int> uses;
  for (Block* b : rpo)
    for (Inst* i : b->insts)
      for (Inst* op : i->ops) uses[op]++;

  // Instruction selection decisions.
  struct Sel { const Inst* a; const Inst* b; bool imm; MOp op; };
  std::unordered_map<const Inst*, Sel> sel;
  std::unordered_map<const Inst*, int> absorbed;
  for (Block* b : rpo)
    for (Inst* i : b->insts) {
      if (!is_binop(i->op)) continue;
      MOp op = to_mop(i->op);
      const Inst* x = i->ops[0];
      const Inst* y = i->ops[1];
      bool divmod = op == MOp::Div || op == MOp::Mod;
      auto ok = [&](const Inst* c) {
        return fuse_and_imm && c->op == Op::Const && c->k >= INT32_MIN && c->k <= INT32_MAX &&
               (is_int((Value)c->k) || op == MOp::Eq || op == MOp::Ne) &&
               !(divmod && c->k == 0);  // x / 0 keeps its register form and traps at run time
      };
      bool imm = false;
      if (ok(y)) {
        imm = true;
      } else if (ok(x) && op != MOp::Sub && !divmod) {
        std::swap(x, y);
        op = swap_cmp(op);
        imm = true;
      }
      if (imm) absorbed[y]++;
      sel[i] = {x, y, imm, op};
    }

  std::unordered_set<const Inst*> fused;
  if (fuse_and_imm)
    for (Block* b : rpo) {
      Inst* t = b->term();
      if (t->op != Op::Br || b->insts.size() < 2) continue;
      Inst* c = t->ops[0];
      if (is_cmp(c->op) && c->bb == b && b->insts[b->insts.size() - 2] == c && uses[c] == 1) fused.insert(c);
    }

  std::unordered_map<const Inst*, int> vr;
  auto V = [&](const Inst* i) {
    auto it = vr.find(i);
    if (it != vr.end()) return it->second;
    return vr[i] = m.nvregs++;
  };

  m.blocks.resize(rpo.size());
  for (size_t bi = 0; bi < rpo.size(); bi++) {
    Block* b = rpo[bi];
    MBlock& mb = m.blocks[bi];
    auto push = [&](MIns x) { mb.ins.push_back(std::move(x)); };
    for (Inst* i : b->insts) {
      if (is_term(i->op)) break;
      MIns x{MOp::Mov};
      switch (i->op) {
        case Op::Phi:
          continue;  // defined by the copies at the end of each predecessor
        case Op::Const:
          if (uses[i] - absorbed[i] <= 0) continue;
          x.op = MOp::Const;
          x.dst = V(i);
          x.imm = i->k;
          break;
        case Op::Param:
          x.op = MOp::Param;
          x.dst = V(i);
          x.imm = i->k;
          m.param_vreg[i->k] = x.dst;
          break;
        case Op::Call:
          x.op = MOp::Call;
          x.dst = V(i);
          x.imm = i->k;
          for (Inst* a : i->ops) x.args.push_back(V(a));
          m.max_args = std::max(m.max_args, (int)i->ops.size());
          m.callees.insert((int)i->k);
          break;
        case Op::Print:
          x.op = MOp::Print;
          x.a = V(i->ops[0]);
          break;
        case Op::Cons:
          x.op = MOp::Cons;
          x.dst = V(i);
          x.a = V(i->ops[0]);
          x.b = V(i->ops[1]);
          m.has_cons = true;
          break;
        default:
          if (is_binop(i->op)) {
            if (fused.count(i)) continue;
            const Sel& s = sel[i];
            x.op = s.op;
            x.dst = V(i);
            x.a = V(s.a);
            x.a_int = int_operand(i, s.a);
            if (s.imm) {
              x.b_imm = true;
              x.imm = s.b->k;
              x.b_int = is_int((Value)s.b->k);
            } else {
              x.b = V(s.b);
              x.b_int = int_operand(i, s.b);
            }
          } else {  // unary
            x.op = to_mop(i->op);
            x.dst = V(i);
            x.a = V(i->ops[0]);
            x.a_int = int_operand(i, i->ops[0]);
          }
      }
      push(std::move(x));
    }
    // Phi copies for each successor. After critical edge splitting, a block
    // with phi copies to make has exactly one successor, so the copies can
    // sit at its end. They form one parallel copy per edge.
    for (Block* s : b->succs()) {
      if (s->num_phis() == 0) continue;
      int idx = pred_index(s, b);
      std::vector<std::pair<int, int>> copies;
      for (Inst* p : s->insts) {
        if (p->op != Op::Phi) break;
        copies.push_back({V(p), V(p->ops[idx])});
      }
      sequentialize(std::move(copies), m, mb.ins);
    }
    Inst* t = b->term();
    MIns x{MOp::Jmp};
    if (t->op == Op::Jmp) {
      x.t = bidx[t->t];
    } else if (t->op == Op::Br) {
      Inst* c = t->ops[0];
      x.t = bidx[t->t];
      x.f = bidx[t->f];
      if (fused.count(c)) {
        const Sel& s = sel[c];
        x.op = MOp::BrCmp;
        x.cc = s.op;
        x.a = V(s.a);
        x.a_int = int_operand(c, s.a);
        if (s.imm) {
          x.b_imm = true;
          x.imm = s.b->k;
          x.b_int = is_int((Value)s.b->k);
        } else {
          x.b = V(s.b);
          x.b_int = int_operand(c, s.b);
        }
      } else {
        x.op = MOp::Br;
        x.a = V(c);
      }
    } else {
      x.op = MOp::Ret;
      x.a = V(t->ops[0]);
    }
    push(std::move(x));
    for (Block* s : b->succs()) mb.succs.push_back(bidx[s]);
  }
  for (size_t bi = 0; bi < m.blocks.size(); bi++)
    for (int s : m.blocks[bi].succs) m.blocks[s].preds.push_back((int)bi);
  return m;
}

std::string print_mfunc(const MFunc& f) {
  std::ostringstream o;
  o << "mir " << f.name << " (" << f.nvregs << " vregs):\n";
  auto val = [](int64_t k) {
    Value v = (Value)k;
    return is_int(v) ? std::to_string(as_int(v)) : v == NIL ? std::string("nil") : std::string("?");
  };
  for (size_t bi = 0; bi < f.blocks.size(); bi++) {
    o << "  B" << bi << ":\n";
    for (auto& x : f.blocks[bi].ins) {
      o << "    ";
      if (x.dst >= 0) o << "v" << x.dst << " = ";
      o << (x.op == MOp::BrCmp ? std::string("br_") + mop_name(x.cc) : std::string(mop_name(x.op)));
      if (x.op == MOp::Const) o << " " << val(x.imm);
      if (x.op == MOp::Param) o << " " << x.imm;
      if (x.op == MOp::Call) o << " @" << x.imm;
      if (x.a >= 0) o << " v" << x.a << (x.a_int ? ":int" : "");
      if (x.b_imm) o << ", #" << val(x.imm);
      else if (x.b >= 0) o << ", v" << x.b << (x.b_int ? ":int" : "");
      for (int a : x.args) o << " v" << a;
      if (x.t >= 0) o << " -> B" << x.t;
      if (x.f >= 0) o << ", B" << x.f;
      o << "\n";
    }
  }
  return o.str();
}
