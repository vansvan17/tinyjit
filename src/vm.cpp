#include "vm.h"
#include "jit.h"
#include <cstdlib>
#include "runtime.h"

VM::VM() {
  stack_size = 1 << 22;
  stack = (Value*)calloc(stack_size, sizeof(Value));
  max_frames = 200000;
  frames = (Frame*)calloc(max_frames, sizeof(Frame));
}

VM::~VM() {
  free(stack);
  free(frames);
}

Value VM::call_native(VMFunc* f, Value* a) {
  using V = Value;
  void* p = f->native;
  switch (f->nparams) {
    case 0: return ((V(*)())p)();
    case 1: return ((V(*)(V))p)(a[0]);
    case 2: return ((V(*)(V, V))p)(a[0], a[1]);
    case 3: return ((V(*)(V, V, V))p)(a[0], a[1], a[2]);
    case 4: return ((V(*)(V, V, V, V))p)(a[0], a[1], a[2], a[3]);
    case 5: return ((V(*)(V, V, V, V, V))p)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((V(*)(V, V, V, V, V, V))p)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7: return ((V(*)(V, V, V, V, V, V, V))p)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default: return ((V(*)(V, V, V, V, V, V, V, V))p)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

void VM::maybe_jit(int fidx) {
  if (jit) jit->compile(fidx);
}

Value VM::run(int fidx) {
  VMFunc* f = &funcs[fidx];
  if (f->native) return call_native(f, stack);
  return goto_dispatch ? run_goto(*this, fidx) : run_switch(*this, fidx);
}

#define INTERP_NAME run_switch
#define INTERP_GOTO 0
#include "interp.inc"
#undef INTERP_NAME
#undef INTERP_GOTO

#define INTERP_NAME run_goto
#define INTERP_GOTO 1
#include "interp.inc"
#undef INTERP_NAME
#undef INTERP_GOTO
