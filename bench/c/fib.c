#include <stdio.h>
long fib(long n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
#ifndef NO_MAIN
int main(void) { printf("%ld\n", fib(32)); return 0; }
#endif
