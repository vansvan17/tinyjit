// Minimal runtime for linking an object from `tinyjit --emit-elf=prog.o`
// into a standalone executable:
//
//   build/tinyjit --emit-elf=prog.o prog.tiny
//   cc -no-pie aot/runtime.c prog.o -o prog && ./prog
//
// -no-pie because the object uses absolute 64-bit relocations (R_X86_64_64)
// in .text for its calls into this runtime; a PIE would need text
// relocations. Only programs whose functions are all jittable (no cons) can
// be linked this way, since allocating functions are never compiled.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint64_t Value;
extern Value tiny_main(void);

static void print_value(Value v) {
  if ((v & 1) == 0) { printf("%lld", (long long)((int64_t)v >> 1)); return; }
  if (v == 1) { printf("nil"); return; }
  printf("<pair>");
}

void rt_print(Value v) {
  print_value(v);
  putchar('\n');
}

void rt_error(int code) {
  fflush(stdout);
  fprintf(stderr, "runtime error: %s\n", code == 2 ? "division by zero" : "type error");
  exit(1);
}

int main(void) {
  tiny_main();
  return 0;
}
