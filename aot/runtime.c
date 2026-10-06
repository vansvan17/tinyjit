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
