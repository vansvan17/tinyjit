// The same fib as bench/fib.tiny, for comparing generated code:
//   cc -O0 -c fib.c && objdump -d fib.o
//   cc -O2 -c fib.c && objdump -d fib.o
#include <stdio.h>
long fib(long n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
#ifndef NO_MAIN
int main(void) { printf("%ld\n", fib(32)); return 0; }
#endif
