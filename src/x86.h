// A tiny x86-64 assembler: just the encodings the JIT needs.
//
// Refresher on the encoding used below:
//   [REX] opcode ModRM [SIB] [disp] [imm]
//   REX = 0100WRXB. W=1 selects 64-bit operand size; R, X, B supply the 4th
//         bit of the ModRM.reg, SIB.index and ModRM.rm/base register numbers
//         (that is how r8-r15 are reached).
//   ModRM = mod(2) reg(3) rm(3). mod=11 means rm is a register; mod=10 means
//         memory at [rm + disp32]. rm=100 (rsp/r12) would need a SIB byte;
//         the JIT only ever addresses memory through rbp and rax, so it never
//         emits one.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

enum Reg : uint8_t { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

// Condition codes, as the low nibble of Jcc/SETcc. cc ^ 1 is the negation.
enum Cond : uint8_t { CC_E = 0x4, CC_NE = 0x5, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

inline const char* reg_name(int r) {
  static const char* n[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                            "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  return n[r];
}

struct Asm {
  std::vector<uint8_t> b;

  size_t pos() const { return b.size(); }
  void u8(uint8_t x) { b.push_back(x); }
  void u32(uint32_t x) { for (int i = 0; i < 4; i++) u8((uint8_t)(x >> (8 * i))); }
  void u64(uint64_t x) { for (int i = 0; i < 8; i++) u8((uint8_t)(x >> (8 * i))); }

  void rex(bool w, int r, int rm) {
    uint8_t v = 0x40 | (w << 3) | (((r >> 3) & 1) << 2) | ((rm >> 3) & 1);
    if (v != 0x40) u8(v);
  }
  void modrm(int mod, int reg, int rm) { u8((uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7))); }

  // op r/m64, r64   (dst in rm, src in reg)
  void rr(uint8_t opc, Reg dst, Reg src) { rex(1, src, dst); u8(opc); modrm(3, src, dst); }
  void mov(Reg d, Reg s) { if (d != s) rr(0x89, d, s); }
  void add(Reg d, Reg s) { rr(0x01, d, s); }
  void sub(Reg d, Reg s) { rr(0x29, d, s); }
  void and_(Reg d, Reg s) { rr(0x21, d, s); }
  void or_(Reg d, Reg s) { rr(0x09, d, s); }
  void cmp(Reg a, Reg b) { rr(0x39, a, b); }  // flags from a - b
  void test(Reg a, Reg b) { rr(0x85, a, b); }
  void imul(Reg d, Reg s) { rex(1, d, s); u8(0x0F); u8(0xAF); modrm(3, d, s); }
  void imul_imm(Reg d, Reg s, int32_t imm) { rex(1, d, s); u8(0x69); modrm(3, d, s); u32((uint32_t)imm); }

  // Group-1 ALU with imm32: /0 add, /1 or, /4 and, /5 sub, /7 cmp
  void alu_imm(int ext, Reg d, int32_t imm) { rex(1, 0, d); u8(0x81); modrm(3, ext, d); u32((uint32_t)imm); }
  void add_imm(Reg d, int32_t imm) { alu_imm(0, d, imm); }
  void sub_imm(Reg d, int32_t imm) { alu_imm(5, d, imm); }
  void and_imm(Reg d, int32_t imm) { alu_imm(4, d, imm); }
  void cmp_imm(Reg d, int32_t imm) { alu_imm(7, d, imm); }
  void test_imm(Reg d, int32_t imm) { rex(1, 0, d); u8(0xF7); modrm(3, 0, d); u32((uint32_t)imm); }

  void mov_imm(Reg d, int64_t imm) {
    if (imm == (int32_t)imm) {  // sign-extended imm32: 7 bytes instead of 10
      rex(1, 0, d); u8(0xC7); modrm(3, 0, d); u32((uint32_t)imm);
    } else {
      mov_imm64(d, (uint64_t)imm);
    }
  }
  // movabs; returns the offset of the 8-byte immediate (for relocations)
  size_t mov_imm64(Reg d, uint64_t imm) {
    rex(1, 0, d); u8((uint8_t)(0xB8 + (d & 7)));
    size_t at = pos();
    u64(imm);
    return at;
  }

  void shl(Reg d, uint8_t n) { rex(1, 0, d); u8(0xC1); modrm(3, 4, d); u8(n); }
  void sar(Reg d, uint8_t n) { rex(1, 0, d); u8(0xC1); modrm(3, 7, d); u8(n); }
  void shr(Reg d, uint8_t n) { rex(1, 0, d); u8(0xC1); modrm(3, 5, d); u8(n); }
  void neg(Reg d) { rex(1, 0, d); u8(0xF7); modrm(3, 3, d); }
  void idiv(Reg s) { rex(1, 0, s); u8(0xF7); modrm(3, 7, s); }
  void cqo() { u8(0x48); u8(0x99); }
  void setcc(Cond c, Reg d8) { u8(0x0F); u8((uint8_t)(0x90 | c)); modrm(3, 0, d8); }  // al, cl, dl, bl only
  void movzx8(Reg d, Reg s8) { u8(0x0F); u8(0xB6); modrm(3, d, s8); }                 // 32-bit dest

  // [base + disp32]
  void mem(int reg, Reg base, int32_t disp) { modrm(2, reg, base); u32((uint32_t)disp); }
  void load(Reg d, Reg base, int32_t disp) { rex(1, d, base); u8(0x8B); mem(d, base, disp); }
  void store(Reg base, int32_t disp, Reg s) { rex(1, s, base); u8(0x89); mem(s, base, disp); }
  void store_imm(Reg base, int32_t disp, int32_t imm) { rex(1, 0, base); u8(0xC7); mem(0, base, disp); u32((uint32_t)imm); }

  size_t jcc(Cond c) { u8(0x0F); u8((uint8_t)(0x80 | c)); size_t at = pos(); u32(0); return at; }
  size_t jmp() { u8(0xE9); size_t at = pos(); u32(0); return at; }
  size_t call_rel() { u8(0xE8); size_t at = pos(); u32(0); return at; }
  void call_reg(Reg r) { rex(0, 0, r); u8(0xFF); modrm(3, 2, r); }
  void ret() { u8(0xC3); }
  void push(Reg r) { rex(0, 0, r); u8((uint8_t)(0x50 + (r & 7))); }
  void pop(Reg r) { rex(0, 0, r); u8((uint8_t)(0x58 + (r & 7))); }

  void patch_rel32(size_t at, size_t target) {
    int32_t rel = (int32_t)((int64_t)target - (int64_t)(at + 4));
    memcpy(&b[at], &rel, 4);
  }
};
