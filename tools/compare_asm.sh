#!/usr/bin/env bash
# Put the JIT's fib next to gcc's at -O0 and -O2.
#   tools/compare_asm.sh            (uses bench/fib.tiny and bench/c/fib.c)
set -euo pipefail
cd "$(dirname "$0")/.."
make -s all
out=build/asm
mkdir -p "$out"
cc -O0 -DNO_MAIN -c bench/c/fib.c -o "$out/fib_O0.o"
cc -O2 -DNO_MAIN -fno-optimize-sibling-calls -c bench/c/fib.c -o "$out/fib_O2.o"
build/tinyjit --emit-elf="$out/fib_jit.o" bench/fib.tiny 2>/dev/null
for f in fib_O0 fib_O2 fib_jit; do
  echo "=================== $f"
  objdump -d --no-show-raw-insn -M intel "$out/$f.o" | awk '/<(fib|tiny_fib)>:/,/^$/'
  nm -S "$out/$f.o" | awk '$4 ~ /^(fib|tiny_fib)$/ { print "size: 0x" $2 " bytes" }'
done
