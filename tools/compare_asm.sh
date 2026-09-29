#!/usr/bin/env bash
# Put the JIT's fib next to clang's at -O0 and -O2 (all arm64 Mach-O).
#   tools/compare_asm.sh      (uses bench/fib.tiny and bench/c/fib.c)
set -euo pipefail
cd "$(dirname "$0")/.."
make -s all
out=build/asm
mkdir -p "$out"
cc -O0 -DNO_MAIN -c bench/c/fib.c -o "$out/fib_O0.o"
cc -O2 -DNO_MAIN -c bench/c/fib.c -o "$out/fib_O2.o"
build/tinyjit --emit-obj="$out/fib_jit.o" bench/fib.tiny 2>/dev/null
for f in fib_O0 fib_O2 fib_jit; do
  echo "=================== $f"
  # Xcode's objdump is llvm-objdump; --no-show-raw-insn keeps it readable.
  # (clang may label the function ltmp0, a local label at the same address)
  objdump -d --no-show-raw-insn "$out/$f.o" | awk '/<(_fib|_tiny_fib|ltmp0)>:/,/^$/'
done
