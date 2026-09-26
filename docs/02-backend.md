# Backend: instruction selection, register allocation, the JIT

Files: `src/mir.{h,cpp}`, `src/regalloc.{h,cpp}`, `src/bytecode.cpp`, `src/jit.cpp`, `src/x86.h`

Both backends (the bytecode VM and the x86-64 JIT) consume the same
lower-level IR, MIR, and share one register allocator. The VM just has a lot
more registers.

## Instruction selection

`lower_to_mir` makes two kinds of selection decisions before anything is
allocated, so neither backend needs a register for things that do not need
one:

- **Immediates.** `i + 1` becomes `add v3, #1` instead of materializing `1`
  into a register. Commutative operations and comparisons with the constant
  on the left are flipped (`1 < x` becomes `x > #1`). The VM gets `ADDK`,
  `LTK` and friends; the JIT gets `add r, imm32` and `cmp r, imm32`.
- **Compare and branch fusion.** A comparison used only by the branch right
  after it becomes a single `br_lt a, b -> T, F`. Without this the result of
  `i < n` would be materialized as 0 or 1 in a register (`setl`, `movzx`,
  shift to tag it) and then tested again. With it, the JIT emits `cmp; jl`.

Division by a constant is also selected specially in the JIT: for 2^k it
emits the shift sequence gcc uses for `x / 8` (add `2^k - 1` to negative
dividends, then shift arithmetic right), which rounds toward zero like
`idiv` but costs four single-cycle instructions instead of 20 to 40 cycles.
On `bench/loop.tiny` (Collatz, full of `n % 2` and `n / 2`) that took the JIT
from 350 ms to 205 ms; copy coalescing (below) then brought it to 142 ms.

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

- **Caller- vs callee-saved.** Under the System V ABI a call may destroy rax,
  rcx, rdx, rsi, rdi, r8-r11, and must preserve rbx, rbp, r12-r15. An interval
  that is live across a call (starts before it and ends after it) may only
  get rbx or r12-r15. Other intervals prefer rsi, rdi, r8, r9, so short-lived
  values do not force the prologue to save anything.
- **Hints.** A value about to be passed as argument i would like to already
  be in the i-th argument register; a move's destination would like its
  source's register. Both are just preferences tried before the free list.

The VM uses the same code with 250 registers, no spilling, and parameters
pinned to r0..rN (the calling convention puts arguments there).

`--dump-regalloc` prints every JIT interval and where it landed.

## The JIT

`src/jit.cpp` and the encoder in `src/x86.h`.

**Encoding.** x86-64 instructions are `[REX] opcode ModRM [disp] [imm]`. REX's
W bit selects 64-bit operands and its R and B bits supply the fourth bit of
register numbers (that is how r8-r15 exist). ModRM picks register-register
(mod=11) or `[base + disp32]` (mod=10). The assembler knows about 40
instruction forms, which is all this needs.

**Frame.** `push rbp; mov rbp, rsp; sub rsp, N` where N covers the saved
callee-saved registers and spill slots and is a multiple of 16. Since the
return address plus the pushed rbp are 16 bytes, rsp stays 16-byte aligned
for the whole body, which the ABI requires at every call, so calls need no
fixups.

**Instruction selection per op.** Values are tagged (`docs/03-runtime.md`),
which makes most arithmetic cheap: tagged `a + b` is a plain `add`, so an add
of two unknown values is

```
mov rdx, a ; or rdx, b ; test rdx, 1 ; jne type_error   ; guard both at once
mov dst, a ; add dst, b                                   ; x86 is two-address
```

and when the optimizer proved both operands are ints the guard is gone.
Comparisons become `cmp` + `jcc` when fused into a branch, or `setcc` + `movzx`
+ `shl` (to tag the 0/1) when their value is needed. Error paths jump to one
out-of-line stub per function that calls `rt_error`.

**Calls.** Arguments go in rdi, rsi, rdx, rcx, r8, r9 (the JIT refuses
functions with more than six parameters; they stay interpreted). Getting the
arguments from wherever the allocator put them into those registers is a
parallel move, the same problem as phi copies: `f(b, a)` with a in rdi and b
in rsi is a swap. `parallel_move` orders the moves and breaks cycles through
rax. The same routine moves incoming parameters to their allocated homes in
the prologue.

**Memory and W^X.** Code goes into one 64 MB `mmap` region, so every call
between compiled functions fits in a 32-bit relative displacement. Pages are
writable while code is copied in and are then `mprotect`ed to read+execute:
never writable and executable at once. Functions are packed back to back.

**What gets compiled.** In tiered mode (default) a function is compiled
after 100 calls, together with everything it can call, so compiled code
never has to call back into the interpreter. A function that allocates
(`cons`) is never compiled: the collector finds roots by scanning the VM's
register stack, and JIT frames keep values in machine registers the
collector cannot see. Real JITs solve this with stack maps (a table, per call
site, of which registers and slots hold pointers). There is no on-stack
replacement either, so a hot loop in a function that is only called once
(like `main`) only gets compiled with `--jit=eager`.

## Inlining and the instruction cache

`bench/gen_inline.py` generates 16 small leaf functions, 48 mid functions
that each call 8 leaves, and a main loop that picks a mid pseudo-randomly each
iteration through a tree of ifs (think `switch` or a virtual call). Numbers
from `bench/bench.py` (instruction and cache counts simulated by cachegrind,
counting only the program run):

| inline threshold | calls inlined | code bytes | instructions | L1i misses | run time |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 24,611 | 31,159,031 | 504 | 10.4 ms |
| 30 | 384 | 92,714 | 25,999,031 | 1,136,529 | 10.8 ms |
| 400 | 432 | 173,467 | 25,099,030 | 1,038,723 | 10.7 ms |

(Measured in a Linux VM; `make bench` regenerates this for your machine in
`build/bench_results.md`.)

Inlining did what it promises: 17% fewer instructions, because the call,
the prologue and epilogue, and the argument shuffling are gone, and the
inlined copies are re-optimized in context. But the reachable code grew
from 25 KB, which fits in a 32 KB L1 instruction cache, to 90-170 KB, which
does not, and because each iteration jumps to a different copy, misses went
from almost none to about 19 per iteration. Run time went up 3% while the
instruction count went down 17%.

Scale the generator up (`M=512 ITERS=200000 python3 bench/gen_inline.py`, 512
mids) and the effect is clearer:

| inline threshold | code bytes | instructions | L1i misses | run time | compile time |
|---:|---:|---:|---:|---:|---:|
| 0 | 222,752 | 106,284,838 | 1,657,355 | 40.8 ms | 109 ms |
| 400 | 1,076,548 | 88,639,293 | 6,326,063 | 44.7 ms | 947 ms |

17% fewer instructions, 10% slower, and 9x the compile time, which in a JIT
is paid while the program waits. An L1i miss that hits L2 only costs about a
dozen cycles, and the CPU front end hides some of that, which is why the
wall-time effect is smaller than the miss counts suggest. This is why real
inliners have budgets: HotSpot caps callee bytecode size (`MaxInlineSize`,
`FreqInlineSize`) and total inlined size per compilation, and weighs call
site frequency, so only hot, small callees get copied.

### A cache effect found by accident

The first version put each compiled unit on its own 4 KB page. With inlining
off, 29 KB of code produced **1,975,895** L1i misses on this benchmark.
Packing the same code contiguously gave **574**. Nothing about the code
changed, only where it lived. (These two numbers are from before copy
coalescing shrank the code, hence 29 KB rather than 25 KB.)

The cause is cache associativity. A 32 KB, 8-way L1 has 64 sets; the set is
picked by address bits 6-11. Every page-aligned function starts at an address
with those bits equal to zero, so all 65 function entry points (and the
first cache lines after them, which are the hottest) compete for the same
few sets, 8 ways each. The cache was nearly empty and still thrashing. This
is one reason linkers and JITs align functions to 16 or 32 bytes, not to
pages, and why profile-guided layout (BOLT, `-freorder-functions`) groups
hot functions together.
