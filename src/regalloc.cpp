#include "regalloc.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <unordered_set>

namespace {

using Bits = std::vector<uint64_t>;
inline void bset(Bits& s, int v) { s[v >> 6] |= 1ull << (v & 63); }
inline bool bget(const Bits& s, int v) { return (s[v >> 6] >> (v & 63)) & 1; }

void liveness(const MFunc& f, std::vector<Bits>& in, std::vector<Bits>& out) {
  const int nb = (int)f.blocks.size();
  const int W = (f.nvregs + 63) / 64;
  std::vector<Bits> use(nb, Bits(W)), def(nb, Bits(W));
  in.assign(nb, Bits(W));
  out.assign(nb, Bits(W));
  for (int b = 0; b < nb; b++)
    for (auto& x : f.blocks[b].ins) {
      for_each_use(x, [&](int v) { if (!bget(def[b], v)) bset(use[b], v); });
      if (x.dst >= 0) bset(def[b], x.dst);
    }
  for (bool changed = true; changed;) {
    changed = false;
    for (int b = nb - 1; b >= 0; b--) {
      Bits o(W);
      for (int s : f.blocks[b].succs)
        for (int w = 0; w < W; w++) o[w] |= in[s][w];
      Bits i(W);
      for (int w = 0; w < W; w++) i[w] = use[b][w] | (o[w] & ~def[b][w]);
      if (i != in[b] || o != out[b]) {
        in[b] = std::move(i);
        out[b] = std::move(o);
        changed = true;
      }
    }
  }
}

}

int coalesce_moves(MFunc& f) {
  const int nv = f.nvregs;
  std::vector<Bits> in, out;
  liveness(f, in, out);

  std::vector<std::unordered_set<int>> adj(nv);
  auto edge = [&](int a, int b) {
    if (a == b) return;
    adj[a].insert(b);
    adj[b].insert(a);
  };
  std::vector<int> dense;
  std::vector<int> where(nv, -1);
  auto add = [&](int v) { if (where[v] < 0) { where[v] = (int)dense.size(); dense.push_back(v); } };
  auto del = [&](int v) {
    if (where[v] < 0) return;
    int last = dense.back();
    dense[where[v]] = last;
    where[last] = where[v];
    dense.pop_back();
    where[v] = -1;
  };
  for (size_t b = 0; b < f.blocks.size(); b++) {
    for (int v : dense) where[v] = -1;
    dense.clear();
    for (int v = 0; v < nv; v++)
      if (bget(out[b], v)) add(v);
    auto& ins = f.blocks[b].ins;
    for (int k = (int)ins.size() - 1; k >= 0; k--) {
      const MIns& x = ins[k];
      if (x.dst >= 0) {
        for (int l : dense)
          if (!(x.op == MOp::Mov && l == x.a)) edge(x.dst, l);
        del(x.dst);
      }
      for_each_use(x, add);
    }
  }
  for (int i = 0; i < f.nparams; i++)
    for (int j = i + 1; j < f.nparams; j++)
      if (f.param_vreg[i] >= 0 && f.param_vreg[j] >= 0) edge(f.param_vreg[i], f.param_vreg[j]);

  std::vector<int> parent(nv);
  std::iota(parent.begin(), parent.end(), 0);
  auto find = [&](int v) {
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  std::vector<char> is_param(nv, 0);
  for (int v : f.param_vreg)
    if (v >= 0) is_param[v] = 1;
  int merged = 0;
  for (auto& blk : f.blocks)
    for (auto& x : blk.ins) {
      if (x.op != MOp::Mov) continue;
      int a = find(x.dst), b = find(x.a);
      if (a == b || adj[a].count(b)) continue;
      if (is_param[a] && is_param[b]) continue;
      if (adj[a].size() < adj[b].size() || is_param[b]) std::swap(a, b);
      parent[b] = a;
      is_param[a] |= is_param[b];
      for (int n : adj[b]) {
        adj[n].erase(b);
        adj[n].insert(a);
        adj[a].insert(n);
      }
      adj[b].clear();
      merged++;
    }
  if (!merged) return 0;
  int removed = 0;
  for (auto& blk : f.blocks) {
    std::vector<MIns> keep;
    for (auto& x : blk.ins) {
      if (x.dst >= 0) x.dst = find(x.dst);
      if (x.a >= 0) x.a = find(x.a);
      if (x.b >= 0 && !x.b_imm) x.b = find(x.b);
      for (int& v : x.args) v = find(v);
      if (x.op == MOp::Mov && x.dst == x.a) { removed++; continue; }
      keep.push_back(x);
    }
    blk.ins = std::move(keep);
  }
  for (int& v : f.param_vreg)
    if (v >= 0) v = find(v);
  return removed;
}

RAResult linear_scan(MFunc& f, const RAConfig& cfg) {
  RAResult r;
  const int nv = f.nvregs;
  const int nb = (int)f.blocks.size();
  std::vector<int> bstart(nb), bend(nb);
  int idx = 0;
  for (int b = 0; b < nb; b++) {
    bstart[b] = 2 * idx;
    for (auto& x : f.blocks[b].ins) x.pos = 2 * idx++;
    bend[b] = 2 * (idx - 1) + 1;
  }

  std::vector<Bits> in, out;
  liveness(f, in, out);
  auto get = bget;

  r.start.assign(nv, INT_MAX);
  r.end.assign(nv, -1);
  std::vector<char> used(nv, 0);
  auto ext = [&](int v, int p) {
    r.start[v] = std::min(r.start[v], p);
    r.end[v] = std::max(r.end[v], p);
  };
  std::vector<int> hint(nv, -1);
  std::vector<int> calls;
  for (int b = 0; b < nb; b++) {
    for (int v = 0; v < nv; v++) {
      if (get(in[b], v)) ext(v, bstart[b]);
      if (get(out[b], v)) ext(v, bend[b]);
    }
    for (auto& x : f.blocks[b].ins) {
      for_each_use(x, [&](int v) { ext(v, x.pos); used[v] = 1; });
      if (x.dst >= 0) ext(x.dst, x.pos + 1);
      if (x.op == MOp::Mov && hint[x.dst] < 0) hint[x.dst] = x.a;
      if (x.op == MOp::Call || x.op == MOp::Print) calls.push_back(x.pos);
    }
  }
  std::vector<int> reg_hint(nv, -1);
  if (!cfg.arg_regs.empty()) {
    for (auto& b : f.blocks)
      for (auto& x : b.ins) {
        if (x.op == MOp::Call)
          for (size_t i = 0; i < x.args.size() && i < cfg.arg_regs.size(); i++)
            if (reg_hint[x.args[i]] < 0) reg_hint[x.args[i]] = cfg.arg_regs[i];
        if (x.op == MOp::Print && reg_hint[x.a] < 0) reg_hint[x.a] = cfg.arg_regs[0];
      }
    for (int i = 0; i < f.nparams && i < (int)cfg.arg_regs.size(); i++)
      if (f.param_vreg[i] >= 0) reg_hint[f.param_vreg[i]] = cfg.arg_regs[i];
  }
  std::vector<int> param_idx(nv, -1);
  for (int i = 0; i < f.nparams; i++) {
    int v = f.param_vreg[i];
    if (v < 0) continue;
    if (!used[v]) {
      r.end[v] = -1;
      continue;
    }
    param_idx[v] = i;
    r.start[v] = 0;
  }
  auto spans_call = [&](int v) {
    auto it = std::lower_bound(calls.begin(), calls.end(), r.start[v]);
    return it != calls.end() && *it < r.end[v];
  };

  std::vector<int> order;
  for (int v = 0; v < nv; v++)
    if (r.end[v] >= 0) order.push_back(v);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (r.start[a] != r.start[b]) return r.start[a] < r.start[b];
    int pa = param_idx[a] < 0 ? INT_MAX : param_idx[a];
    int pb = param_idx[b] < 0 ? INT_MAX : param_idx[b];
    if (pa != pb) return pa < pb;
    return a < b;
  });

  r.reg.assign(nv, -1);
  r.slot.assign(nv, -1);
  std::vector<char> free_reg(256, 1);
  std::vector<int> active;
  std::vector<char> callee_used(256, 0);

  for (int v : order) {
    for (size_t k = 0; k < active.size();) {
      int a = active[k];
      if (r.end[a] < r.start[v]) {
        free_reg[r.reg[a]] = 1;
        active[k] = active.back();
        active.pop_back();
      } else {
        k++;
      }
    }
    bool across = !cfg.vm && spans_call(v);
    auto allowed = [&](int reg) { return !across || cfg.callee_saved[reg]; };
    int pick = -1;
    if (cfg.vm && param_idx[v] >= 0) {
      pick = param_idx[v];
    } else {
      int h = hint[v];
      if (h >= 0 && r.reg[h] >= 0 && free_reg[r.reg[h]] && allowed(r.reg[h])) {
        pick = r.reg[h];
        r.moves_coalesced++;
      }
      int fh = reg_hint[v];
      if (pick < 0 && fh >= 0 && free_reg[fh] && allowed(fh) &&
          std::find(cfg.order.begin(), cfg.order.end(), fh) != cfg.order.end())
        pick = fh;
      for (size_t k = 0; pick < 0 && k < cfg.order.size(); k++)
        if (free_reg[cfg.order[k]] && allowed(cfg.order[k])) pick = cfg.order[k];
    }
    if (pick < 0) {
      if (cfg.vm) {
        r.ok = false;
        r.error = "function " + f.name + " needs more than " + std::to_string(cfg.order.size()) + " VM registers";
        return r;
      }
      int victim = -1;
      for (int a : active)
        if (allowed(r.reg[a]) && (victim < 0 || r.end[a] > r.end[victim])) victim = a;
      if (victim >= 0 && r.end[victim] > r.end[v]) {
        pick = r.reg[victim];
        r.reg[victim] = -1;
        r.slot[victim] = r.nslots++;
        active.erase(std::find(active.begin(), active.end(), victim));
      } else {
        r.slot[v] = r.nslots++;
        r.spilled++;
        continue;
      }
      r.spilled++;
    }
    r.reg[v] = pick;
    free_reg[pick] = 0;
    active.push_back(v);
    r.max_reg = std::max(r.max_reg, pick);
    if (!cfg.vm && cfg.callee_saved[pick]) callee_used[pick] = 1;
  }
  for (int k = 0; k < 256; k++)
    if (callee_used[k]) r.used_callee_saved.push_back(k);
  return r;
}

std::string print_intervals(const MFunc& f, const RAResult& r) {
  std::ostringstream o;
  o << "intervals " << f.name << ": " << r.spilled << " spilled, " << r.nslots << " slots, "
    << r.moves_coalesced << " moves coalesced\n";
  for (int v = 0; v < f.nvregs; v++) {
    if (r.end[v] < 0) continue;
    o << "  v" << v << " [" << r.start[v] << ", " << r.end[v] << "] -> ";
    if (r.reg[v] >= 0) o << "r" << r.reg[v];
    else o << "slot" << r.slot[v];
    o << "\n";
  }
  return o.str();
}
