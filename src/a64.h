// A small AArch64 (ARM64) assembler: just the encodings the JIT needs.
//
// Every A64 instruction is exactly 32 bits, which makes the encoder much
// simpler than x86's: no prefixes, no variable lengths. The price is that
// immediates are small and oddly shaped (12-bit add/sub immediates, 16-bit
// move-wide chunks, the "bitmask immediate" scheme for logical ops), so
// large constants take several instructions to build.
//
// Register 31 means SP in some instructions (add/sub immediate, loads and
// stores) and XZR, the zero register, in others (most register-register
// forms). `cmp a, b` is really `subs xzr, a, b`, `mov a, b` is really
// `orr a, xzr, b`, and so on; the comments show the underlying instruction.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

enum XReg : uint8_t {
  X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15,
  X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28,
  FP = 29,  // x29, frame pointer
  LR = 30,  // x30, link register (return address)
  XZR = 31, // zero register / SP depending on the instruction
};
constexpr uint8_t SP = 31;

// Condition codes. c ^ 1 is the negation.
enum Cond : uint8_t { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

struct A64 {
  std::vector<uint32_t> w;  // instruction words

  size_t pos() const { return w.size(); }  // in instructions
  void emit(uint32_t x) { w.push_back(x); }
  void quad(uint64_t v) {  // 8 bytes of data inline (for literal pools)
    emit((uint32_t)v);
    emit((uint32_t)(v >> 32));
  }

  // ---- moves and immediates ----
  void mov(XReg d, XReg s) {  // orr d, xzr, s
    if (d != s) emit(0xAA0003E0 | s << 16 | d);
  }
  // Build any 64-bit constant with movz/movn + movk: one instruction per
  // 16-bit chunk that differs from the background (all 0s or all 1s).
  void movi(XReg d, int64_t v) {
    uint64_t u = (uint64_t)v;
    int zeros = 0, ones = 0;
    for (int i = 0; i < 4; i++) {
      uint16_t h = (uint16_t)(u >> (16 * i));
      zeros += h == 0;
      ones += h == 0xFFFF;
    }
    bool inverted = ones > zeros;
    uint16_t bg = inverted ? 0xFFFF : 0;
    bool first = true;
    for (int i = 0; i < 4; i++) {
      uint16_t h = (uint16_t)(u >> (16 * i));
      if (h == bg) continue;
      if (first) {
        if (inverted) emit(0x92800000 | i << 21 | (uint16_t)~h << 5 | d);  // movn
        else emit(0xD2800000 | i << 21 | h << 5 | d);                      // movz
        first = false;
      } else {
        emit(0xF2800000 | i << 21 | h << 5 | d);  // movk
      }
    }
    if (first) {  // all chunks equal the background: 0 or -1
      if (inverted) emit(0x92800000 | d);  // movn d, #0  -> -1
      else emit(0xD2800000 | d);           // movz d, #0
    }
  }

  // ---- arithmetic, register forms ----
  void add(XReg d, XReg n, XReg m) { emit(0x8B000000 | m << 16 | n << 5 | d); }
  void sub(XReg d, XReg n, XReg m) { emit(0xCB000000 | m << 16 | n << 5 | d); }
  void and_(XReg d, XReg n, XReg m) { emit(0x8A000000 | m << 16 | n << 5 | d); }
  void orr(XReg d, XReg n, XReg m) { emit(0xAA000000 | m << 16 | n << 5 | d); }
  void cmp(XReg n, XReg m) { emit(0xEB00001F | m << 16 | n << 5); }  // subs xzr, n, m
  void neg(XReg d, XReg m) { emit(0xCB0003E0 | m << 16 | d); }       // sub d, xzr, m
  void mul(XReg d, XReg n, XReg m) { emit(0x9B007C00 | m << 16 | n << 5 | d); }  // madd d, n, m, xzr
  void sdiv(XReg d, XReg n, XReg m) { emit(0x9AC00C00 | m << 16 | n << 5 | d); }
  void msub(XReg d, XReg n, XReg m, XReg a) { emit(0x9B008000 | m << 16 | a << 10 | n << 5 | d); }  // d = a - n*m

  // ---- arithmetic, immediate forms (12-bit unsigned, optionally << 12) ----
  static bool imm12(int64_t v) { return v >= 0 && v < 4096; }
  void add_imm(XReg d, XReg n, uint32_t imm) { emit(0x91000000 | imm << 10 | n << 5 | d); }
  void sub_imm(XReg d, XReg n, uint32_t imm) { emit(0xD1000000 | imm << 10 | n << 5 | d); }
  // d = n + v for any v; uses `tmp` when v does not fit an immediate.
  void add_any(XReg d, XReg n, int64_t v, XReg tmp) {
    if (imm12(v)) add_imm(d, n, (uint32_t)v);
    else if (imm12(-v)) sub_imm(d, n, (uint32_t)-v);
    else { movi(tmp, v); add(d, n, tmp); }
  }
  void cmp_any(XReg n, int64_t v, XReg tmp) {
    if (imm12(v)) emit(0xF100001F | (uint32_t)v << 10 | n << 5);        // subs xzr, n, #v
    else if (imm12(-v)) emit(0xB100001F | (uint32_t)-v << 10 | n << 5); // adds xzr, n, #-v (cmn)
    else { movi(tmp, v); cmp(n, tmp); }
  }

  // ---- bit tricks with the constant 1 (a "bitmask immediate": N=1, immr=0, imms=0) ----
  void tst1(XReg n) { emit(0xF240001F | n << 5); }             // ands xzr, n, #1
  void and1(XReg d, XReg n) { emit(0x92400000 | n << 5 | d); }  // and d, n, #1

  // ---- shifts (bitfield moves) ----
  void asr(XReg d, XReg n, unsigned sh) { emit(0x9340FC00 | sh << 16 | n << 5 | d); }  // sbfm d, n, #sh, #63
  void lsr(XReg d, XReg n, unsigned sh) { emit(0xD340FC00 | sh << 16 | n << 5 | d); }  // ubfm d, n, #sh, #63
  void lsl(XReg d, XReg n, unsigned sh) {                                              // ubfm d, n, #-sh, #63-sh
    emit(0xD3400000 | ((64 - sh) & 63) << 16 | (63 - sh) << 10 | n << 5 | d);
  }
  void cset(XReg d, Cond c) { emit(0x9A9F07E0 | (c ^ 1) << 12 | d); }  // csinc d, xzr, xzr, !c

  // ---- loads and stores, [base + off] ----
  // ldur/stur take a signed 9-bit byte offset; beyond that, ldr/str take an
  // unsigned 12-bit offset scaled by 8; beyond that, compute the address.
  void ldr(XReg t, uint8_t base, int32_t off, XReg tmp = X17) { mem(0xF8400000, 0xF9400000, t, base, off, tmp); }
  void str(XReg t, uint8_t base, int32_t off, XReg tmp = X17) { mem(0xF8000000, 0xF9000000, t, base, off, tmp); }
  void mem(uint32_t unscaled, uint32_t scaled, XReg t, uint8_t base, int32_t off, XReg tmp) {
    if (off >= -256 && off <= 255) {
      emit(unscaled | ((uint32_t)off & 0x1FF) << 12 | base << 5 | t);
    } else if (off >= 0 && off % 8 == 0 && off / 8 < 4096) {
      emit(scaled | (uint32_t)(off / 8) << 10 | base << 5 | t);
    } else {
      movi(tmp, off);
      add(tmp, (XReg)base, tmp);
      emit(scaled | tmp << 5 | t);
    }
  }

  // ---- frame ----
  void push_fp_lr() { emit(0xA9BF7BFD); }  // stp x29, x30, [sp, #-16]!
  void pop_fp_lr() { emit(0xA8C17BFD); }   // ldp x29, x30, [sp], #16
  void mov_fp_sp() { emit(0x910003FD); }   // add x29, sp, #0
  void mov_sp_fp() { emit(0x910003BF); }   // add sp, x29, #0
  void sub_sp(uint32_t n) {                // n is a multiple of 16
    while (n) {
      uint32_t k = n > 4080 ? 4080 : n;
      sub_imm((XReg)SP, (XReg)SP, k);
      n -= k;
    }
  }

  // ---- control flow. Branch offsets are in instructions and patched later. ----
  size_t b() { emit(0x14000000); return pos() - 1; }
  size_t bl() { emit(0x94000000); return pos() - 1; }
  size_t b_cond(Cond c) { emit(0x54000000 | c); return pos() - 1; }
  size_t cbz(XReg t) { emit(0xB4000000 | t); return pos() - 1; }
  size_t cbnz(XReg t) { emit(0xB5000000 | t); return pos() - 1; }
  void br(XReg n) { emit(0xD61F0000 | n << 5); }
  void blr(XReg n) { emit(0xD63F0000 | n << 5); }
  void ret() { emit(0xD65F03C0); }
  void ldr_literal(XReg t, int words_ahead) { emit(0x58000000 | (uint32_t)words_ahead << 5 | t); }

  // Point the branch at `at` to instruction index `target`. Returns false if
  // the distance does not fit (b.cond/cbz reach +-1 MB, b/bl +-128 MB).
  bool patch(size_t at, size_t target) {
    int64_t d = (int64_t)target - (int64_t)at;
    uint32_t& x = w[at];
    if ((x & 0x7C000000) == 0x14000000) {  // b, bl: imm26
      if (d < -(1 << 25) || d >= (1 << 25)) return false;
      x = (x & 0xFC000000) | ((uint32_t)d & 0x03FFFFFF);
    } else {  // b.cond, cbz, cbnz: imm19 at bit 5
      if (d < -(1 << 18) || d >= (1 << 18)) return false;
      x = (x & 0xFF00001F) | ((uint32_t)d & 0x7FFFF) << 5;
    }
    return true;
  }
};
