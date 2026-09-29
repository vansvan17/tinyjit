# tinyjit

A small language with a real compiler pipeline, built for Apple Silicon: a
Pratt parser, SSA construction, optimization passes, a register-based bytecode
VM with a mark-sweep garbage collector, and an ARM64 JIT with linear scan
register allocation that follows the Apple arm64 calling convention and can
write its output as a Mach-O object you link with `cc`. About 4,600 lines of
C++, no dependencies.

```
fn fib(n) {
  if n < 2 { return n; }
  return fib(n - 1) + fib(n - 2);
}
fn main() { print(fib(32)); }
```

```
$ build/tinyjit --stats --jit=eager bench/fib.tiny
2178309
[stats] run time        13.01 ms
[stats] jit             2 functions, 260 bytes of code, 0 spills
```

Benchmarks against the interpreter and against C are under Results.

## Build and run

An Apple Silicon Mac with the Xcode command line tools
(`xcode-select --install`), and Python 3 for the tests.

```
make                 # build/tinyjit and build/machodump
make test            # 14 programs x 10 configurations, plus an AOT link test
make fuzz            # 300 random programs, every backend must agree
make bench           # the tables below, for your machine
make asm             # the JIT's fib next to clang -O0 and -O2
make aot             # compile fib to a Mach-O object, link with cc, run it
```

Elsewhere, everything but the JIT still builds: `--jit` falls back to the
interpreter. The JIT also runs on arm64 Linux, which is how it is fuzzed
under emulation (`TINYJIT="qemu-aarch64 build/tinyjit" make test`).

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
  │ linear scan, 250 VM registers    │ linear scan, 22 arm64 registers, spilling
  ▼                                  ▼
bytecode                             ARM64 machine code   src/jit.cpp, src/a64.h
  │  register VM                     │  MAP_JIT, per-thread W^X, icache flush
  │  switch or computed goto         │  Apple arm64 calling convention
  │  mark-sweep GC                   │  --emit-obj: Mach-O .o with symbols
  │                                  │  and relocations
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

ARM64 (`--emit-obj`, then `objdump -d`): `n` arrives in x0 and stays there,
`s` and `i` get x1 and x2. Constants are doubled because ints are tagged
(`n << 1`). The one type guard left is on `n`, the only value not proven to
be an int:

```
  10:  tst   x0, #0x1          ; is n an int?
  14:  b.ne  0x3c              ;   no: type error
  18:  cmp   x2, x0            ; i <= n ?
  1c:  b.gt  0x2c
  20:  add   x1, x1, x2        ; s = s + i
  24:  add   x2, x2, #0x2      ; i = i + 1   (1 is stored as 2)
  28:  b     0x10
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

Measured on an M5 MacBook Air (in an arm64 Linux VM, where the JIT emits the
same code as on macOS), best of 5 runs, program run time only. `make bench`
regenerates these tables on your machine.

| program | switch interp | goto interp | JIT tiered | JIT eager |
|---|---:|---:|---:|---:|
| fib.tiny: 7M calls | 39.1 ms | 27.1 ms | 5.9 ms | 5.8 ms |
| loop.tiny: Collatz to 300k | 295.0 ms | 204.8 ms | 203.6 ms | 73.4 ms |
| sieve.tiny: allocation heavy | 24.7 ms | 22.1 ms | 22.2 ms | 23.4 ms |

loop.tiny's hot loop gets inlined into `main`, which is only called once;
tiered mode counts calls and has no on-stack replacement, so only eager mode
compiles it. sieve.tiny allocates in its hot functions, which the JIT
declines (it has no stack maps for the GC), so it stays interpreted.

**Dispatch.** Computed goto is 30% faster than `switch` on fib and loop.
[docs/03-runtime.md](docs/03-runtime.md) explains why.

**Inlining vs code size.** On a generated program with many call sites
dispatched pseudo-randomly, inlining wins while the code fits the M5's large
L1 instruction cache, and loses once it does not:

| program | inline threshold | code bytes | run time | compile time |
|---|---:|---:|---:|---:|
| 48 mid functions | 0 | 18,952 | 4.62 ms | 2.3 ms |
| | 400 | 122,464 | 4.40 ms | 54.7 ms |
| 512 mid functions | 0 | 169,300 | 16.84 ms | 47.5 ms |
| | 400 | 762,132 | 17.40 ms | 304.4 ms |

Details and the reasoning in [docs/02-backend.md](docs/02-backend.md).

**GC pauses** grow with the heap, as they must for stop-the-world
mark-sweep: 0.01 ms at 2K cells, about 1 ms at 512K cells. How Go and Rust
avoid this, and what it costs them instead, is in
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
2. **Unspecified argument order.** `write_var(var, cur, expr(e))` reads
   `cur` (the current block) and evaluates `expr(e)`, which can move `cur`
   when `e` contains `&&` or `||`. C++ does not say which argument is
   evaluated first. The x86-64 build happened to do it right; the first
   arm64 build wrote the variable into the wrong block and printed wrong
   answers in about 1 in 11 fuzzed programs, in every mode, interpreter
   included.
3. **Writing into an executable page.** Padding between packed code units
   was written into a page that had already been flipped to executable.
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
   SSA, coalescing, linear scan, the ARM64 JIT, inlining and code size.
3. [docs/03-runtime.md](docs/03-runtime.md): tagged values, the register VM,
   dispatch, the GC, Go vs Rust.
4. [docs/04-binary.md](docs/04-binary.md): objdump and otool, Mach-O,
   relocations, the Apple arm64 calling convention, linking JIT output as an
   AOT compiler.

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
  a64.h jit.cpp     ARM64 encoder, JIT, Mach-O writer
  runtime.cpp       print, errors (called from JIT code too)
  main.cpp          driver and flags (build/tinyjit --help)
tools/  machodump.cpp  fuzz.py  compare_asm.sh
bench/  fib, loop, sieve, gc_live, gen_inline.py, bench.py, c/fib.c
tests/  cases/*.tiny  run_tests.py
aot/    runtime.c   for linking --emit-obj output
```

## Limitations

- The JIT targets arm64 only; on other machines everything is interpreted.
- The JIT does not compile functions that allocate (no GC stack maps), with
  more than eight parameters (no stack-passed arguments), or bigger than
  about 1 MB (conditional branches reach +-1 MB), and there is no on-stack
  replacement.
- Intervals are single conservative ranges with whole-interval spilling.
  Second-chance binpacking or interval splitting (Wimmer and Mössenböck)
  would allocate loops with high register pressure better.
- The collector is stop-the-world and non-moving, and the language has no
  mutation of pairs, which is what lets it skip write barriers.
- Inlining and several passes use O(n) scans where use lists would be O(1);
  compile time grows quadratically on very large functions.
