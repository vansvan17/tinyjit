# tinyjit

A small language with a real compiler pipeline: a Pratt parser, SSA
construction, optimization passes, a register-based bytecode VM with a
mark-sweep garbage collector, and an x86-64 JIT with linear scan register
allocation that follows the System V ABI and can write its output as an ELF
object you can link. About 4,300 lines of C++, no dependencies.

```
fn fib(n) {
  if n < 2 { return n; }
  return fib(n - 1) + fib(n - 2);
}
fn main() { print(fib(32)); }
```

```
$ build/tinyjit --stats bench/fib.tiny
2178309
[stats] run time        13.84 ms
[stats] jit             2 functions, 305 bytes of code, 0 spills
```

Same program, same machine: 77 ms in the interpreter, 14 ms JIT-compiled,
20 ms for gcc `-O0`, 5.4 ms for gcc `-O2`.

## Build and run

Linux x86-64, g++ 11+ or clang 14+, Python 3 for the tests.

```
make                 # build/tinyjit and build/elfdump
make test            # 14 programs x 10 configurations, plus an AOT link test
make fuzz            # 300 random programs, every backend must agree
make bench           # tables below (uses perf if it works, else cachegrind)
make asm             # the JIT's fib next to gcc -O0 and -O2
make aot             # compile fib to an ELF object, link with cc, run it
```

On macOS, including Apple Silicon, use the Dockerfile (the JIT emits x86-64
and uses Linux `mmap`, and the ELF tools target Linux):

```
docker build --platform linux/amd64 -t tinyjit . && docker run --rm -it --platform linux/amd64 tinyjit
```

## The pipeline

```
source
  │  lexer, Pratt parser                          src/lexer.cpp, src/parser.cpp
  ▼
AST                                               --dump-ast
  │  Braun et al. SSA construction                src/ssa_builder.cpp
  ▼
SSA IR (CFG of basic blocks, phis)                --dump-ir
  │  constant folding, CFG simplification, DCE,   src/opt.cpp
  │  int-type analysis, dominator-based guard
  │  elimination, inlining
  ▼
optimized SSA                                     --dump-opt
  │  instruction selection (immediates,           src/mir.cpp
  │  compare+branch fusion), critical edge
  │  splitting, phis -> parallel copies
  │  copy coalescing (interference graph)         src/regalloc.cpp
  ▼
MIR (machine-level, virtual registers)            --dump-mir
  │                                  │
  │ linear scan, 250 VM registers    │ linear scan, 9 x86 registers, spilling
  ▼                                  ▼
bytecode                             x86-64 machine code  src/jit.cpp, src/x86.h
  │  register VM                     │  mmap, W^X, SysV calls
  │  switch or computed goto         │  --emit-elf: ELF .o with symbols
  │  mark-sweep GC                   │  and relocations
  ▼                                  ▼
  interpreter ── hot function (100 calls) ──► native code
```

Every stage has a dump flag, so you can watch one function go all the way
down. For `examples/sum.tiny`:

```
fn sum_to(n) {
  let s = 0;
  let i = 1;
  while i <= n { s = s + i; i = i + 1; }
  return s;
}
```

SSA after optimization (`--dump-opt --inline=0`): the assignments are gone,
replaced by two phis in the loop header, and every value the analysis could
prove is an int is marked:

```
  b1:  ; preds b0 b2
    %4 = phi %2 [b0], %11 [b2]   ; int
    %8 = phi %1 [b0], %9 [b2]    ; int
    %6 = le %4, %0               ; int
    br %6, b2, b3
  b2:  ; preds b1
    %9 = add %8, %4              ; int
    %10 = const 1                ; int
    %11 = add %4, %10            ; int
    jmp b1
```

MIR (`--dump-mir --inline=0`): the compare is fused into the branch, the
constant is an immediate, and coalescing has merged each phi with the values
that flow into it, so there are no copies left:

```
  B1:
    br_le v3:int, v0 -> B2, B3
  B2:
    v4 = add v4:int, v3:int
    v3 = add v3:int, #1
    jmp -> B1
```

x86-64 (`--emit-elf`, then `objdump -d -M intel`): `n` arrives in rdi and
stays there, `i` and `s` get r8 and rsi. Constants are doubled because ints
are tagged (`n << 1`). The one type guard left is on `n`, the only value not
proven to be an int:

```
  12:  test   rdi,0x1          ; is n an int?
  19:  jne    3f               ;   no: type error
  1f:  cmp    r8,rdi           ; i <= n ?
  22:  jg     37
  28:  add    rsi,r8           ; s = s + i
  2b:  add    r8,0x2           ; i = i + 1   (1 is stored as 2)
  32:  jmp    12
```

## The language

```
fn name(a, b) { ... }        functions; main() is the entry point
let x = expr;  x = expr;     block-scoped variables, shadowing allowed
if c { } else if d { } else { }
while c { }
return expr;
+ - * / %  < <= > >= == !=  && ||  !  unary -
nil  true  false  integers (63-bit, wrap on overflow)
print(x)  cons(a, b)  car(p)  cdr(p)  is_pair(x)  is_nil(x)
```

Dynamically typed: values are ints or pairs, `1 + nil` is a runtime error.
Only `0` is false. `/` and `%` truncate toward zero, like C.

## Results

Measured in a Linux VM without hardware performance counters, so cache and
branch figures are cachegrind simulations counting only the program run, not
the compiler. Times are best of 5. `make bench` regenerates everything on
your machine (with `perf stat` if it works there).

**Execution modes** (run time, ms):

| program | switch interp | goto interp | JIT tiered | JIT eager |
|---|---:|---:|---:|---:|
| fib.tiny: 7M calls | 83.1 | 77.0 | 13.8 | 15.6 |
| loop.tiny: Collatz to 300k | 490.7 | 423.8 | 426.1 | 142.0 |
| sieve.tiny: allocation heavy | 54.3 | 53.1 | 51.1 | 51.1 |

loop.tiny's hot loop gets inlined into `main`, which is only called once;
tiered mode counts calls and has no on-stack replacement, so only eager mode
compiles it. sieve.tiny allocates in its hot functions, which the JIT
declines (it has no stack maps for the GC), so it stays interpreted.

**Dispatch.** Computed goto vs `switch`, cachegrind branch simulation on
fib.tiny: 7.9M vs 42.3M mispredicted branches, 11% fewer instructions.
[docs/03-runtime.md](docs/03-runtime.md) explains why, and why modern CPUs
shrink the gap.

**Inlining vs the instruction cache.** On a generated program with many
call sites dispatched pseudo-randomly:

| inline threshold | code bytes | instructions | L1i misses | run time |
|---:|---:|---:|---:|---:|
| 0 | 24,611 | 31,159,031 | 504 | 10.4 ms |
| 30 | 92,714 | 25,999,031 | 1,136,529 | 10.8 ms |

17% fewer instructions, 3% slower. Scaled up to a megabyte of inlined code
it is 17% fewer instructions and 10% slower, with 9x the compile time.
Details and the reasoning in [docs/02-backend.md](docs/02-backend.md).

**GC pauses** grow with the heap, as they must for stop-the-world
mark-sweep: 0.01 ms at 2K cells, 4 ms at 512K cells. How Go and Rust avoid
this, and what it costs them instead, is in
[docs/03-runtime.md](docs/03-runtime.md).

## How it is tested

- `tests/cases/*.tiny` carry their expected output in comments
  (`// expect: 42`, `// expect-error: type error`). `tests/run_tests.py` runs
  each under 10 configurations: -O0/-O1/-O2, switch and goto dispatch, JIT
  off, tiered and eager, aggressive inlining, and `--gc-stress`, which runs a
  full collection on every allocation so a single missed root corrupts the
  heap immediately.
- `tools/fuzz.py` generates random programs (nested loops, branches, calls,
  short-circuit operators, lists, overflowing constants, and occasionally
  deliberate runtime errors) and requires every configuration to match the
  -O0 interpreter byte for byte, including the exit status. The final
  build passes 500 of 500 (and about 2,400 more ran clean during
  development, after the fixes below).
- The division tests' expected values are computed independently in Python.

Bugs found along the way, each worth knowing about:

1. **Stale phi pointer in SSA construction** (fuzzer, 13 of the first 150
   programs). In `f(a, b)`, the phi returned for `a` could be deleted as
   trivial while `b` was being evaluated, leaving a dangling pointer in a C++
   local. See [docs/01-frontend.md](docs/01-frontend.md).
2. **Page-aligned code thrashing the icache.** Putting each compiled unit on
   its own page made 29 KB of code miss the L1i 2 million times; packing it
   gave 574 misses. Cache set aliasing, explained in
   [docs/02-backend.md](docs/02-backend.md).
3. **Writing into an executable page.** Padding between packed code units
   was written into a page that had already been flipped to read+execute.
   Only showed up once code crossed a page boundary at the wrong offset, in
   a 500-function benchmark.
4. **`continue` inside `do { } while (0)`.** The `switch` interpreter's
   dispatch macro was wrapped in the usual `do/while(0)`, where `continue`
   exits the do-loop instead of restarting dispatch. Every instruction fell
   through into the next handler.

## Reading order

1. [docs/01-frontend.md](docs/01-frontend.md): Pratt parsing, SSA and the
   Braun algorithm, dominators, the passes.
2. [docs/02-backend.md](docs/02-backend.md): instruction selection, out of
   SSA, coalescing, linear scan, the JIT, inlining and the icache.
3. [docs/03-runtime.md](docs/03-runtime.md): tagged values, the register VM,
   dispatch, the GC, Go vs Rust.
4. [docs/04-binary.md](docs/04-binary.md): objdump, ELF, relocations, the
   SysV calling convention, linking JIT output as an AOT compiler.

## Layout

```
src/
  value.h           tagged values, the semantics of every primitive
  lexer.cpp parser.cpp ast.h
  ir.h ir.cpp       SSA IR, printer, verifier
  ssa_builder.cpp   AST -> SSA (Braun et al.)
  opt.cpp passes.h  folding, CFG cleanup, DCE, int analysis, dominators, inlining
  mir.h mir.cpp     instruction selection, out-of-SSA
  regalloc.cpp      liveness, coalescing, linear scan (shared by both backends)
  vm.h vm.cpp interp.inc bytecode.cpp   register VM, two dispatch loops
  gc.h gc.cpp       mark-sweep collector
  x86.h jit.cpp     x86-64 encoder, JIT, ELF writer
  runtime.cpp       print, errors (called from JIT code too)
  main.cpp          driver and flags (build/tinyjit --help)
tools/  elfdump.cpp  fuzz.py  compare_asm.sh
bench/  fib, loop, sieve, gc_live, gen_inline.py, bench.py, c/fib.c
tests/  cases/*.tiny  run_tests.py
aot/    runtime.c   for linking --emit-elf output
```

## Limitations

- x86-64 Linux only. An arm64 backend would need its own encoder and
  calling convention; the MIR and register allocator would carry over.
- The JIT does not compile functions that allocate (no GC stack maps), with
  more than six parameters (no stack-passed arguments), and there is no
  on-stack replacement.
- Intervals are single conservative ranges with whole-interval spilling.
  Second-chance binpacking or interval splitting (Wimmer and Mössenböck)
  would allocate loops with high register pressure better.
- The collector is stop-the-world and non-moving, and the language has no
  mutation of pairs, which is what lets it skip write barriers.
- Inlining and several passes use O(n) scans where use lists would be O(1);
  compile time grows quadratically on very large functions.
