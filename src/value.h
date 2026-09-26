// Tagged values and the semantics of every primitive operation.
//
// A Value is 64 bits:
//   int:  n << 1          (low bit 0), so ints are 63-bit and wrap on overflow
//   pair: address | 1     (low bit 1, cells are 8-byte aligned)
//   nil:  1               (the "null pair")
//
// Tagged ints make add/sub/compare work directly on the raw bits, which is
// what lets the JIT emit a single `add` after a one-instruction type guard.
// The interpreter, the constant folder and the JIT must all agree on these
// rules; this file is the single source of truth for the first two, and the
// JIT is checked against it by the differential tests.
#pragma once
#include <cstdint>

using Value = uint64_t;

constexpr Value NIL = 1;

inline Value mk_int(int64_t n) { return (Value)((uint64_t)n << 1); }
inline int64_t as_int(Value v) { return (int64_t)v >> 1; }
inline bool is_int(Value v) { return (v & 1) == 0; }
inline bool is_pair(Value v) { return (v & 1) && v != NIL; }

struct Pair {
  uint64_t header;  // bit 0 = mark, bit 1 = on free list
  Value car;
  Value cdr;
};
inline Pair* as_pair(Value v) { return (Pair*)(v - 1); }
inline Value mk_pair(Pair* p) { return (Value)p | 1; }

// Offsets the JIT uses to load car/cdr straight from a tagged pointer.
constexpr int32_t CAR_OFFSET = 8 - 1;
constexpr int32_t CDR_OFFSET = 16 - 1;

enum class Trap { None, Type, DivZero };

enum class BinOp { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };

// Evaluate a binary op. Type checks happen before the zero check so that the
// interpreter and JIT report the same error for `nil / 0`.
inline Trap eval_binop(BinOp op, Value a, Value b, Value& out) {
  if (op == BinOp::Eq) { out = mk_int(a == b); return Trap::None; }
  if (op == BinOp::Ne) { out = mk_int(a != b); return Trap::None; }
  if (!is_int(a) || !is_int(b)) return Trap::Type;
  switch (op) {
    case BinOp::Add: out = a + b; return Trap::None;
    case BinOp::Sub: out = a - b; return Trap::None;
    case BinOp::Mul: out = (uint64_t)as_int(a) * b; return Trap::None;
    case BinOp::Div:
      if (b == 0) return Trap::DivZero;
      out = mk_int(as_int(a) / as_int(b));
      return Trap::None;
    case BinOp::Mod:
      if (b == 0) return Trap::DivZero;
      out = mk_int(as_int(a) % as_int(b));
      return Trap::None;
    case BinOp::Lt: out = mk_int((int64_t)a < (int64_t)b); return Trap::None;
    case BinOp::Le: out = mk_int((int64_t)a <= (int64_t)b); return Trap::None;
    case BinOp::Gt: out = mk_int((int64_t)a > (int64_t)b); return Trap::None;
    case BinOp::Ge: out = mk_int((int64_t)a >= (int64_t)b); return Trap::None;
    default: return Trap::Type;
  }
}

inline Trap eval_neg(Value a, Value& out) {
  if (!is_int(a)) return Trap::Type;
  out = (Value)0 - a;
  return Trap::None;
}

// Truthiness: only the integer 0 is false. nil and pairs are true.
inline Value eval_not(Value a) { return mk_int(a == 0); }
