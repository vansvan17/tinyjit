// ARM64 JIT for MIR, targeting Apple Silicon.
//
// Pipeline per function: MIR -> linear scan over 22 machine registers ->
// instruction selection straight to A64 machine code -> a MAP_JIT region
// that is writable or executable for this thread, never both at once.
//
// Compiled code follows the Apple arm64 calling convention (AAPCS64 with
// Apple's changes, see docs/04-binary.md), so the interpreter calls it
// through an ordinary C function pointer and it calls the C runtime
// (rt_print, rt_error). A function is compiled together with everything it
// can call, so compiled code never calls back into the interpreter.
#include "jit.h"

#if defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__)) && !defined(TINYJIT_NO_JIT)

#include <sys/mman.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "a64.h"
#include "regalloc.h"
#include "runtime.h"
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

namespace {

// ---------------------------------------------------------------- memory
//
// macOS on Apple Silicon enforces W^X per thread: a MAP_JIT region is
// mapped RWX once, and pthread_jit_write_protect_np() flips whether the
// *current thread* sees it as writable or executable. Other platforms (Linux
// arm64, used for testing under emulation) get the classic mprotect flip.
// Either way the instruction cache has to be told about new code: ARM does
// not keep it coherent with data writes, so without the flush the CPU can
// execute stale bytes.
#ifdef __APPLE__
uint8_t* map_code(size_t n) {
  void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}
void begin_write(uint8_t*, size_t) { pthread_jit_write_protect_np(0); }
void end_write(uint8_t* p, size_t n) {
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(p, n);
}
#else
uint8_t* map_code(size_t n) {
  void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}
size_t page_floor(uintptr_t x) { return x & ~(uintptr_t)4095; }
void begin_write(uint8_t* p, size_t n) {
  uintptr_t lo = page_floor((uintptr_t)p), hi = page_floor((uintptr_t)p + n + 4095);
  if (mprotect((void*)lo, hi - lo, PROT_READ | PROT_WRITE) != 0) { perror("mprotect"); exit(1); }
}
void end_write(uint8_t* p, size_t n) {
  uintptr_t lo = page_floor((uintptr_t)p), hi = page_floor((uintptr_t)p + n + 4095);
  if (mprotect((void*)lo, hi - lo, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); exit(1); }
  __builtin___clear_cache((char*)p, (char*)p + n);
}
#endif

// ---------------------------------------------------------------- registers
//
// x0-x7    arguments and results                  caller-saved
// x8       indirect result (unused here)           caller-saved
// x9-x15   temporaries                             caller-saved
// x16-x17  intra-procedure-call scratch (IP0/IP1)  may be clobbered by veneers
// x18      platform register: reserved on Apple, never touched
// x19-x28  callee-saved
// x29 fp, x30 lr, sp
//
// Scratch for instruction selection: x9-x12, x16, x17. Everything else is
// allocatable: 12 caller-saved and 10 callee-saved registers, against the 9
// an x86-64 JIT gets after taking out its own scratch registers.
const XReg kArgRegs[8] = {X0, X1, X2, X3, X4, X5, X6, X7};
const XReg kAllocOrder[] = {X1,  X2,  X3,  X4,  X5,  X6,  X7,  X8,  X13, X14, X15, X0,
                            X19, X20, X21, X22, X23, X24, X25, X26, X27, X28};
const XReg kCalleeSaved[] = {X19, X20, X21, X22, X23, X24, X25, X26, X27, X28};

struct Helper { const char* name; void* fn; };
const Helper kHelpers[] = {{"rt_print", (void*)rt_print}, {"rt_error", (void*)rt_error}};
enum { H_PRINT, H_ERROR, H_COUNT };

constexpr uint32_t BRK = 0xD4200000;  // brk #0: padding that traps if executed

struct Loc {
  bool reg;
  XReg r;
  int32_t off;  // [fp + off] when !reg
};

Cond cond_of(MOp cc) {
  switch (cc) {
    case MOp::Lt: return LT;
    case MOp::Le: return LE;
    case MOp::Gt: return GT;
    case MOp::Ge: return GE;
    case MOp::Eq: return EQ;
    default: return NE;
  }
}

size_t align_up(size_t x, size_t a) { return (x + a - 1) / a * a; }

// Code generation for one function.
//
// Frame layout (fp-relative), set up by the prologue:
//   [fp + 8]            saved lr (return address)
//   [fp]                caller's fp        <- fp points here
//   [fp - 8 * (i+1)]    saved callee-saved register i
//   [fp - 8 * (n+s+1)]  spill slot s   (n = number of saved registers)
// sp stays 16-byte aligned throughout, which arm64 checks in hardware.
struct FnGen {
  A64& a;
  MFunc& m;
  const RAResult& ra;
  std::vector<XReg> saved;
  std::vector<size_t> block_off;
  std::vector<std::pair<size_t, int>> jumps;      // branch -> block
  std::vector<size_t> type_err, div0, rets;       // branch -> stub / epilogue
  std::vector<std::pair<size_t, int>> calls;      // bl -> function index
  std::vector<std::pair<size_t, int>> helper_bls; // bl -> helper id
  std::vector<size_t> literals;                   // word index of 64-bit literals
  bool ok = true;                                 // false if a branch is out of range

  FnGen(A64& a, MFunc& m, const RAResult& ra) : a(a), m(m), ra(ra) {
    for (int r : ra.used_callee_saved) saved.push_back((XReg)r);
  }

  Loc loc(int v) const {
    if (ra.reg[v] >= 0) return {true, (XReg)ra.reg[v], 0};
    return {false, X9, -8 * (int32_t)(saved.size() + ra.slot[v] + 1)};
  }
  XReg use(int v, XReg scratch) {
    Loc l = loc(v);
    if (l.reg) return l.r;
    a.ldr(scratch, FP, l.off);
    return scratch;
  }
  // Where to compute a result: straight into its register, or X9 if spilled.
  XReg dst(int v) {
    Loc l = loc(v);
    return l.reg ? l.r : X9;
  }
  void def(int v, XReg src) {
    Loc l = loc(v);
    if (l.reg) a.mov(l.r, src);
    else a.str(src, FP, l.off);
  }
  void guard_int(XReg r) {
    a.tst1(r);
    type_err.push_back(a.b_cond(NE));
  }
  // One test for two operands: (x | y) has a low bit iff either is a pointer.
  void guard_both(const MIns& x, XReg A, XReg B) {
    if (!x.a_int && !x.b_int) {
      a.orr(X12, A, B);
      guard_int(X12);
    } else if (!x.a_int) {
      guard_int(A);
    } else if (!x.b_int) {
      guard_int(B);
    }
  }
  void call_helper(int h) { helper_bls.push_back({a.bl(), h}); }
  void branch(Cond c, int t, int f, int next) {
    if (t == next) {
      jumps.push_back({a.b_cond((Cond)(c ^ 1)), f});
    } else {
      jumps.push_back({a.b_cond(c), t});
      if (f != next) jumps.push_back({a.b(), f});
    }
  }

  // Moves that happen "at once" (argument setup, parameter reception):
  // memory destinations first, then register moves in dependency order with
  // cycles (x1 <-> x2) broken through x16, then loads from stack slots.
  void parallel_move(std::vector<std::pair<Loc, Loc>> moves) {
    std::vector<std::pair<XReg, XReg>> rr;
    std::vector<std::pair<XReg, int32_t>> rm;
    for (auto& [d, s] : moves) {
      if (!d.reg) {
        if (s.reg) {
          a.str(s.r, FP, d.off);
        } else if (s.off != d.off) {
          a.ldr(X9, FP, s.off);
          a.str(X9, FP, d.off);
        }
      } else if (s.reg) {
        if (d.r != s.r) rr.push_back({d.r, s.r});
      } else {
        rm.push_back({d.r, s.off});
      }
    }
    while (!rr.empty()) {
      bool progress = false;
      for (size_t i = 0; i < rr.size() && !progress; i++) {
        XReg d = rr[i].first;
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
        XReg d = rr[0].first;
        a.mov(X16, d);
        for (auto& mv : rr)
          if (mv.second == d) mv.second = X16;
      }
    }
    for (auto& [d, off] : rm) a.ldr(d, FP, off);
  }

  void gen() {
    uint32_t frame = (uint32_t)align_up(8 * (saved.size() + ra.nslots), 16);
    a.push_fp_lr();
    a.mov_fp_sp();
    a.sub_sp(frame);
    for (size_t i = 0; i < saved.size(); i++) a.str(saved[i], FP, -8 * (int32_t)(i + 1));

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
      for (size_t k = 0; k < ins.size(); k++) gen_ins(ins[k], next, bi == nb - 1 && k + 1 == ins.size());
    }

    size_t epi = a.pos();
    for (size_t at : rets) ok &= a.patch(at, epi);
    for (size_t i = 0; i < saved.size(); i++) a.ldr(saved[i], FP, -8 * (int32_t)(i + 1));
    a.mov_sp_fp();
    a.pop_fp_lr();
    a.ret();

    // Out-of-line error stubs, shared by every guard in the function.
    if (!type_err.empty()) {
      for (size_t at : type_err) ok &= a.patch(at, a.pos());
      a.movi(X0, RT_TYPE);
      call_helper(H_ERROR);
    }
    if (!div0.empty()) {
      for (size_t at : div0) ok &= a.patch(at, a.pos());
      a.movi(X0, RT_DIVZERO);
      call_helper(H_ERROR);
    }
    // Trampolines to the C runtime. The runtime lives in the tinyjit binary,
    // which can be further than bl's +-128 MB reach from the JIT region, so
    // each `bl` goes to a stub in this function that loads the full 64-bit
    // address from a literal and branches to it.
    size_t tramp[H_COUNT];
    bool used[H_COUNT] = {};
    for (auto& [at, h] : helper_bls) used[h] = true;
    for (int h = 0; h < H_COUNT; h++) {
      if (!used[h]) continue;
      tramp[h] = a.pos();
      a.ldr_literal(X16, 2);  // x16 = the 8 bytes after the next instruction
      a.br(X16);
      literals.push_back(a.pos());
      a.quad((uint64_t)kHelpers[h].fn);
    }
    for (auto& [at, h] : helper_bls) ok &= a.patch(at, tramp[h]);
    for (auto& [at, b] : jumps) ok &= a.patch(at, block_off[b]);
  }

  void gen_ins(const MIns& x, int next, bool last) {
    switch (x.op) {
      case MOp::Param:
        break;
      case MOp::Const: {
        XReg d = dst(x.dst);
        a.movi(d, x.imm);
        def(x.dst, d);
        break;
      }
      case MOp::Mov: {
        Loc d = loc(x.dst), s = loc(x.a);
        if (d.reg && s.reg) a.mov(d.r, s.r);
        else if (d.reg) a.ldr(d.r, FP, s.off);
        else if (s.reg) a.str(s.r, FP, d.off);
        else if (d.off != s.off) { a.ldr(X9, FP, s.off); a.str(X9, FP, d.off); }
        break;
      }
      case MOp::Add: case MOp::Sub: case MOp::Mul: {
        // Tagged arithmetic: (2x) + (2y) = 2(x+y), and (2x >> 1) * 2y = 2xy,
        // so add/sub need no untagging and mul needs one shift. A64 is
        // three-address, so the result goes straight to its register.
        XReg A = use(x.a, X10);
        XReg D = dst(x.dst);
        if (x.b_imm) {
          if (!x.a_int) guard_int(A);
          if (x.op == MOp::Add) a.add_any(D, A, x.imm, X11);
          else if (x.op == MOp::Sub) a.add_any(D, A, -x.imm, X11);
          else { a.asr(X12, A, 1); a.movi(X11, x.imm); a.mul(D, X12, X11); }
        } else {
          XReg B = use(x.b, X11);
          guard_both(x, A, B);
          if (x.op == MOp::Add) a.add(D, A, B);
          else if (x.op == MOp::Sub) a.sub(D, A, B);
          else { a.asr(X12, A, 1); a.mul(D, X12, B); }
        }
        def(x.dst, D);
        break;
      }
      case MOp::Div: case MOp::Mod: {
        if (x.b_imm) { gen_divmod_imm(x); break; }
        XReg A = use(x.a, X10);
        XReg B = use(x.b, X11);
        guard_both(x, A, B);
        div0.push_back(a.cbz(B));
        a.asr(X12, A, 1);  // untag
        a.asr(X16, B, 1);
        a.sdiv(X9, X12, X16);                                  // quotient, truncated
        if (x.op == MOp::Mod) a.msub(X9, X9, X16, X12);       // x - q * y
        XReg D = dst(x.dst);
        a.lsl(D, X9, 1);  // retag
        def(x.dst, D);
        break;
      }
      case MOp::Lt: case MOp::Le: case MOp::Gt: case MOp::Ge: case MOp::Eq: case MOp::Ne: {
        emit_cmp(x, x.op);
        XReg D = dst(x.dst);
        a.cset(X9, cond_of(x.op));
        a.lsl(D, X9, 1);
        def(x.dst, D);
        break;
      }
      case MOp::Neg: {
        XReg A = use(x.a, X10);
        if (!x.a_int) guard_int(A);
        XReg D = dst(x.dst);
        a.neg(D, A);
        def(x.dst, D);
        break;
      }
      case MOp::Not: {
        XReg A = use(x.a, X10);
        a.cmp_any(A, 0, X11);
        a.cset(X9, EQ);
        XReg D = dst(x.dst);
        a.lsl(D, X9, 1);
        def(x.dst, D);
        break;
      }
      case MOp::Car: case MOp::Cdr: {
        XReg A = use(x.a, X10);
        a.tst1(A);  // ints have the low bit clear
        type_err.push_back(a.b_cond(EQ));
        a.cmp_any(A, (int64_t)NIL, X11);
        type_err.push_back(a.b_cond(EQ));
        XReg D = dst(x.dst);
        a.ldr(D, A, x.op == MOp::Car ? CAR_OFFSET : CDR_OFFSET);
        def(x.dst, D);
        break;
      }
      case MOp::IsPair: {
        XReg A = use(x.a, X10);
        a.and1(X9, A);
        a.cmp_any(A, (int64_t)NIL, X11);
        a.cset(X12, NE);
        a.and_(X9, X9, X12);
        XReg D = dst(x.dst);
        a.lsl(D, X9, 1);
        def(x.dst, D);
        break;
      }
      case MOp::IsNil: {
        XReg A = use(x.a, X10);
        a.cmp_any(A, (int64_t)NIL, X11);
        a.cset(X9, EQ);
        XReg D = dst(x.dst);
        a.lsl(D, X9, 1);
        def(x.dst, D);
        break;
      }
      case MOp::Print: {
        XReg A = use(x.a, X10);
        a.mov(X0, A);
        call_helper(H_PRINT);
        break;
      }
      case MOp::Call: {
        std::vector<std::pair<Loc, Loc>> pm;
        for (size_t i = 0; i < x.args.size(); i++) pm.push_back({{true, kArgRegs[i], 0}, loc(x.args[i])});
        parallel_move(pm);
        calls.push_back({a.bl(), (int)x.imm});
        def(x.dst, X0);
        break;
      }
      case MOp::Cons:
        fprintf(stderr, "jit: cons reached codegen\n");
        abort();
      case MOp::Jmp:
        if (x.t != next) jumps.push_back({a.b(), x.t});
        break;
      case MOp::Br: {
        XReg A = use(x.a, X10);
        if (x.t == next) {
          jumps.push_back({a.cbz(A), x.f});
        } else {
          jumps.push_back({a.cbnz(A), x.t});
          if (x.f != next) jumps.push_back({a.b(), x.f});
        }
        break;
      }
      case MOp::BrCmp:
        emit_cmp(x, x.cc);
        branch(cond_of(x.cc), x.t, x.f, next);
        break;
      case MOp::Ret: {
        XReg A = use(x.a, X10);
        a.mov(X0, A);
        if (!last) rets.push_back(a.b());
        break;
      }
    }
  }

  // Division by a constant. For 2^k this is the shift sequence clang emits
  // for `x / 8`: add 2^k - 1 to negative dividends so the arithmetic shift
  // rounds toward zero like sdiv does.
  void gen_divmod_imm(const MIns& x) {
    int64_t d = as_int((Value)x.imm);
    XReg A = use(x.a, X10);
    if (!x.a_int) guard_int(A);
    a.asr(X12, A, 1);  // untag: x
    XReg D = dst(x.dst);
    bool pow2 = d > 0 && (d & (d - 1)) == 0;
    int k = pow2 ? __builtin_ctzll((uint64_t)d) : 0;
    if (pow2 && k == 0) {  // x / 1, x % 1
      if (x.op == MOp::Div) a.lsl(D, X12, 1);
      else a.movi(D, 0);
    } else if (pow2) {
      a.asr(X16, X12, 63);
      a.lsr(X16, X16, 64 - k);  // 2^k - 1 if negative, else 0
      a.add(X16, X16, X12);
      a.asr(X16, X16, k);       // quotient
      if (x.op == MOp::Div) {
        a.lsl(D, X16, 1);
      } else {
        a.lsl(X16, X16, k);
        a.sub(X16, X12, X16);   // remainder = x - q * 2^k
        a.lsl(D, X16, 1);
      }
    } else {
      a.movi(X16, d);
      a.sdiv(X9, X12, X16);
      if (x.op == MOp::Mod) a.msub(X9, X9, X16, X12);
      a.lsl(D, X9, 1);
    }
    def(x.dst, D);
  }

  void emit_cmp(const MIns& x, MOp cc) {
    bool ordering = mop_is_ordering(cc);
    XReg A = use(x.a, X10);
    if (x.b_imm) {
      if (ordering && !x.a_int) guard_int(A);
      a.cmp_any(A, x.imm, X11);
    } else {
      XReg B = use(x.b, X11);
      if (ordering) guard_both(x, A, B);
      a.cmp(A, B);
    }
  }
};

}  // namespace

bool jit_supported() { return true; }

JIT::JIT(VM& vm, std::vector<MFunc>& mir) : vm_(vm), mir_(mir) {
  cap_ = 64 << 20;  // one region, so every call between JIT functions fits in bl's +-128 MB
  mem_ = map_code(cap_);
  if (!mem_) { perror("mmap"); exit(1); }
}

JIT::~JIT() { munmap(mem_, cap_); }

bool JIT::collect_group(int f, std::vector<char>& in_group, std::vector<int>& group) {
  VMFunc& vf = vm_.funcs[f];
  if (vf.native || in_group[f]) return true;
  if (vf.jit_state == 2) return false;
  MFunc& m = mir_[f];
  if (m.has_cons || m.nparams > 8) {
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
  for (XReg r : kCalleeSaved) cfg.callee_saved[r] = true;
  cfg.arg_regs.assign(std::begin(kArgRegs), std::end(kArgRegs));

  A64 a;
  std::vector<size_t> off(mir_.size()), size(mir_.size());
  std::vector<std::pair<size_t, int>> calls;
  std::vector<std::pair<size_t, const char*>> helpers;
  std::vector<size_t> literals;
  bool ok = true;
  for (int f : group) {
    while (a.pos() % 4) a.emit(BRK);  // functions start on 16 bytes
    off[f] = a.pos();
    RAResult ra = linear_scan(mir_[f], cfg);
    spills += ra.spilled;
    if (print_intervals) fprintf(stderr, "%s", ::print_intervals(mir_[f], ra).c_str());
    FnGen g(a, mir_[f], ra);
    g.gen();
    ok &= g.ok;
    calls.insert(calls.end(), g.calls.begin(), g.calls.end());
    for (auto& [at, h] : g.helper_bls) helpers.push_back({at, kHelpers[h].name});
    literals.insert(literals.end(), g.literals.begin(), g.literals.end());
    size[f] = a.pos() - off[f];
  }
  size_t bytes = a.pos() * 4;
  size_t start = align_up(used_, 16);
  if (!ok || start + bytes > cap_) {  // a function too big for b.cond's reach, or out of space
    for (int f : group) vm_.funcs[f].jit_state = 2;
    return false;
  }

  // Resolve calls between JIT functions: bl to a function in this group, or
  // to one compiled earlier (anywhere in the same 64 MB region).
  uint8_t* dest = mem_ + start;
  for (auto& [at, callee] : calls) {
    int64_t target = in_group[callee] ? (int64_t)off[callee]
                                      : ((uint8_t*)vm_.funcs[callee].native - dest) / 4;
    int64_t d = target - (int64_t)at;
    a.w[at] = (a.w[at] & 0xFC000000) | ((uint32_t)d & 0x03FFFFFF);
  }

  // Code is packed back to back (16-byte aligned) so a hot loop's functions
  // share cache lines and pages. Compilation only happens from the
  // interpreter, never while JIT code is on the stack, so flipping pages to
  // writable is safe.
  begin_write(mem_ + used_, start + bytes - used_);
  for (size_t p = used_; p < start; p += 4) memcpy(mem_ + p, &BRK, 4);
  memcpy(dest, a.w.data(), bytes);
  end_write(mem_ + used_, start + bytes - used_);
  used_ = start + bytes;

  for (int f : group) {
    vm_.funcs[f].native = dest + off[f] * 4;
    vm_.funcs[f].jit_state = 1;
    syms_.push_back({mir_[f].name, start + off[f] * 4, size[f] * 4});
    functions_compiled++;
  }
  for (auto& [at, name] : helpers) relocs_.push_back({start + at * 4, name});
  for (size_t at : literals) literals_.push_back(start + at * 4);
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

// ---------------------------------------------------------------- Mach-O
//
// A relocatable Mach-O object (MH_OBJECT) for arm64 holding all compiled
// code, so `objdump -d -r` / `otool -tvV` can show it with symbol names and
// `cc` can link it into an executable (see aot/runtime.c):
//
//   mach_header_64
//   LC_SEGMENT_64    one unnamed segment with one section, __TEXT,__text
//   LC_BUILD_VERSION platform macOS, so the linker does not warn
//   LC_SYMTAB        where the symbol and string tables are
//   LC_DYSYMTAB      which symbols are local / defined / undefined
//   __text bytes, relocations, nlist_64 symbols, strings
//
// Calls into the runtime are `bl` instructions that, in memory, go to a
// trampoline. In the object they instead carry an ARM64_RELOC_BRANCH26
// relocation against _rt_print / _rt_error, and the linker points them at
// the real function (adding its own stub if it is out of reach). The
// trampolines stay in the file as dead code, with their addresses zeroed.
// The structs are spelled out here instead of using <mach-o/loader.h> so the
// layout is visible and the file also builds on Linux.

namespace macho {
#pragma pack(push, 4)
struct Header { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct Segment {
  uint32_t cmd, cmdsize;
  char segname[16];
  uint64_t vmaddr, vmsize, fileoff, filesize;
  int32_t maxprot, initprot;
  uint32_t nsects, flags;
};
struct Section {
  char sectname[16], segname[16];
  uint64_t addr, size;
  uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct BuildVersion { uint32_t cmd, cmdsize, platform, minos, sdk, ntools; };
struct Symtab { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct Dysymtab {
  uint32_t cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
  uint32_t rest[12];
};
struct Nlist { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value; };
struct Reloc { int32_t r_address; uint32_t info; };
#pragma pack(pop)
constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF, CPU_TYPE_ARM64 = 0x0100000C, MH_OBJECT = 1;
constexpr uint32_t MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000;
constexpr uint32_t LC_SEGMENT_64 = 0x19, LC_SYMTAB = 0x2, LC_DYSYMTAB = 0xB, LC_BUILD_VERSION = 0x32;
constexpr uint32_t PLATFORM_MACOS = 1;
constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000, S_ATTR_SOME_INSTRUCTIONS = 0x400;
constexpr uint8_t N_EXT = 0x01, N_SECT = 0x0E, N_UNDF = 0x0;
constexpr uint32_t ARM64_RELOC_BRANCH26 = 2;
}  // namespace macho

bool JIT::write_object(const std::string& path, std::string& err) const {
  using namespace macho;
  std::vector<uint8_t> text(mem_, mem_ + used_);
  for (size_t at : literals_) memset(&text[at], 0, 8);

  // Symbols: defined functions first, then undefined helpers, each sorted
  // by name (the order LC_DYSYMTAB describes).
  struct S { std::string name; bool defined; uint64_t value; };
  std::vector<S> defs, undefs;
  for (auto& s : syms_) defs.push_back({"_tiny_" + s.name, true, s.offset});
  for (auto& r : relocs_) {
    std::string n = std::string("_") + r.symbol;
    bool seen = false;
    for (auto& u : undefs) seen |= u.name == n;
    if (!seen) undefs.push_back({n, false, 0});
  }
  auto by_name = [](const S& x, const S& y) { return x.name < y.name; };
  std::sort(defs.begin(), defs.end(), by_name);
  std::sort(undefs.begin(), undefs.end(), by_name);
  std::vector<S> all = defs;
  all.insert(all.end(), undefs.begin(), undefs.end());

  std::string strtab(1, '\0');
  std::vector<Nlist> nl;
  for (auto& s : all) {
    Nlist e{};
    e.n_strx = (uint32_t)strtab.size();
    strtab += s.name;
    strtab += '\0';
    e.n_type = s.defined ? (N_SECT | N_EXT) : (N_UNDF | N_EXT);
    e.n_sect = s.defined ? 1 : 0;
    e.n_value = s.value;
    nl.push_back(e);
  }
  while (strtab.size() % 8) strtab += '\0';

  std::vector<Reloc> rel;
  for (auto& r : relocs_) {
    // The bl keeps its opcode with a zero offset; the linker fills it in.
    uint32_t insn = 0x94000000;
    memcpy(&text[r.offset], &insn, 4);
    uint32_t sym = 0;
    for (size_t i = 0; i < all.size(); i++)
      if (all[i].name == std::string("_") + r.symbol) sym = (uint32_t)i;
    // r_symbolnum:24 r_pcrel:1 r_length:2 r_extern:1 r_type:4
    rel.push_back({(int32_t)r.offset, sym | 1u << 24 | 2u << 25 | 1u << 27 | ARM64_RELOC_BRANCH26 << 28});
  }

  const uint32_t cmds = sizeof(Segment) + sizeof(Section) + sizeof(BuildVersion) + sizeof(Symtab) + sizeof(Dysymtab);
  size_t text_off = align_up(sizeof(Header) + cmds, 16);
  size_t rel_off = align_up(text_off + text.size(), 8);
  size_t sym_off = align_up(rel_off + rel.size() * sizeof(Reloc), 8);
  size_t str_off = sym_off + nl.size() * sizeof(Nlist);
  size_t end = str_off + strtab.size();

  Header h{MH_MAGIC_64, CPU_TYPE_ARM64, 0, MH_OBJECT, 4, cmds, MH_SUBSECTIONS_VIA_SYMBOLS, 0};
  Segment seg{};
  seg.cmd = LC_SEGMENT_64;
  seg.cmdsize = sizeof(Segment) + sizeof(Section);
  seg.vmsize = seg.filesize = text.size();
  seg.fileoff = text_off;
  seg.maxprot = seg.initprot = 7;
  seg.nsects = 1;
  Section sec{};
  strcpy(sec.sectname, "__text");
  strcpy(sec.segname, "__TEXT");
  sec.size = text.size();
  sec.offset = (uint32_t)text_off;
  sec.align = 4;  // 2^4 = 16 bytes
  sec.reloff = rel.empty() ? 0 : (uint32_t)rel_off;
  sec.nreloc = (uint32_t)rel.size();
  sec.flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
  BuildVersion bv{LC_BUILD_VERSION, sizeof(BuildVersion), PLATFORM_MACOS, 11u << 16, 11u << 16, 0};
  Symtab st{LC_SYMTAB, sizeof(Symtab), (uint32_t)sym_off, (uint32_t)nl.size(), (uint32_t)str_off,
            (uint32_t)strtab.size()};
  Dysymtab dy{};
  dy.cmd = LC_DYSYMTAB;
  dy.cmdsize = sizeof(Dysymtab);
  dy.iextdefsym = 0;
  dy.nextdefsym = (uint32_t)defs.size();
  dy.iundefsym = (uint32_t)defs.size();
  dy.nundefsym = (uint32_t)undefs.size();

  std::vector<uint8_t> file(end, 0);
  size_t p = 0;
  auto put = [&](const void* src, size_t n) { memcpy(&file[p], src, n); p += n; };
  put(&h, sizeof h);
  put(&seg, sizeof seg);
  put(&sec, sizeof sec);
  put(&bv, sizeof bv);
  put(&st, sizeof st);
  put(&dy, sizeof dy);
  memcpy(&file[text_off], text.data(), text.size());
  if (!rel.empty()) memcpy(&file[rel_off], rel.data(), rel.size() * sizeof(Reloc));
  if (!nl.empty()) memcpy(&file[sym_off], nl.data(), nl.size() * sizeof(Nlist));
  memcpy(&file[str_off], strtab.data(), strtab.size());

  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) { err = "cannot open " + path; return false; }
  fwrite(file.data(), 1, file.size(), fp);
  fclose(fp);
  return true;
}

#else  // no JIT on this platform

bool jit_supported() { return false; }

JIT::JIT(VM& vm, std::vector<MFunc>& mir) : vm_(vm), mir_(mir) {}
JIT::~JIT() {}
bool JIT::collect_group(int, std::vector<char>&, std::vector<int>&) { return false; }
bool JIT::compile(int fidx) {
  vm_.funcs[fidx].jit_state = 2;
  return false;
}
void JIT::compile_all() {
  for (auto& f : vm_.funcs) f.jit_state = 2;
}
bool JIT::write_raw(const std::string&) const { return false; }
bool JIT::write_object(const std::string&, std::string& err) const {
  err = "--emit-obj needs the JIT, which is built for arm64 (Apple Silicon) only";
  return false;
}

#endif
