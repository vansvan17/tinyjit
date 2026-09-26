#include "jit.h"
#include <elf.h>
#include <sys/mman.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "regalloc.h"
#include "runtime.h"
#include "x86.h"

namespace {

// System V AMD64: integer arguments in these registers, result in rax.
const Reg kArgRegs[6] = {RDI, RSI, RDX, RCX, R8, R9};

// Allocatable registers. rax, rcx, rdx, r10, r11 are kept back as scratch
// for instruction selection (idiv needs rax/rdx, spilled operands get loaded
// into rax/rcx, parallel moves break cycles through rax). rsp and rbp run
// the frame. Caller-saved ones come first so short-lived values do not force
// the prologue to save a callee-saved register.
const Reg kAllocOrder[] = {RSI, RDI, R8, R9, RBX, R12, R13, R14, R15};

struct Loc {
  bool reg;
  Reg r;
  int32_t disp;  // [rbp + disp] when !reg
};

Cond cond_of(MOp cc) {
  switch (cc) {
    case MOp::Lt: return CC_L;
    case MOp::Le: return CC_LE;
    case MOp::Gt: return CC_G;
    case MOp::Ge: return CC_GE;
    case MOp::Eq: return CC_E;
    default: return CC_NE;
  }
}

size_t align_up(size_t x, size_t a) { return (x + a - 1) / a * a; }

// Code generation for one function.
//
// Frame layout (rbp-relative), set up by the prologue:
//   [rbp + 8]           return address
//   [rbp]               caller's rbp
//   [rbp - 8 * (i+1)]   saved callee-saved register i
//   [rbp - 8 * (n+s+1)] spill slot s   (n = number of saved registers)
// rsp is kept 16-byte aligned for the whole body, as the ABI requires at
// every call, so nothing needs re-aligning around calls.
struct FnGen {
  Asm& a;
  MFunc& m;
  const RAResult& ra;
  std::vector<Reg> saved;
  std::vector<size_t> block_off;
  std::vector<std::pair<size_t, int>> jumps;         // rel32 -> block
  std::vector<size_t> type_err, div0, rets;          // rel32 -> stubs / epilogue
  std::vector<std::pair<size_t, int>> calls;         // rel32 -> function index
  std::vector<std::pair<size_t, const char*>> helpers;  // imm64 -> runtime symbol

  FnGen(Asm& a, MFunc& m, const RAResult& ra) : a(a), m(m), ra(ra) {
    for (int r : ra.used_callee_saved) saved.push_back((Reg)r);
  }

  Loc loc(int v) const {
    if (ra.reg[v] >= 0) return {true, (Reg)ra.reg[v], 0};
    return {false, RAX, -8 * (int32_t)(saved.size() + ra.slot[v] + 1)};
  }
  // Operand in a register: its own, or `scratch` after a load from its slot.
  Reg use(int v, Reg scratch) {
    Loc l = loc(v);
    if (l.reg) return l.r;
    a.load(scratch, RBP, l.disp);
    return scratch;
  }
  void def(int v, Reg src) {
    Loc l = loc(v);
    if (l.reg) a.mov(l.r, src);
    else a.store(RBP, l.disp, src);
  }
  void guard_int(Reg r) {
    a.test_imm(r, 1);
    type_err.push_back(a.jcc(CC_NE));
  }
  // One test for two operands: (x | y) has a low bit iff either is a pointer.
  void guard_both(const MIns& x, Reg ra_, Reg rb) {
    if (!x.a_int && !x.b_int) {
      a.mov(RDX, ra_);
      a.or_(RDX, rb);
      guard_int(RDX);
    } else if (!x.a_int) {
      guard_int(ra_);
    } else if (!x.b_int) {
      guard_int(rb);
    }
  }
  void call_helper(const char* name, void* fn) {
    helpers.push_back({a.mov_imm64(RAX, (uint64_t)fn), name});
    a.call_reg(RAX);
  }
  void branch(Cond c, int t, int f, int next) {
    if (t == next) {
      jumps.push_back({a.jcc((Cond)(c ^ 1)), f});
    } else {
      jumps.push_back({a.jcc(c), t});
      if (f != next) jumps.push_back({a.jmp(), f});
    }
  }

  // Emit a set of moves that conceptually happen at the same instant
  // (argument setup, parameter reception). Destinations in memory first,
  // then register-to-register moves in dependency order, breaking cycles
  // (e.g. rdi <-> rsi) through rax, then loads from stack slots.
  void parallel_move(std::vector<std::pair<Loc, Loc>> moves) {
    std::vector<std::pair<Reg, Reg>> rr;
    std::vector<std::pair<Reg, int32_t>> rm;
    for (auto& [d, s] : moves) {
      if (!d.reg) {
        if (s.reg) {
          a.store(RBP, d.disp, s.r);
        } else if (s.disp != d.disp) {
          a.load(RAX, RBP, s.disp);
          a.store(RBP, d.disp, RAX);
        }
      } else if (s.reg) {
        if (d.r != s.r) rr.push_back({d.r, s.r});
      } else {
        rm.push_back({d.r, s.disp});
      }
    }
    while (!rr.empty()) {
      bool progress = false;
      for (size_t i = 0; i < rr.size() && !progress; i++) {
        Reg d = rr[i].first;
        bool blocked = false;
        for (size_t j = 0; j < rr.size(); j++)
          if (j != i && rr[j].second == d) blocked = true;
        if (!blocked) {
          a.mov(d, rr[i].second);
          rr.erase(rr.begin() + i);
          progress = true;
        }
      }
      if (!progress) {
        Reg d = rr[0].first;
        a.mov(RAX, d);
        for (auto& mv : rr)
          if (mv.second == d) mv.second = RAX;
      }
    }
    for (auto& [d, disp] : rm) a.load(d, RBP, disp);
  }

  void gen() {
    int frame = (int)align_up(8 * (saved.size() + ra.nslots), 16);
    a.push(RBP);
    a.mov(RBP, RSP);
    if (frame) a.sub_imm(RSP, frame);
    for (size_t i = 0; i < saved.size(); i++) a.store(RBP, -8 * (int32_t)(i + 1), saved[i]);

    std::vector<std::pair<Loc, Loc>> pm;
    for (int i = 0; i < m.nparams; i++) {
      int v = m.param_vreg[i];
      if (v >= 0 && ra.end[v] >= 0) pm.push_back({loc(v), {true, kArgRegs[i], 0}});
    }
    parallel_move(pm);

    int nb = (int)m.blocks.size();
    block_off.resize(nb);
    for (int bi = 0; bi < nb; bi++) {
      block_off[bi] = a.pos();
      int next = bi + 1 < nb ? bi + 1 : -1;
      auto& ins = m.blocks[bi].ins;
      for (size_t k = 0; k < ins.size(); k++) {
        bool last = bi == nb - 1 && k + 1 == ins.size();
        gen_ins(ins[k], next, last);
      }
    }

    size_t epi = a.pos();
    for (size_t at : rets) a.patch_rel32(at, epi);
    for (size_t i = 0; i < saved.size(); i++) a.load(saved[i], RBP, -8 * (int32_t)(i + 1));
    a.mov(RSP, RBP);
    a.pop(RBP);
    a.ret();

    // Out-of-line error stubs, shared by every guard in the function.
    if (!type_err.empty()) {
      for (size_t at : type_err) a.patch_rel32(at, a.pos());
      a.mov_imm(RDI, RT_TYPE);
      call_helper("rt_error", (void*)rt_error);
    }
    if (!div0.empty()) {
      for (size_t at : div0) a.patch_rel32(at, a.pos());
      a.mov_imm(RDI, RT_DIVZERO);
      call_helper("rt_error", (void*)rt_error);
    }
    for (auto& [at, b] : jumps) a.patch_rel32(at, block_off[b]);
  }

  void gen_ins(const MIns& x, int next, bool last) {
    switch (x.op) {
      case MOp::Param:
        break;
      case MOp::Const: {
        Loc d = loc(x.dst);
        if (d.reg) {
          a.mov_imm(d.r, x.imm);
        } else if (x.imm == (int32_t)x.imm) {
          a.store_imm(RBP, d.disp, (int32_t)x.imm);
        } else {
          a.mov_imm(RAX, x.imm);
          a.store(RBP, d.disp, RAX);
        }
        break;
      }
      case MOp::Mov: {
        Loc d = loc(x.dst), s = loc(x.a);
        if (d.reg && s.reg) a.mov(d.r, s.r);
        else if (d.reg) a.load(d.r, RBP, s.disp);
        else if (s.reg) a.store(RBP, d.disp, s.r);
        else if (d.disp != s.disp) { a.load(RAX, RBP, s.disp); a.store(RBP, d.disp, RAX); }
        break;
      }
      case MOp::Add: case MOp::Sub: case MOp::Mul: {
        // Tagged arithmetic: (2x) + (2y) = 2(x+y), and (2x >> 1) * 2y = 2xy,
        // so add/sub need no untagging and mul needs one shift.
        // x86 ALU ops are two-address (dst op= src), so compute straight into
        // dst's register when dst is not also the right operand; otherwise
        // go through rax.
        Reg ra_ = use(x.a, RAX);
        Reg rb = x.b_imm ? RAX : use(x.b, RCX);
        if (x.b_imm) {
          if (!x.a_int) guard_int(ra_);
        } else {
          guard_both(x, ra_, rb);
        }
        Loc d = loc(x.dst);
        Reg w = (d.reg && (x.b_imm || d.r != rb)) ? d.r : RAX;
        a.mov(w, ra_);
        if (x.op == MOp::Add) {
          if (x.b_imm) a.add_imm(w, (int32_t)x.imm); else a.add(w, rb);
        } else if (x.op == MOp::Sub) {
          if (x.b_imm) a.sub_imm(w, (int32_t)x.imm); else a.sub(w, rb);
        } else {
          a.sar(w, 1);
          if (x.b_imm) a.imul_imm(w, w, (int32_t)x.imm); else a.imul(w, rb);
        }
        def(x.dst, w);
        break;
      }
      case MOp::Div: case MOp::Mod: {
        if (x.b_imm) {
          gen_divmod_imm(x);
          break;
        }
        Reg ra_ = use(x.a, R10);
        Reg rb = use(x.b, R11);
        guard_both(x, ra_, rb);
        a.test(rb, rb);
        div0.push_back(a.jcc(CC_E));
        a.mov(RAX, ra_);
        a.sar(RAX, 1);
        a.mov(RCX, rb);
        a.sar(RCX, 1);
        a.cqo();
        a.idiv(RCX);  // rax = quotient, rdx = remainder
        Reg res = x.op == MOp::Div ? RAX : RDX;
        a.shl(res, 1);
        def(x.dst, res);
        break;
      }
      case MOp::Lt: case MOp::Le: case MOp::Gt: case MOp::Ge: case MOp::Eq: case MOp::Ne: {
        emit_cmp(x, x.op);
        a.setcc(cond_of(x.op), RAX);
        a.movzx8(RAX, RAX);
        a.shl(RAX, 1);
        def(x.dst, RAX);
        break;
      }
      case MOp::Neg: {
        Reg r = use(x.a, RAX);
        if (!x.a_int) guard_int(r);
        a.mov(RAX, r);
        a.neg(RAX);
        def(x.dst, RAX);
        break;
      }
      case MOp::Not: {
        Reg r = use(x.a, RAX);
        a.test(r, r);
        a.setcc(CC_E, RAX);
        a.movzx8(RAX, RAX);
        a.shl(RAX, 1);
        def(x.dst, RAX);
        break;
      }
      case MOp::Car: case MOp::Cdr: {
        Reg r = use(x.a, RAX);
        a.mov(RAX, r);
        a.test_imm(RAX, 1);           // ints have the low bit clear
        type_err.push_back(a.jcc(CC_E));
        a.cmp_imm(RAX, (int32_t)NIL);
        type_err.push_back(a.jcc(CC_E));
        a.load(RAX, RAX, x.op == MOp::Car ? CAR_OFFSET : CDR_OFFSET);
        def(x.dst, RAX);
        break;
      }
      case MOp::IsPair: {
        Reg r = use(x.a, RDX);
        a.mov(RAX, r);
        a.and_imm(RAX, 1);
        a.cmp_imm(r, (int32_t)NIL);
        a.setcc(CC_NE, RCX);
        a.movzx8(RCX, RCX);
        a.and_(RAX, RCX);
        a.shl(RAX, 1);
        def(x.dst, RAX);
        break;
      }
      case MOp::IsNil: {
        Reg r = use(x.a, RAX);
        a.cmp_imm(r, (int32_t)NIL);
        a.setcc(CC_E, RAX);
        a.movzx8(RAX, RAX);
        a.shl(RAX, 1);
        def(x.dst, RAX);
        break;
      }
      case MOp::Print: {
        Reg r = use(x.a, RAX);
        a.mov(RDI, r);
        call_helper("rt_print", (void*)rt_print);
        break;
      }
      case MOp::Call: {
        std::vector<std::pair<Loc, Loc>> pm;
        for (size_t i = 0; i < x.args.size(); i++) pm.push_back({{true, kArgRegs[i], 0}, loc(x.args[i])});
        parallel_move(pm);
        calls.push_back({a.call_rel(), (int)x.imm});
        def(x.dst, RAX);
        break;
      }
      case MOp::Cons:
        fprintf(stderr, "jit: cons reached codegen\n");
        abort();
      case MOp::Jmp:
        if (x.t != next) jumps.push_back({a.jmp(), x.t});
        break;
      case MOp::Br: {
        Reg r = use(x.a, RAX);
        a.test(r, r);
        branch(CC_NE, x.t, x.f, next);
        break;
      }
      case MOp::BrCmp:
        emit_cmp(x, x.cc);
        branch(cond_of(x.cc), x.t, x.f, next);
        break;
      case MOp::Ret: {
        Reg r = use(x.a, RAX);
        a.mov(RAX, r);
        if (!last) rets.push_back(a.jmp());
        break;
      }
    }
  }

  // Division by a constant. For 2^k this is the textbook shift sequence
  // (what gcc emits for `x / 8`): add 2^k - 1 to negative dividends so the
  // arithmetic shift rounds toward zero like idiv does. idiv costs 20-40
  // cycles; this is four single-cycle instructions.
  void gen_divmod_imm(const MIns& x) {
    int64_t d = as_int((Value)x.imm);
    Reg r = use(x.a, RAX);
    if (!x.a_int) guard_int(r);
    a.mov(RAX, r);
    a.sar(RAX, 1);  // untag
    bool pow2 = d > 0 && (d & (d - 1)) == 0;
    int k = pow2 ? __builtin_ctzll((uint64_t)d) : 0;
    Reg res;
    if (pow2 && k == 0) {  // x / 1, x % 1
      if (x.op == MOp::Mod) a.mov_imm(RAX, 0);
      res = RAX;
    } else if (pow2) {
      a.mov(RDX, RAX);
      a.sar(RDX, 63);
      a.shr(RDX, (uint8_t)(64 - k));  // 2^k - 1 if negative, else 0
      a.add(RDX, RAX);
      a.sar(RDX, (uint8_t)k);         // quotient
      if (x.op == MOp::Div) {
        res = RDX;
      } else {
        a.shl(RDX, (uint8_t)k);
        a.sub(RAX, RDX);              // remainder = x - q * 2^k
        res = RAX;
      }
    } else {
      a.mov_imm(RCX, d);
      a.cqo();
      a.idiv(RCX);
      res = x.op == MOp::Div ? RAX : RDX;
    }
    a.shl(res, 1);  // retag
    def(x.dst, res);
  }

  void emit_cmp(const MIns& x, MOp cc) {
    bool ordering = mop_is_ordering(cc);
    Reg ra_ = use(x.a, RAX);
    if (x.b_imm) {
      if (ordering && !x.a_int) guard_int(ra_);
      a.cmp_imm(ra_, (int32_t)x.imm);
    } else {
      Reg rb = use(x.b, RCX);
      if (ordering) guard_both(x, ra_, rb);
      a.cmp(ra_, rb);
    }
  }
};

}  // namespace

JIT::JIT(VM& vm, std::vector<MFunc>& mir) : vm_(vm), mir_(mir) {
  cap_ = 64 << 20;  // one region, so every call between JIT functions fits in rel32
  void* p = mmap(nullptr, cap_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  mem_ = (uint8_t*)p;
}

JIT::~JIT() { munmap(mem_, cap_); }

bool JIT::collect_group(int f, std::vector<char>& in_group, std::vector<int>& group) {
  VMFunc& vf = vm_.funcs[f];
  if (vf.native || in_group[f]) return true;
  if (vf.jit_state == 2) return false;
  MFunc& m = mir_[f];
  if (m.has_cons || m.nparams > 6) {
    vf.jit_state = 2;
    return false;
  }
  in_group[f] = 1;
  group.push_back(f);
  for (int c : m.callees)
    if (!collect_group(c, in_group, group)) return false;
  return true;
}

bool JIT::compile(int fidx) {
  VMFunc& vf = vm_.funcs[fidx];
  if (vf.native) return true;
  if (vf.jit_state == 2) return false;
  std::vector<char> in_group(mir_.size(), 0);
  std::vector<int> group;
  if (!collect_group(fidx, in_group, group)) {
    vf.jit_state = 2;
    return false;
  }

  auto t0 = std::chrono::steady_clock::now();
  RAConfig cfg;
  cfg.order.assign(std::begin(kAllocOrder), std::end(kAllocOrder));
  cfg.callee_saved.assign(256, false);
  for (Reg r : {RBX, R12, R13, R14, R15}) cfg.callee_saved[r] = true;
  cfg.arg_regs.assign(std::begin(kArgRegs), std::end(kArgRegs));

  Asm a;
  std::vector<size_t> off(mir_.size()), size(mir_.size());
  std::vector<std::pair<size_t, int>> calls;
  std::vector<std::pair<size_t, const char*>> helpers;
  for (int f : group) {
    while (a.pos() % 16) a.u8(0xCC);
    off[f] = a.pos();
    RAResult ra = linear_scan(mir_[f], cfg);
    spills += ra.spilled;
    if (print_intervals) fprintf(stderr, "%s", ::print_intervals(mir_[f], ra).c_str());
    FnGen g(a, mir_[f], ra);
    g.gen();
    calls.insert(calls.end(), g.calls.begin(), g.calls.end());
    helpers.insert(helpers.end(), g.helpers.begin(), g.helpers.end());
    size[f] = a.pos() - off[f];
  }

  // Code is packed back to back (16-byte aligned) so a hot loop's functions
  // share cache lines and pages. The pages being written are made writable
  // just for the copy and executable again right after (W^X: never both).
  // This is safe because compilation only happens from the interpreter,
  // never while JIT code is on the stack.
  size_t start = align_up(used_, 16);
  if (start + a.pos() > cap_) {
    vf.jit_state = 2;
    return false;
  }
  uint8_t* dest = mem_ + start;
  size_t page_lo = used_ / 4096 * 4096;  // includes the padding written below
  size_t page_hi = align_up(start + a.pos(), 4096);
  if (mprotect(mem_ + page_lo, page_hi - page_lo, PROT_READ | PROT_WRITE) != 0) { perror("mprotect"); exit(1); }
  memset(mem_ + used_, 0xCC, start - used_);  // int3 between units
  memcpy(dest, a.b.data(), a.pos());
  for (auto& [at, callee] : calls) {
    uint8_t* target = in_group[callee] ? dest + off[callee] : (uint8_t*)vm_.funcs[callee].native;
    int32_t rel = (int32_t)(target - (dest + at + 4));
    memcpy(dest + at, &rel, 4);
  }
  if (mprotect(mem_ + page_lo, page_hi - page_lo, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); exit(1); }
  used_ = start + a.pos();

  for (int f : group) {
    vm_.funcs[f].native = dest + off[f];
    vm_.funcs[f].jit_state = 1;
    syms_.push_back({mir_[f].name, start + off[f], size[f]});
    functions_compiled++;
  }
  for (auto& [at, name] : helpers) relocs_.push_back({start + at, name});
  compile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

void JIT::compile_all() {
  for (size_t f = 0; f < vm_.funcs.size(); f++)
    if (vm_.funcs[f].jit_state == 0) compile((int)f);
}

bool JIT::write_raw(const std::string& path) const {
  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) return false;
  fwrite(mem_, 1, used_, fp);
  fclose(fp);
  return true;
}

// A relocatable ELF64 object with the generated code:
//   .text       all compiled code (internal calls are already PC-relative)
//   .rela.text  R_X86_64_64 for each absolute address of a runtime helper
//   .symtab     tiny_<name> for every function, plus undefined rt_* helpers
//   .strtab, .shstrtab
//   .note.GNU-stack (empty: no executable stack needed)
// objdump -dr shows it with symbol names, and `cc -no-pie` can link it
// against aot/runtime.c into a standalone executable (an AOT compiler).
bool JIT::write_elf(const std::string& path, std::string& err) const {
  std::vector<uint8_t> text(mem_, mem_ + used_);
  for (auto& r : relocs_) memset(&text[r.offset], 0, 8);  // RELA: value comes from the addend

  std::string strtab(1, '\0');
  auto add_str = [&](const std::string& s) {
    size_t o = strtab.size();
    strtab += s;
    strtab += '\0';
    return (uint32_t)o;
  };
  std::vector<Elf64_Sym> syms(2);
  memset(syms.data(), 0, sizeof(Elf64_Sym) * 2);
  syms[1].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
  syms[1].st_shndx = 1;
  for (auto& s : syms_) {
    Elf64_Sym e{};
    e.st_name = add_str("tiny_" + s.name);
    e.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    e.st_shndx = 1;
    e.st_value = s.offset;
    e.st_size = s.size;
    syms.push_back(e);
  }
  std::vector<std::string> helper_names;
  auto helper_index = [&](const std::string& n) {
    for (size_t i = 0; i < helper_names.size(); i++)
      if (helper_names[i] == n) return (uint32_t)(2 + syms_.size() + i);
    helper_names.push_back(n);
    Elf64_Sym e{};
    e.st_name = add_str(n);
    e.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
    e.st_shndx = SHN_UNDEF;
    syms.push_back(e);
    return (uint32_t)(syms.size() - 1);
  };
  std::vector<Elf64_Rela> rela;
  for (auto& r : relocs_) {
    Elf64_Rela e{};
    e.r_offset = r.offset;
    e.r_info = ELF64_R_INFO(helper_index(r.symbol), R_X86_64_64);
    e.r_addend = 0;
    rela.push_back(e);
  }

  const char* names[] = {"", ".text", ".rela.text", ".symtab", ".strtab", ".shstrtab", ".note.GNU-stack"};
  std::string shstr;
  uint32_t name_off[7];
  for (int i = 0; i < 7; i++) {
    name_off[i] = (uint32_t)shstr.size();
    shstr += names[i];
    shstr += '\0';
  }

  size_t off = sizeof(Elf64_Ehdr);
  auto place = [&](size_t bytes, size_t al) { off = align_up(off, al); size_t o = off; off += bytes; return o; };
  size_t text_off = place(text.size(), 16);
  size_t rela_off = place(rela.size() * sizeof(Elf64_Rela), 8);
  size_t sym_off = place(syms.size() * sizeof(Elf64_Sym), 8);
  size_t str_off = place(strtab.size(), 1);
  size_t shstr_off = place(shstr.size(), 1);
  size_t sh_off = place(7 * sizeof(Elf64_Shdr), 8);

  Elf64_Ehdr eh{};
  memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh.e_type = ET_REL;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_shoff = sh_off;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = 7;
  eh.e_shstrndx = 5;

  Elf64_Shdr sh[7] = {};
  sh[1] = {name_off[1], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, text_off, text.size(), 0, 0, 16, 0};
  sh[2] = {name_off[2], SHT_RELA, SHF_INFO_LINK, 0, rela_off, rela.size() * sizeof(Elf64_Rela), 3, 1, 8, sizeof(Elf64_Rela)};
  sh[3] = {name_off[3], SHT_SYMTAB, 0, 0, sym_off, syms.size() * sizeof(Elf64_Sym), 4, 2, 8, sizeof(Elf64_Sym)};
  sh[4] = {name_off[4], SHT_STRTAB, 0, 0, str_off, strtab.size(), 0, 0, 1, 0};
  sh[5] = {name_off[5], SHT_STRTAB, 0, 0, shstr_off, shstr.size(), 0, 0, 1, 0};
  // Empty marker section: tells the linker this code does not need an
  // executable stack.
  sh[6] = {name_off[6], SHT_PROGBITS, 0, 0, shstr_off, 0, 0, 0, 1, 0};

  std::vector<uint8_t> file(off, 0);
  memcpy(&file[0], &eh, sizeof eh);
  memcpy(&file[text_off], text.data(), text.size());
  if (!rela.empty()) memcpy(&file[rela_off], rela.data(), rela.size() * sizeof(Elf64_Rela));
  memcpy(&file[sym_off], syms.data(), syms.size() * sizeof(Elf64_Sym));
  memcpy(&file[str_off], strtab.data(), strtab.size());
  memcpy(&file[shstr_off], shstr.data(), shstr.size());
  memcpy(&file[sh_off], sh, sizeof sh);

  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) { err = "cannot open " + path; return false; }
  fwrite(file.data(), 1, file.size(), fp);
  fclose(fp);
  return true;
}
