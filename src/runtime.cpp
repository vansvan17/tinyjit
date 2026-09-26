#include "runtime.h"
#include <cstdlib>

void print_value(FILE* out, Value v) {
  if (is_int(v)) { fprintf(out, "%lld", (long long)as_int(v)); return; }
  if (v == NIL) { fputs("nil", out); return; }
  // Lists print as (1 2 3), improper tails as (1 2 . 3).
  fputc('(', out);
  bool first = true;
  while (is_pair(v)) {
    if (!first) fputc(' ', out);
    first = false;
    print_value(out, as_pair(v)->car);
    v = as_pair(v)->cdr;
  }
  if (v != NIL) { fputs(" . ", out); print_value(out, v); }
  fputc(')', out);
}

extern "C" void rt_print(Value v) {
  print_value(stdout, v);
  fputc('\n', stdout);
}

[[noreturn]] void runtime_error(const char* msg) {
  fflush(stdout);
  fprintf(stderr, "runtime error: %s\n", msg);
  exit(1);
}

extern "C" [[noreturn]] void rt_error(int code) {
  runtime_error(code == RT_DIVZERO ? "division by zero" : "type error");
}
