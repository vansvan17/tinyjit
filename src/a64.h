#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

enum XReg : uint8_t {
  X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15,
  X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28,
  FP = 29,
  LR = 30,
  XZR = 31,
};
constexpr uint8_t SP = 31;

enum Cond : uint8_t { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

struct A64 {
  std::vector<uint32_t> w;

  size_t pos() const { return w.size(); }
  void emit(uint32_t x) { w.push_back(x); }
  void quad(uint64_t v) {
    emit((uint32_t)v);
    emit((uint32_t)(v >> 32));
  }

  void mov(XReg d, XReg s) {
    if (d != s) emit(0xAA0003E0 | s << 16 | d);
  }
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
        if (inverted) emit(0x92800000 | i << 21 | (uint16_t)~h << 5 | d);
        else emit(0xD2800000 | i << 21 | h << 5 | d);
        first = false;
      } else {
        emit(0xF2800000 | i << 21 | h << 5 | d);
      }
    }
    if (first) {
      if (inverted) emit(0x92800000 | d);
      else emit(0xD2800000 | d);
    }
  }

  void add(XReg d, XReg n, XReg m) { emit(0x8B000000 | m << 16 | n << 5 | d); }
  void sub(XReg d, XReg n, XReg m) { emit(0xCB000000 | m << 16 | n << 5 | d); }
  void and_(XReg d, XReg n, XReg m) { emit(0x8A000000 | m << 16 | n << 5 | d); }
  void orr(XReg d, XReg n, XReg m) { emit(0xAA000000 | m << 16 | n << 5 | d); }
  void cmp(XReg n, XReg m) { emit(0xEB00001F | m << 16 | n << 5); }
  void neg(XReg d, XReg m) { emit(0xCB0003E0 | m << 16 | d); }
  void mul(XReg d, XReg n, XReg m) { emit(0x9B007C00 | m << 16 | n << 5 | d); }
  void sdiv(XReg d, XReg n, XReg m) { emit(0x9AC00C00 | m << 16 | n << 5 | d); }
  void msub(XReg d, XReg n, XReg m, XReg a) { emit(0x9B008000 | m << 16 | a << 10 | n << 5 | d); }

  static bool imm12(int64_t v) { return v >= 0 && v < 4096; }
  void add_imm(XReg d, XReg n, uint32_t imm) { emit(0x91000000 | imm << 10 | n << 5 | d); }
  void sub_imm(XReg d, XReg n, uint32_t imm) { emit(0xD1000000 | imm << 10 | n << 5 | d); }
  void add_any(XReg d, XReg n, int64_t v, XReg tmp) {
    if (imm12(v)) add_imm(d, n, (uint32_t)v);
    else if (imm12(-v)) sub_imm(d, n, (uint32_t)-v);
    else { movi(tmp, v); add(d, n, tmp); }
  }
  void cmp_any(XReg n, int64_t v, XReg tmp) {
    if (imm12(v)) emit(0xF100001F | (uint32_t)v << 10 | n << 5);
    else if (imm12(-v)) emit(0xB100001F | (uint32_t)-v << 10 | n << 5);
    else { movi(tmp, v); cmp(n, tmp); }
  }

  void tst1(XReg n) { emit(0xF240001F | n << 5); }
  void and1(XReg d, XReg n) { emit(0x92400000 | n << 5 | d); }

  void asr(XReg d, XReg n, unsigned sh) { emit(0x9340FC00 | sh << 16 | n << 5 | d); }
  void lsr(XReg d, XReg n, unsigned sh) { emit(0xD340FC00 | sh << 16 | n << 5 | d); }
  void lsl(XReg d, XReg n, unsigned sh) {
    emit(0xD3400000 | ((64 - sh) & 63) << 16 | (63 - sh) << 10 | n << 5 | d);
  }
  void cset(XReg d, Cond c) { emit(0x9A9F07E0 | (c ^ 1) << 12 | d); }

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

  void push_fp_lr() { emit(0xA9BF7BFD); }
  void pop_fp_lr() { emit(0xA8C17BFD); }
  void mov_fp_sp() { emit(0x910003FD); }
  void mov_sp_fp() { emit(0x910003BF); }
  void sub_sp(uint32_t n) {
    while (n) {
      uint32_t k = n > 4080 ? 4080 : n;
      sub_imm((XReg)SP, (XReg)SP, k);
      n -= k;
    }
  }

  size_t b() { emit(0x14000000); return pos() - 1; }
  size_t bl() { emit(0x94000000); return pos() - 1; }
  size_t b_cond(Cond c) { emit(0x54000000 | c); return pos() - 1; }
  size_t cbz(XReg t) { emit(0xB4000000 | t); return pos() - 1; }
  size_t cbnz(XReg t) { emit(0xB5000000 | t); return pos() - 1; }
  void br(XReg n) { emit(0xD61F0000 | n << 5); }
  void blr(XReg n) { emit(0xD63F0000 | n << 5); }
  void ret() { emit(0xD65F03C0); }
  void ldr_literal(XReg t, int words_ahead) { emit(0x58000000 | (uint32_t)words_ahead << 5 | t); }

  bool patch(size_t at, size_t target) {
    int64_t d = (int64_t)target - (int64_t)at;
    uint32_t& x = w[at];
    if ((x & 0x7C000000) == 0x14000000) {
      if (d < -(1 << 25) || d >= (1 << 25)) return false;
      x = (x & 0xFC000000) | ((uint32_t)d & 0x03FFFFFF);
    } else {
      if (d < -(1 << 18) || d >= (1 << 18)) return false;
      x = (x & 0xFF00001F) | ((uint32_t)d & 0x7FFFF) << 5;
    }
    return true;
  }
};
