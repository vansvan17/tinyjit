# tinyjit

A small dynamically typed language with a parser, SSA optimizer, register
bytecode VM, mark-sweep GC, and an ARM64 JIT for Apple Silicon. The JIT can
also write a Mach-O object that links with `cc`. About 4,000 lines of C++, no
dependencies.

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

## Build

Apple Silicon Mac, Xcode command line tools, Python 3 for the tests.

```
make          # build/tinyjit, build/machodump
make test     # 14 programs x 10 configurations, plus an AOT link test
make fuzz     # 300 random programs; every configuration must agree
make bench
make asm      # JIT output next to clang -O0 and -O2
make aot      # compile fib to a .o, link with cc, run it
```

On other machines everything except the JIT builds and `--jit` falls back to
the interpreter. The JIT also runs on arm64 Linux
(`TINYJIT="qemu-aarch64 build/tinyjit" make test`).

## Pipeline

```
source -> AST -> SSA -> optimized SSA -> MIR -+-> bytecode -> VM + GC
                                              +-> ARM64 -> JIT / Mach-O .o
```

| stage | code | dump flag |
|---|---|---|
| lexer, Pratt parser | `lexer.cpp`, `parser.cpp` | `--dump-ast` |
| SSA construction (Braun et al.) | `ssa_builder.cpp` | `--dump-ir` |
| folding, CFG cleanup, DCE, guard elimination, inlining | `opt.cpp` | `--dump-opt` |
| instruction selection, out of SSA, coalescing | `mir.cpp`, `regalloc.cpp` | `--dump-mir` |
| linear scan, bytecode | `regalloc.cpp`, `bytecode.cpp` | `--dump-bc` |
| ARM64 codegen | `jit.cpp`, `a64.h` | `--dump-regalloc`, `--emit-obj` |

Functions start in the interpreter and are compiled after 100 calls.

`examples/sum.tiny` (a loop summing 1..n) after the whole pipeline:

```
  10:  tst   x0, #0x1          ; n is an int?
  14:  b.ne  0x3c              ; no: type error
  18:  cmp   x2, x0            ; i <= n
  1c:  b.gt  0x2c
  20:  add   x1, x1, x2        ; s += i
  24:  add   x2, x2, #0x2      ; i += 1 (ints are tagged, 1 is stored as 2)
  28:  b     0x10
```

## Language

```
fn name(a, b) { ... }      main() is the entry point
let x = expr;  x = expr;   block scoped, shadowing allowed
if c { } else if d { } else { }
while c { }
return expr;
+ - * / %  < <= > >= == !=  && ||  !  unary -
nil true false, 63-bit ints that wrap
print(x) cons(a, b) car(p) cdr(p) is_pair(x) is_nil(x)
```

Values are ints or pairs; `1 + nil` is a runtime error. Only `0` is false.
`/` and `%` truncate toward zero.

## Results

M5 MacBook Air (arm64 Linux VM), best of 5, run time only.

| program | switch interp | goto interp | JIT tiered | JIT eager |
|---|---:|---:|---:|---:|
| fib.tiny | 39.1 ms | 27.1 ms | 5.9 ms | 5.8 ms |
| loop.tiny | 295.0 ms | 204.8 ms | 203.6 ms | 73.4 ms |
| sieve.tiny | 24.7 ms | 22.1 ms | 22.2 ms | 23.4 ms |

loop.tiny's hot loop is inlined into `main`, which runs once, so only eager
mode compiles it (no on-stack replacement). sieve.tiny allocates, and the JIT
skips functions that allocate.

Inlining on a generated program with many call sites:

| program | inline threshold | code bytes | run time | compile time |
|---|---:|---:|---:|---:|
| 48 mid functions | 0 | 18,952 | 4.62 ms | 2.3 ms |
| | 400 | 122,464 | 4.40 ms | 54.7 ms |
| 512 mid functions | 0 | 169,300 | 16.84 ms | 47.5 ms |
| | 400 | 762,132 | 17.40 ms | 304.4 ms |

GC pauses grow with the heap: about 0.01 ms at 2K cells, 1 ms at 512K.

## Testing

- `tests/cases/*.tiny` hold their expected output in `// expect:` comments.
  Each runs under -O0/-O1/-O2, both dispatch modes, JIT off/tiered/eager,
  heavy inlining, and `--gc-stress` (collect on every allocation).
- `tools/fuzz.py` generates random programs and checks every configuration
  against the -O0 interpreter, output and exit status.

Bugs the tests found:

1. In `f(a, b)`, the phi returned for `a` could be removed as trivial while
   `b` was evaluated, leaving a dangling pointer in the SSA builder.
2. `write_var(var, cur, expr(e))` depended on argument evaluation order, which
   C++ leaves unspecified. Correct on x86-64, wrong on arm64.
3. Padding was written into a code page that was already executable.
4. A `continue` inside the usual `do { } while (0)` macro wrapper broke the
   `switch` interpreter.

## Docs

1. [Frontend](docs/01-frontend.md): parsing, SSA, the passes
2. [Backend](docs/02-backend.md): isel, out of SSA, register allocation, the JIT
3. [Runtime](docs/03-runtime.md): tagged values, the VM, the GC
4. [Binaries](docs/04-binary.md): Mach-O, relocations, calling convention

## Limitations

- JIT is arm64 only.
- No JIT for functions that allocate (no stack maps), take more than eight
  arguments, or are larger than about 1 MB. No on-stack replacement.
- Single-range intervals, spilled whole.
- Stop-the-world, non-moving collector.
- Some passes scan the whole function where use lists would be faster.
