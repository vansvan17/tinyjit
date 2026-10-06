#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gc.h"
#include "mir.h"
#include "value.h"

enum BOp : uint8_t {
  B_LOADK, B_LOADKX, B_MOV,
  B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_LT, B_LE, B_GT, B_GE, B_EQ, B_NE,
  B_ADDK, B_SUBK, B_MULK, B_DIVK, B_MODK, B_LTK, B_LEK, B_GTK, B_GEK, B_EQK, B_NEK,
  B_NEG, B_NOT, B_CONS, B_CAR, B_CDR, B_ISPAIR, B_ISNIL, B_PRINT,
  B_JMP, B_JT, B_JF,
  B_JLT, B_JLE, B_JGT, B_JGE, B_JEQ, B_JNE,
  B_JLTK, B_JLEK, B_JGTK, B_JGEK, B_JEQK, B_JNEK,
  B_ARG, B_CALL, B_RET,
  B_COUNT
};

struct BIns {
  uint8_t op, a, b, c;
  int32_t k;
  int32_t j;
};

struct VMFunc {
  std::string name;
  int nparams = 0;
  int frame_size = 0;
  int max_args = 0;
  std::vector<BIns> code;
  std::vector<Value> consts;
  uint64_t calls = 0;
  void* native = nullptr;
  int jit_state = 0;
};

struct Frame {
  VMFunc* fn;
  const BIns* pc;
  Value* base;
};

class JIT;

struct VM {
  VM();
  ~VM();
  std::vector<VMFunc> funcs;
  std::vector<MFunc>* mir = nullptr;
  Heap heap;
  Value* stack = nullptr;
  size_t stack_size = 0;
  Frame* frames = nullptr;
  int max_frames = 0;
  bool goto_dispatch = true;
  bool clear_frames = false;
  JIT* jit = nullptr;
  uint64_t jit_threshold = 100;

  Value run(int fidx);
  Value call_native(VMFunc* f, Value* args);
  void maybe_jit(int fidx);
};

bool compile_bytecode(MFunc& m, VMFunc& out, std::string& err);
std::string disasm(const VMFunc& f);

Value run_switch(VM& vm, int fidx);
Value run_goto(VM& vm, int fidx);
