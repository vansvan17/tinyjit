// MIR + register assignment -> bytecode.
#include <sstream>
#include "regalloc.h"
#include "vm.h"

static uint8_t bin_op(MOp op, bool imm) {
  switch (op) {
    case MOp::Add: return imm ? B_ADDK : B_ADD;
    case MOp::Sub: return imm ? B_SUBK : B_SUB;
    case MOp::Mul: return imm ? B_MULK : B_MUL;
    case MOp::Div: return imm ? B_DIVK : B_DIV;
    case MOp::Mod: return imm ? B_MODK : B_MOD;
    case MOp::Lt: return imm ? B_LTK : B_LT;
    case MOp::Le: return imm ? B_LEK : B_LE;
    case MOp::Gt: return imm ? B_GTK : B_GT;
    case MOp::Ge: return imm ? B_GEK : B_GE;
    case MOp::Eq: return imm ? B_EQK : B_EQ;
    default: return imm ? B_NEK : B_NE;
  }
}

static uint8_t jcc_op(MOp cc, bool imm) {
  int base = imm ? B_JLTK : B_JLT;
  return (uint8_t)(base + ((int)cc - (int)MOp::Lt));
}

bool compile_bytecode(MFunc& m, VMFunc& out, std::string& err) {
  RAConfig cfg;
  cfg.vm = true;
  for (int i = 0; i < 250; i++) cfg.order.push_back(i);
  cfg.callee_saved.assign(256, true);
  RAResult ra = linear_scan(m, cfg);
  if (!ra.ok) { err = ra.error; return false; }

  out.name = m.name;
  out.nparams = m.nparams;
  out.frame_size = std::max(ra.max_reg + 1, m.nparams);
  out.max_args = m.max_args;
  if (out.frame_size + out.max_args > 255) { err = "frame too large in " + m.name; return false; }
  out.code.clear();
  out.consts.clear();

  auto R = [&](int v) { return (uint8_t)ra.reg[v]; };
  std::vector<int> block_pc(m.blocks.size());
  std::vector<std::pair<size_t, int>> fix;  // (instruction index, target block)
  auto emit = [&](uint8_t op, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, int32_t k = 0) {
    out.code.push_back({op, a, b, c, k, 0});
    return out.code.size() - 1;
  };
  auto jump_to = [&](size_t at, int block) { fix.push_back({at, block}); };

  for (size_t bi = 0; bi < m.blocks.size(); bi++) {
    block_pc[bi] = (int)out.code.size();
    int next = (int)bi + 1;
    for (auto& x : m.blocks[bi].ins) {
      switch (x.op) {
        case MOp::Param: break;
        case MOp::Const:
          if (x.imm >= INT32_MIN && x.imm <= INT32_MAX) {
            emit(B_LOADK, R(x.dst), 0, 0, (int32_t)x.imm);
          } else {
            out.consts.push_back((Value)x.imm);
            emit(B_LOADKX, R(x.dst), 0, 0, (int32_t)out.consts.size() - 1);
          }
          break;
        case MOp::Mov:
          if (R(x.dst) != R(x.a)) emit(B_MOV, R(x.dst), R(x.a));
          break;
        case MOp::Add: case MOp::Sub: case MOp::Mul: case MOp::Div: case MOp::Mod:
        case MOp::Lt: case MOp::Le: case MOp::Gt: case MOp::Ge: case MOp::Eq: case MOp::Ne:
          if (x.b_imm) emit(bin_op(x.op, true), R(x.dst), R(x.a), 0, (int32_t)x.imm);
          else emit(bin_op(x.op, false), R(x.dst), R(x.a), R(x.b));
          break;
        case MOp::Neg: emit(B_NEG, R(x.dst), R(x.a)); break;
        case MOp::Not: emit(B_NOT, R(x.dst), R(x.a)); break;
        case MOp::Car: emit(B_CAR, R(x.dst), R(x.a)); break;
        case MOp::Cdr: emit(B_CDR, R(x.dst), R(x.a)); break;
        case MOp::IsPair: emit(B_ISPAIR, R(x.dst), R(x.a)); break;
        case MOp::IsNil: emit(B_ISNIL, R(x.dst), R(x.a)); break;
        case MOp::Cons: emit(B_CONS, R(x.dst), R(x.a), R(x.b)); break;
        case MOp::Print: emit(B_PRINT, R(x.a)); break;
        case MOp::Call:
          for (size_t i = 0; i < x.args.size(); i++) emit(B_ARG, (uint8_t)i, R(x.args[i]));
          emit(B_CALL, R(x.dst), 0, 0, (int32_t)x.imm);
          break;
        case MOp::Ret: emit(B_RET, R(x.a)); break;
        case MOp::Jmp:
          if (x.t != next) jump_to(emit(B_JMP), x.t);
          break;
        case MOp::Br:
          if (x.t == next) {
            jump_to(emit(B_JF, R(x.a)), x.f);
          } else {
            jump_to(emit(B_JT, R(x.a)), x.t);
            if (x.f != next) jump_to(emit(B_JMP), x.f);
          }
          break;
        case MOp::BrCmp: {
          MOp cc = x.cc;
          int target = x.t, other = x.f;
          if (x.t == next) { cc = invert_cmp(cc); target = x.f; other = x.t; }
          size_t at = x.b_imm ? emit(jcc_op(cc, true), R(x.a), 0, 0, (int32_t)x.imm)
                              : emit(jcc_op(cc, false), R(x.a), R(x.b));
          jump_to(at, target);
          if (other != next) jump_to(emit(B_JMP), other);
          break;
        }
      }
    }
  }
  for (auto& [at, block] : fix) out.code[at].j = block_pc[block];
  return true;
}

static const char* bop_name(uint8_t op) {
  static const char* n[] = {
      "loadk", "loadkx", "mov", "add", "sub", "mul", "div", "mod", "lt", "le", "gt", "ge", "eq", "ne",
      "addk", "subk", "mulk", "divk", "modk", "ltk", "lek", "gtk", "gek", "eqk", "nek",
      "neg", "not", "cons", "car", "cdr", "is_pair", "is_nil", "print",
      "jmp", "jt", "jf", "jlt", "jle", "jgt", "jge", "jeq", "jne",
      "jltk", "jlek", "jgtk", "jgek", "jeqk", "jnek", "arg", "call", "ret"};
  return n[op];
}

std::string disasm(const VMFunc& f) {
  std::ostringstream o;
  o << "bytecode " << f.name << ": " << f.nparams << " params, " << f.frame_size << " registers, "
    << f.code.size() << " instructions\n";
  auto imm = [](int32_t k) {
    Value v = (Value)(int64_t)k;
    return is_int(v) ? std::to_string(as_int(v)) : v == NIL ? std::string("nil") : std::to_string(k);
  };
  for (size_t pc = 0; pc < f.code.size(); pc++) {
    const BIns& x = f.code[pc];
    char buf[96];
    int op = x.op;
    std::string s;
    if (op == B_LOADK) s = "r" + std::to_string(x.a) + ", " + imm(x.k);
    else if (op == B_LOADKX) s = "r" + std::to_string(x.a) + ", k" + std::to_string(x.k);
    else if (op == B_MOV || (op >= B_NEG && op <= B_ISNIL && op != B_CONS))
      s = "r" + std::to_string(x.a) + ", r" + std::to_string(x.b);
    else if (op == B_CONS || (op >= B_ADD && op <= B_NE))
      s = "r" + std::to_string(x.a) + ", r" + std::to_string(x.b) + ", r" + std::to_string(x.c);
    else if (op >= B_ADDK && op <= B_NEK)
      s = "r" + std::to_string(x.a) + ", r" + std::to_string(x.b) + ", " + imm(x.k);
    else if (op == B_PRINT || op == B_RET) s = "r" + std::to_string(x.a);
    else if (op == B_JMP) s = "-> " + std::to_string(x.j);
    else if (op == B_JT || op == B_JF) s = "r" + std::to_string(x.a) + " -> " + std::to_string(x.j);
    else if (op >= B_JLT && op <= B_JNE)
      s = "r" + std::to_string(x.a) + ", r" + std::to_string(x.b) + " -> " + std::to_string(x.j);
    else if (op >= B_JLTK && op <= B_JNEK)
      s = "r" + std::to_string(x.a) + ", " + imm(x.k) + " -> " + std::to_string(x.j);
    else if (op == B_ARG) s = "#" + std::to_string(x.a) + ", r" + std::to_string(x.b);
    else if (op == B_CALL) s = "r" + std::to_string(x.a) + ", @" + std::to_string(x.k);
    snprintf(buf, sizeof buf, "  %4zu  %-8s %s\n", pc, bop_name(x.op), s.c_str());
    o << buf;
  }
  return o.str();
}
