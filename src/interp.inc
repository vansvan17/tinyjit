Value INTERP_NAME(VM& vm, int entry) {
  VMFunc* fn = &vm.funcs[entry];
  Value* base = vm.stack;
  Value* const stack_end = vm.stack + vm.stack_size;
  Frame* frames = vm.frames;
  const int max_depth = vm.max_frames;
  int depth = 0;
  const BIns* pc = fn->code.data();
  Value x, y;

  for (int i = 0; i < fn->frame_size + fn->max_args; i++) base[i] = 0;

#define R(i) base[i]
#define TYPE_ERROR() runtime_error("type error")

#if INTERP_GOTO
  static const void* labels[B_COUNT] = {
      &&L_B_LOADK, &&L_B_LOADKX, &&L_B_MOV,
      &&L_B_ADD, &&L_B_SUB, &&L_B_MUL, &&L_B_DIV, &&L_B_MOD, &&L_B_LT, &&L_B_LE, &&L_B_GT, &&L_B_GE, &&L_B_EQ, &&L_B_NE,
      &&L_B_ADDK, &&L_B_SUBK, &&L_B_MULK, &&L_B_DIVK, &&L_B_MODK, &&L_B_LTK, &&L_B_LEK, &&L_B_GTK, &&L_B_GEK, &&L_B_EQK, &&L_B_NEK,
      &&L_B_NEG, &&L_B_NOT, &&L_B_CONS, &&L_B_CAR, &&L_B_CDR, &&L_B_ISPAIR, &&L_B_ISNIL, &&L_B_PRINT,
      &&L_B_JMP, &&L_B_JT, &&L_B_JF,
      &&L_B_JLT, &&L_B_JLE, &&L_B_JGT, &&L_B_JGE, &&L_B_JEQ, &&L_B_JNE,
      &&L_B_JLTK, &&L_B_JLEK, &&L_B_JGTK, &&L_B_JGEK, &&L_B_JEQK, &&L_B_JNEK,
      &&L_B_ARG, &&L_B_CALL, &&L_B_RET,
  };
#define CASE(op) L_##op:
#define DISPATCH() goto *labels[pc->op]
#define NEXT() { ++pc; DISPATCH(); }
  DISPATCH();
  {
#else
#define CASE(op) case op:
#define DISPATCH() continue
#define NEXT() { ++pc; continue; }
  for (;;) {
    switch (pc->op) {
#endif
#define JUMP_IF(cond) { if (cond) { pc = fn->code.data() + pc->j; DISPATCH(); } NEXT(); }
#define ARITH(op_, expr_)                          \
  CASE(op_) {                                      \
    x = R(pc->b); y = R(pc->c);                    \
    if ((x | y) & 1) TYPE_ERROR();                 \
    R(pc->a) = (expr_);                            \
    NEXT();                                        \
  }
#define ARITHK(op_, expr_)                         \
  CASE(op_) {                                      \
    x = R(pc->b); y = (Value)(int64_t)pc->k;       \
    if (x & 1) TYPE_ERROR();                       \
    R(pc->a) = (expr_);                            \
    NEXT();                                        \
  }
#define CMPJ(op_, cmp_, yexpr_, check_)            \
  CASE(op_) {                                      \
    x = R(pc->a); y = (yexpr_);                    \
    if (check_ && ((x | y) & 1)) TYPE_ERROR();     \
    JUMP_IF((int64_t)x cmp_ (int64_t)y);           \
  }

    CASE(B_LOADK) R(pc->a) = (Value)(int64_t)pc->k; NEXT();
    CASE(B_LOADKX) R(pc->a) = fn->consts[pc->k]; NEXT();
    CASE(B_MOV) R(pc->a) = R(pc->b); NEXT();

    ARITH(B_ADD, x + y)
    ARITH(B_SUB, x - y)
    ARITH(B_MUL, (uint64_t)as_int(x) * y)
    CASE(B_DIV) CASE(B_MOD) {
      x = R(pc->b); y = R(pc->c);
      if ((x | y) & 1) TYPE_ERROR();
      if (y == 0) runtime_error("division by zero");
      R(pc->a) = pc->op == B_DIV ? mk_int(as_int(x) / as_int(y)) : mk_int(as_int(x) % as_int(y));
      NEXT();
    }
    ARITH(B_LT, mk_int((int64_t)x < (int64_t)y))
    ARITH(B_LE, mk_int((int64_t)x <= (int64_t)y))
    ARITH(B_GT, mk_int((int64_t)x > (int64_t)y))
    ARITH(B_GE, mk_int((int64_t)x >= (int64_t)y))
    CASE(B_EQ) R(pc->a) = mk_int(R(pc->b) == R(pc->c)); NEXT();
    CASE(B_NE) R(pc->a) = mk_int(R(pc->b) != R(pc->c)); NEXT();

    ARITHK(B_ADDK, x + y)
    ARITHK(B_SUBK, x - y)
    ARITHK(B_MULK, (uint64_t)as_int(x) * y)
    ARITHK(B_DIVK, mk_int(as_int(x) / as_int(y)))
    ARITHK(B_MODK, mk_int(as_int(x) % as_int(y)))
    ARITHK(B_LTK, mk_int((int64_t)x < (int64_t)y))
    ARITHK(B_LEK, mk_int((int64_t)x <= (int64_t)y))
    ARITHK(B_GTK, mk_int((int64_t)x > (int64_t)y))
    ARITHK(B_GEK, mk_int((int64_t)x >= (int64_t)y))
    CASE(B_EQK) R(pc->a) = mk_int(R(pc->b) == (Value)(int64_t)pc->k); NEXT();
    CASE(B_NEK) R(pc->a) = mk_int(R(pc->b) != (Value)(int64_t)pc->k); NEXT();

    CASE(B_NEG) {
      x = R(pc->b);
      if (x & 1) TYPE_ERROR();
      R(pc->a) = (Value)0 - x;
      NEXT();
    }
    CASE(B_NOT) R(pc->a) = mk_int(R(pc->b) == 0); NEXT();
    CASE(B_CONS) {
      vm.heap.roots_begin = vm.stack;
      vm.heap.roots_end = base + fn->frame_size + fn->max_args;
      R(pc->a) = vm.heap.alloc(R(pc->b), R(pc->c));
      NEXT();
    }
    CASE(B_CAR) {
      x = R(pc->b);
      if (!is_pair(x)) TYPE_ERROR();
      R(pc->a) = as_pair(x)->car;
      NEXT();
    }
    CASE(B_CDR) {
      x = R(pc->b);
      if (!is_pair(x)) TYPE_ERROR();
      R(pc->a) = as_pair(x)->cdr;
      NEXT();
    }
    CASE(B_ISPAIR) R(pc->a) = mk_int(is_pair(R(pc->b))); NEXT();
    CASE(B_ISNIL) R(pc->a) = mk_int(R(pc->b) == NIL); NEXT();
    CASE(B_PRINT) rt_print(R(pc->a)); NEXT();

    CASE(B_JMP) pc = fn->code.data() + pc->j; DISPATCH();
    CASE(B_JT) JUMP_IF(R(pc->a) != 0);
    CASE(B_JF) JUMP_IF(R(pc->a) == 0);
    CMPJ(B_JLT, <, R(pc->b), true)
    CMPJ(B_JLE, <=, R(pc->b), true)
    CMPJ(B_JGT, >, R(pc->b), true)
    CMPJ(B_JGE, >=, R(pc->b), true)
    CMPJ(B_JEQ, ==, R(pc->b), false)
    CMPJ(B_JNE, !=, R(pc->b), false)
    CMPJ(B_JLTK, <, (Value)(int64_t)pc->k, true)
    CMPJ(B_JLEK, <=, (Value)(int64_t)pc->k, true)
    CMPJ(B_JGTK, >, (Value)(int64_t)pc->k, true)
    CMPJ(B_JGEK, >=, (Value)(int64_t)pc->k, true)
    CMPJ(B_JEQK, ==, (Value)(int64_t)pc->k, false)
    CMPJ(B_JNEK, !=, (Value)(int64_t)pc->k, false)

    CASE(B_ARG) base[fn->frame_size + pc->a] = R(pc->b); NEXT();
    CASE(B_CALL) {
      int ci = pc->k;
      VMFunc* callee = &vm.funcs[ci];
      Value* nb = base + fn->frame_size;
      if (!callee->native && vm.jit && ++callee->calls >= vm.jit_threshold && callee->jit_state == 0)
        vm.maybe_jit(ci);
      if (callee->native) {
        R(pc->a) = vm.call_native(callee, nb);
        NEXT();
      }
      if (depth + 1 >= max_depth || nb + callee->frame_size + callee->max_args > stack_end)
        runtime_error("stack overflow");
      frames[depth++] = {fn, pc, base};
      fn = callee;
      base = nb;
      pc = fn->code.data();
      if (vm.clear_frames)
        for (int i = fn->nparams; i < fn->frame_size + fn->max_args; i++) base[i] = 0;
      DISPATCH();
    }
    CASE(B_RET) {
      Value v = R(pc->a);
      if (depth == 0) return v;
      Frame& f = frames[--depth];
      fn = f.fn;
      pc = f.pc;
      base = f.base;
      R(pc->a) = v;
      NEXT();
    }
#if !INTERP_GOTO
      default: __builtin_unreachable();  // NOLINT
    }
#endif
  }
  __builtin_unreachable();
#undef R
#undef TYPE_ERROR
#undef CASE
#undef DISPATCH
#undef NEXT
#undef JUMP_IF
#undef ARITH
#undef ARITHK
#undef CMPJ
}
