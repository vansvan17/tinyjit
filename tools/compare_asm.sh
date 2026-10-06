#!/usr/bin/env bash
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
  objdump -d --no-show-raw-insn "$out/$f.o" | awk '/<(_fib|_tiny_fib|ltmp0)>:/,/^$/'
done
