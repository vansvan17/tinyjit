# Backend: instruction selection, register allocation, the JIT

Files: `src/mir.{h,cpp}`, `src/regalloc.{h,cpp}`, `src/bytecode.cpp`, `src/jit.cpp`, `src/a64.h`

Both backends (the bytecode VM and the ARM64 JIT) consume the same
lower-level IR, MIR, and share one register allocator. The VM just has a lot
more registers.

## Instruction selection

`lower_to_mir` makes two kinds of selection decisions before anything is
allocated, so neither backend needs a register for things that do not need
one:

- **Immediates.** `i + 1` becomes `add v3, #1` instead of materializing `1`
  into a register. Commutative operations and comparisons with the constant
  on the left are flipped (`1 < x` becomes `x > #1`). The VM gets `ADDK`,
  `LTK` and friends; the JIT gets `add x, x, #imm` and `cmp x, #imm` when the
  constant fits ARM64's 12-bit immediate field, and builds it in a scratch
  register otherwise.
- **Compare and branch fusion.** A comparison used only by the branch right
  after it becomes a single `br_lt a, b -> T, F`. Without this the result of
  `i < n` would be materialized as 0 or 1 in a register (`cset`, then a
  shift to tag it) and then tested again. With it, the JIT emits `cmp; b.lt`.

Division by a constant is also selected specially in the JIT: for 2^k it
emits the shift sequence clang uses for `x / 8` (add `2^k - 1` to negative
dividends, then shift arithmetic right), which rounds toward zero like
`sdiv` but uses four single-cycle instructions instead of a divide.
`bench/loop.tiny` (Collatz, full of `n % 2` and `n / 2`) is the benchmark
where this shows.

Everything else is one MIR op per SSA op.

## Out of SSA

Machines do not have phis. Each phi `p = phi(a [B1], b [B2])` becomes a copy
`p = a` at the end of B1 and `p = b` at the end of B2. Two classic traps:

- **Lost copy.** If B1 has two successors, a copy at its end runs on both
  edges and can clobber a value the other path still needs. The fix is to
  split the edge: give it its own block to hold the copies
  (`split_critical_edges`).
- **Swap.** `let t = a; a = b; b = t;` in a loop gives two phis that read each
  other: `a2 = phi(.., b2)`, `b2 = phi(.., a2)`. Emitting `a2 = b2; b2 = a2`
  in sequence is wrong. The copies on one edge are a *parallel* copy, and
  `sequentialize` orders them so no destination is written while another
  copy still needs to read it, breaking cycles with a temporary.

`tests/cases/phis.tiny` exercises both, including a three-way rotation.

## Copy coalescing

Out-of-SSA leaves a lot of moves. `coalesce_moves` removes most of them
before allocation, Chaitin style:

1. Build an interference graph: walking each block backwards with the live
   set, every definition interferes with everything live right after it.
   The one exception is `mov d, s`, which does not make d interfere with s,
   since they hold the same value.
2. For each move whose source and destination do not interfere, merge them
   into one vreg (union-find, merging adjacency sets) and delete the move.

For `examples/sum.tiny` this turns

```
B1: v5 = mov v3            (phi copies, header side)
    v6 = mov v4
    br_le v5, v0 -> ...
B3: v7 = add v6, v5
    v8 = add v5, #1
    v3 = mov v8            (phi copies, latch side)
    v4 = mov v7
```

into

```
B1: br_le v3:int, v0 -> B2, B3
B2: v4 = add v4:int, v3:int
    v3 = add v3:int, #1
    jmp -> B1
```

which is exactly the loop you would write by hand. `--dump-mir -O0` shows the
uncoalesced version.

## Linear scan register allocation

`src/regalloc.cpp`, after Poletto and Sarkar, "Linear Scan Register
Allocation" (TOPLAS 1999).

1. Number instructions linearly, blocks in reverse postorder. Instruction i
   reads its operands at 2i and writes its result at 2i+1, so a value that
   dies at an instruction and the value that instruction defines can share a
   register, while two values live at the same point never can.
2. Compute liveness with a standard backward dataflow over bitsets, then give
   each vreg one conservative interval: from the first point it is live to
   the last.
3. Walk intervals in order of start. Expire the ones that ended, freeing
   their registers. Take a free register. If there is none, spill whichever
   of the current and active intervals ends last, to a stack slot, for its
   whole life.

Chaitin-style graph coloring usually allocates better, but linear scan is a
single pass over sorted intervals, which is why JITs like it (HotSpot's C1
and V8's Crankshaft both used linear scan variants): allocation time matters
when compilation happens while the program waits.

Two extra rules for the JIT, both about calls:

- **Caller- vs callee-saved.** Under the arm64 calling convention a call
  may destroy x0-x17, and must preserve x19-x28 (and fp, lr, sp). An interval
  that is live across a call (starts before it and ends after it) may only
  get x19-x28. Other intervals prefer x1-x8 and x13-x15, so short-lived
  values do not force the prologue to save anything.
- **Hints.** A value about to be passed as argument i would like to already
  be in the i-th argument register; a move's destination would like its
  source's register. Both are just preferences tried before the free list.

The VM uses the same code with 250 registers, no spilling, and parameters
pinned to r0..rN (the calling convention puts arguments there).

`--dump-regalloc` prints every JIT interval and where it landed.

## The JIT

`src/jit.cpp` and the encoder in `src/a64.h`.

**Encoding.** Every ARM64 instruction is exactly 32 bits, so the encoder is a
list of bit patterns: `add x1, x2, x3` is `0x8B000000 | x3 << 16 | x2 << 5 |
x1`. The awkward part is immediates. `add` takes a 12-bit unsigned constant,
logical operations take "bitmask immediates" (a repeating pattern of ones,
which is how `tst x0, #1` fits), and an arbitrary 64-bit constant takes up to
four instructions: `movz` for one 16-bit chunk, then `movk` to fill in the
others (`movn` when the value is mostly ones, like small negatives). Register
number 31 means the zero register in most instructions and the stack pointer
in a few, which is why `mov x29, sp` is really `add x29, sp, #0`.

**Registers.** 31 general registers is a lot next to x86-64's 16. The JIT
keeps x9-x12, x16 and x17 as scratch for instruction selection, never touches
x18 (reserved by Apple for the OS), and allocates the other 22.

**Frame.** `stp x29, x30, [sp, #-16]!; mov x29, sp; sub sp, sp, #N`: save
the frame pointer and the link register (the return address, which arm64
keeps in a register rather than on the stack), point x29 at the pair, and
reserve N bytes for saved callee-saved registers and spill slots. N is a
multiple of 16 because arm64 faults on a misaligned sp.

**Instruction selection per op.** Values are tagged (`docs/03-runtime.md`),
which makes most arithmetic cheap: tagged `a + b` is a plain `add`, so an add
of two unknown values is

```
orr  x12, a, b ; tst x12, #1 ; b.ne type_error    ; guard both at once
add  dst, a, b                                    ; three-address: no copies
```

and when the optimizer proved both operands are ints the guard is gone.
Comparisons become `cmp` + `b.cond` when fused into a branch, or `cset` +
`lsl` (to tag the 0/1) when their value is needed. Error paths jump to one
out-of-line stub per function.

**Calls.** Arguments go in x0-x7 (the JIT refuses functions with more than
eight parameters; they stay interpreted), the result comes back in x0, and
`bl` puts the return address in x30. Getting the arguments from wherever the
allocator put them into x0-x7 is a parallel move, the same problem as phi
copies: `f(b, a)` with a in x0 and b in x1 is a swap. `parallel_move` orders
the moves and breaks cycles through x16. The same routine moves incoming
parameters to their allocated homes in the prologue.

Calls between compiled functions are a single `bl`, which reaches +-128 MB;
all JIT code lives in one 64 MB region so that always works. Calls into the C
runtime (`rt_print`, `rt_error`) cannot assume that, since the runtime lives
in the tinyjit binary somewhere else in the address space. They go to a small
trampoline at the end of the function that loads the full address and jumps:

```
bl   tramp          ; in the body
...
tramp:
ldr  x16, #8        ; x16 = the 8 bytes right after the next instruction
br   x16
.quad rt_print
```

Linkers do the same thing for far calls (they call them veneers or branch
islands), and it is why x16 and x17 are reserved as "intra-procedure-call"
scratch registers in the calling convention.

**Memory, W^X, and the instruction cache.** macOS on Apple Silicon does not
allow a page to be writable and executable at the same time. A JIT maps its
region with `MAP_JIT`, and each thread flips its own view of that region with
`pthread_jit_write_protect_np(0)` (writable) and `(1)` (executable). The
flip is per thread and costs almost nothing, unlike an `mprotect` system
call. After writing, the JIT calls `sys_icache_invalidate`: ARM keeps the
instruction cache separate from data writes, so without that the CPU can run
whatever stale bytes it had cached for those addresses. On Linux (used to
fuzz under emulation) the same steps are `mprotect` and
`__builtin___clear_cache`.

Functions are packed back to back, 16-byte aligned, so a hot loop's functions
share cache lines and pages.

**What gets compiled.** In tiered mode (default) a function is compiled
after 100 calls, together with everything it can call, so compiled code
never has to call back into the interpreter. A function that allocates
(`cons`) is never compiled: the collector finds roots by scanning the VM's
register stack, and JIT frames keep values in machine registers the
collector cannot see. Real JITs solve this with stack maps (a table, per call
site, of which registers and slots hold pointers). There is no on-stack
replacement either, so a hot loop in a function that is only called once
(like `main`) only gets compiled with `--jit=eager`.

## Inlining and code size

`bench/gen_inline.py` generates 16 small leaf functions, 48 mid functions
that each call 8 leaves, and a main loop that picks a mid pseudo-randomly each
iteration through a tree of ifs (think `switch` or a virtual call). On an M5,
best of 5:

| inline threshold | calls inlined | code bytes | run time | compile time |
|---:|---:|---:|---:|---:|
| 0 | 0 | 18,952 | 4.62 ms | 2.3 ms |
| 30 | 384 | 66,220 | 4.47 ms | 14.8 ms |
| 400 | 432 | 122,464 | 4.40 ms | 54.7 ms |

Inlining wins here. The call, the prologue and epilogue and the argument
shuffling are gone, and the inlined copies are re-optimized in context. The
code grew 6x, to 120 KB, but that still fits in the L1 instruction cache of
Apple's performance cores (192 KB on the M1 through M4, several times the
32 KB typical of x86 cores).

Scale the generator up (`M=512 ITERS=200000 python3 bench/gen_inline.py`, 512
mids) and it flips:

| inline threshold | calls inlined | code bytes | run time | compile time |
|---:|---:|---:|---:|---:|
| 0 | 0 | 169,300 | 16.84 ms | 47.5 ms |
| 30 | 4,096 | 672,988 | 17.77 ms | 185.9 ms |
| 400 | 4,172 | 762,132 | 17.40 ms | 304.4 ms |

Now the inlined code is several times the L1 instruction cache, and each
iteration jumps to a different copy of the same leaf code, so the front end
keeps refetching from L2. The non-inlined version executes more instructions
but keeps reusing the same 170 KB. Inlining is 3-6% slower here and costs 4
to 6 times the compile time, which in a JIT is paid while the program waits.

This is why real inliners have budgets: HotSpot caps callee bytecode size
(`MaxInlineSize`, `FreqInlineSize`) and total inlined size per compilation,
and weighs call-site frequency, so only hot, small callees get copied. The
right threshold also depends on the machine: the crossover on a core with a
32 KB instruction cache comes much earlier than on an M-series core.

To count instruction cache misses yourself on a Mac, Instruments' CPU
Counters template can sample the L1I miss events while the benchmark runs.
