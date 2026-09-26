// AST -> SSA in one pass, following Braun, Buchwald, Hack, Leissa, Mallon,
// Zwinkau, "Simple and Efficient Construction of Static Single Assignment
// Form" (CC 2013).
//
// The idea: keep, per variable, the value it holds at the end of each block
// (write_var). A read in a block with no local definition walks up to the
// predecessors. A block is "sealed" once all its predecessors are known; reads
// in unsealed blocks (loop headers) create an operandless phi that gets filled
// in at seal time. Phis whose operands are all the same value (or the phi
// itself) are trivial and are removed on the spot, which is also where the
// copy propagation happens: an assignment `x = y` never creates an
// instruction, it just maps x to y's value.
#include <unordered_map>
#include "ast.h"
#include "ir.h"
#include "lexer.h"
#include "value.h"

namespace {

struct Builtin { Op op; int arity; };
const std::unordered_map<std::string, Builtin> kBuiltins = {
    {"print", {Op::Print, 1}}, {"cons", {Op::Cons, 2}},     {"car", {Op::Car, 1}},
    {"cdr", {Op::Cdr, 1}},     {"is_pair", {Op::IsPair, 1}}, {"is_nil", {Op::IsNil, 1}},
};

struct Sig { int index; int arity; };

class Builder {
 public:
  Builder(Function* f, const std::unordered_map<std::string, Sig>& sigs) : F(f), sigs(sigs) {}

  void build(const FnDecl& d) {
    cur = F->new_block();
    cur->sealed = true;
    scopes.emplace_back();
    for (size_t i = 0; i < d.params.size(); i++) {
      if (scopes.back().count(d.params[i])) compile_error(d.line, "duplicate parameter " + d.params[i]);
      int v = new_var();
      scopes.back()[d.params[i]] = v;
      write_var(v, cur, emit(Op::Param, {}, (int64_t)i));
    }
    stmts(d.body);
    ret(konst(mk_int(0)));
    F->blocks.pop_back();  // the empty block ret() opened after the final return
  }

 private:
  Function* F;
  const std::unordered_map<std::string, Sig>& sigs;
  Block* cur = nullptr;
  std::vector<std::unordered_map<Block*, Inst*>> defs;  // per variable
  std::unordered_map<Block*, std::vector<std::pair<int, Inst*>>> incomplete;
  std::vector<std::unordered_map<std::string, int>> scopes;
  // A phi can be found trivial and removed while a pointer to it is still
  // held in a C++ local (e.g. the first argument of a call whose later
  // arguments trigger the removal). Removed phis forward to their
  // replacement so such pointers can be fixed up when they are finally used.
  std::unordered_map<Inst*, Inst*> forward;

  Inst* resolve(Inst* v) {
    while (v && v->removed) {
      auto it = forward.find(v);
      if (it == forward.end()) break;
      v = it->second;
    }
    return v;
  }

  // ---- instruction helpers ----
  Inst* emit(Op op, std::vector<Inst*> ops = {}, int64_t k = 0) {
    Inst* i = F->make(op);
    for (Inst*& o : ops) o = resolve(o);
    i->ops = std::move(ops);
    i->k = k;
    i->bb = cur;
    cur->insts.push_back(i);
    return i;
  }
  Inst* konst(Value v) { return emit(Op::Const, {}, (int64_t)v); }
  void jmp(Block* t) {
    Inst* i = emit(Op::Jmp);
    i->t = t;
    t->preds.push_back(cur);
  }
  void br(Inst* c, Block* t, Block* f) {
    Inst* i = emit(Op::Br, {c});
    i->t = t;
    i->f = f;
    t->preds.push_back(cur);
    f->preds.push_back(cur);
  }
  void ret(Inst* v) {
    emit(Op::Ret, {v});
    // Anything after a return is unreachable. Keep lowering into a fresh
    // block with no predecessors; simplify_cfg deletes it later.
    cur = F->new_block();
    cur->sealed = true;
  }
  Inst* undef() {
    // Reading a variable on a path where it has no definition (only possible
    // in unreachable code). Materialize 0 at the top of the entry block,
    // which dominates everything.
    Inst* c = F->make(Op::Const);
    c->k = (int64_t)mk_int(0);
    insert_at(F->entry(), 0, c);
    return c;
  }

  // ---- Braun et al. ----
  int new_var() { defs.emplace_back(); return (int)defs.size() - 1; }
  void write_var(int v, Block* b, Inst* val) { defs[v][b] = resolve(val); }

  Inst* read_var(int v, Block* b) {
    auto it = defs[v].find(b);
    if (it != defs[v].end()) return resolve(it->second);
    return read_var_recursive(v, b);
  }

  Inst* new_phi(Block* b) {
    Inst* p = F->make(Op::Phi);
    insert_at(b, b->num_phis(), p);
    return p;
  }

  Inst* read_var_recursive(int v, Block* b) {
    Inst* val;
    if (!b->sealed) {
      val = new_phi(b);
      incomplete[b].push_back({v, val});
    } else if (b->preds.empty()) {
      val = undef();
    } else if (b->preds.size() == 1) {
      val = read_var(v, b->preds[0]);
    } else {
      // Write the phi first to break cycles through loops.
      val = new_phi(b);
      write_var(v, b, val);
      val = add_phi_operands(v, val);
    }
    write_var(v, b, val);
    return val;
  }

  Inst* add_phi_operands(int v, Inst* phi) {
    // Copy preds: reading may not add edges, but be safe against aliasing.
    std::vector<Block*> preds = phi->bb->preds;
    for (Block* p : preds) phi->ops.push_back(read_var(v, p));
    for (Inst*& o : phi->ops) o = resolve(o);
    return try_remove_trivial_phi(phi);
  }

  Inst* try_remove_trivial_phi(Inst* phi) {
    if (phi->removed) return phi;
    // A phi that is still being filled in cannot be judged yet.
    if (phi->ops.size() != phi->bb->preds.size()) return phi;
    Inst* same = nullptr;
    for (Inst* op : phi->ops) {
      if (op == same || op == phi) continue;
      if (same) return phi;  // merges at least two values: not trivial
      same = op;
    }
    if (!same) same = undef();
    std::vector<Inst*> users;
    for (Block* b : F->blocks)
      for (Inst* i : b->insts)
        if (i != phi && i->op == Op::Phi)
          for (Inst* op : i->ops)
            if (op == phi) { users.push_back(i); break; }
    replace_all_uses(*F, phi, same);
    for (auto& m : defs)
      for (auto& [blk, val] : m)
        if (val == phi) val = same;
    remove_inst(phi);
    forward[phi] = same;
    for (Inst* u : users) try_remove_trivial_phi(u);
    return resolve(same);
  }

  void seal(Block* b) {
    auto pending = std::move(incomplete[b]);
    incomplete.erase(b);
    for (auto& [v, phi] : pending) add_phi_operands(v, phi);
    b->sealed = true;
  }

  // ---- scopes ----
  int lookup(const std::string& name, int line) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    compile_error(line, "undefined variable " + name);
  }

  // ---- statements ----
  void stmts(const std::vector<std::unique_ptr<Stmt>>& ss) {
    scopes.emplace_back();
    for (auto& s : ss) stmt(*s);
    scopes.pop_back();
  }

  void stmt(const Stmt& s) {
    switch (s.kind) {
      case SK::Let: {
        Inst* v = expr(*s.e);  // evaluate before the name is in scope
        int var = new_var();
        scopes.back()[s.name] = var;
        write_var(var, cur, v);
        break;
      }
      case SK::Assign: {
        int var = lookup(s.name, s.line);
        write_var(var, cur, expr(*s.e));
        break;
      }
      case SK::ExprS: expr(*s.e); break;
      case SK::Block: stmts(s.body); break;
      case SK::Return: ret(s.e ? expr(*s.e) : konst(mk_int(0))); break;
      case SK::If: {
        Inst* c = expr(*s.e);
        Block* then_b = F->new_block();
        Block* else_b = s.els.empty() ? nullptr : F->new_block();
        Block* join = F->new_block();
        br(c, then_b, else_b ? else_b : join);
        seal(then_b);
        cur = then_b;
        stmts(s.body);
        jmp(join);
        if (else_b) {
          seal(else_b);
          cur = else_b;
          stmts(s.els);
          jmp(join);
        }
        seal(join);
        cur = join;
        break;
      }
      case SK::While: {
        Block* header = F->new_block();
        jmp(header);
        cur = header;  // not sealed: the back edge is not known yet
        Inst* c = expr(*s.e);
        Block* body = F->new_block();
        Block* exit = F->new_block();
        br(c, body, exit);
        seal(body);
        cur = body;
        stmts(s.body);
        jmp(header);
        seal(header);
        seal(exit);
        cur = exit;
        break;
      }
    }
  }

  // ---- expressions ----
  Inst* expr(const Expr& e) {
    switch (e.kind) {
      case EK::Int: return konst(mk_int(e.ival));
      case EK::Nil: return konst(NIL);
      case EK::Var: return read_var(lookup(e.name, e.line), cur);
      case EK::Unary: {
        Inst* a = expr(*e.kids[0]);
        return emit(e.uop == '-' ? Op::Neg : Op::Not, {a});
      }
      case EK::Binary: {
        Inst* a = expr(*e.kids[0]);
        Inst* b = expr(*e.kids[1]);
        return emit((Op)((int)Op::Add + (int)e.bop), {a, b});
      }
      case EK::And: case EK::Or: {
        // Short circuit through a temporary variable, so the join value is
        // just another phi built by read_var. Result is normalized to 0/1.
        bool is_and = e.kind == EK::And;
        int tmp = new_var();
        Inst* a = expr(*e.kids[0]);
        write_var(tmp, cur, konst(mk_int(is_and ? 0 : 1)));
        Block* rhs = F->new_block();
        Block* join = F->new_block();
        if (is_and) br(a, rhs, join);
        else br(a, join, rhs);
        seal(rhs);
        cur = rhs;
        Inst* b = expr(*e.kids[1]);
        write_var(tmp, cur, emit(Op::Ne, {b, konst(mk_int(0))}));
        jmp(join);
        seal(join);
        cur = join;
        return read_var(tmp, cur);
      }
      case EK::Call: {
        std::vector<Inst*> args;
        for (auto& k : e.kids) args.push_back(expr(*k));
        auto b = kBuiltins.find(e.name);
        if (b != kBuiltins.end()) {
          if ((int)args.size() != b->second.arity)
            compile_error(e.line, e.name + " takes " + std::to_string(b->second.arity) + " argument(s)");
          Inst* i = emit(b->second.op, args);
          if (b->second.op == Op::Print) return konst(mk_int(0));
          return i;
        }
        auto s = sigs.find(e.name);
        if (s == sigs.end()) compile_error(e.line, "undefined function " + e.name);
        if ((int)args.size() != s->second.arity)
          compile_error(e.line, e.name + " takes " + std::to_string(s->second.arity) + " argument(s)");
        return emit(Op::Call, args, s->second.index);
      }
    }
    return nullptr;
  }
};

}  // namespace

Module build_ssa(const Program& p) {
  Module m;
  std::unordered_map<std::string, Sig> sigs;
  for (size_t i = 0; i < p.fns.size(); i++) {
    auto& d = p.fns[i];
    if (sigs.count(d.name)) compile_error(d.line, "duplicate function " + d.name);
    if (kBuiltins.count(d.name)) compile_error(d.line, d.name + " is a builtin");
    sigs[d.name] = {(int)i, (int)d.params.size()};
  }
  auto mainf = sigs.find("main");
  if (mainf == sigs.end()) compile_error(1, "no main function");
  if (mainf->second.arity != 0) compile_error(1, "main takes no arguments");
  for (size_t i = 0; i < p.fns.size(); i++) {
    auto f = std::make_unique<Function>();
    f->name = p.fns[i].name;
    f->index = (int)i;
    f->nparams = (int)p.fns[i].params.size();
    Builder(f.get(), sigs).build(p.fns[i]);
    m.index[f->name] = (int)i;
    m.fns.push_back(std::move(f));
  }
  return m;
}
