# Backend

`src/mir.{h,cpp}`, `src/regalloc.{h,cpp}`, `src/bytecode.cpp`, `src/jit.cpp`, `src/a64.h`

The VM and the JIT both consume MIR and share one register allocator.

## Instruction selection

- Constants become immediates: `i + 1` is `add v3, #1`, and `1 < x` is
  flipped to `x > #1`. The VM gets `ADDK`, `LTK` etc.; the JIT uses a 12-bit
  immediate when the constant fits and a scratch register otherwise.
- A compare used only by the following branch is fused into it
  (`br_lt a, b`), which becomes `cmp; b.lt` instead of materializing a 0/1.
- Division by a power of two uses shifts (adding `2^k - 1` to negative
  dividends so it rounds toward zero like `sdiv`).

## Out of SSA

Each phi becomes a copy at the end of each predecessor.

- Critical edges are split first (`split_critical_edges`); otherwise a copy
  for one edge would also run on the block's other edge.
- The copies on one edge are a parallel copy. `sequentialize` orders them so
  nothing is overwritten before it is read, and breaks cycles (a swap) with a
  temporary.

`tests/cases/phis.tiny` covers swaps and rotations.

## Copy coalescing

`coalesce_moves` builds an interference graph from liveness (a `mov d, s` does
not make `d` interfere with `s`) and merges the two sides of every move that
don't interfere. On `examples/sum.tiny` that removes every copy:

```
B1: br_le v3:int, v0 -> B2, B3
B2: v4 = add v4:int, v3:int
    v3 = add v3:int, #1
    jmp -> B1
```

`--dump-mir -O0` shows it without coalescing.

## Linear scan

Poletto and Sarkar, TOPLAS 1999.

1. Number instructions in reverse postorder. Operands are read at `2i`, the
   result written at `2i+1`, so a dying operand and the result can share a
   register.
2. Backward liveness over bitsets gives each vreg one interval.
3. Walk intervals by start, free expired registers, take a free one, or spill
   whichever interval ends last.

JIT rules:

- An interval live across a call only gets x19-x28 (callee-saved). Others
  prefer caller-saved registers so the prologue saves less.
- Arguments prefer their argument register, and a move's destination prefers
  its source's register.

The VM uses the same code with 250 registers, no spilling, and parameters in
r0..rN. `--dump-regalloc` prints the JIT intervals.

## JIT

Every A64 instruction is 32 bits, so `a64.h` is mostly bit patterns. Constants
over 16 bits take `movz` plus up to three `movk` (or `movn` for mostly-ones
values). Register 31 is the zero register in most instructions and sp in a few.

The JIT keeps x9-x12, x16 and x17 as scratch, never touches x18 (reserved on
Apple platforms), and allocates the other 22.

Frame: `stp x29, x30, [sp, #-16]!; mov x29, sp; sub sp, sp, #N`, with N a
multiple of 16 covering saved registers and spill slots.

Tagged ints make arithmetic cheap. An add of two unknown values:

```
orr  x12, a, b ; tst x12, #1 ; b.ne type_error
add  dst, a, b
```

The guard disappears when both operands are proven ints. Compares become
`cmp` + `b.cond`, or `cset` + `lsl` when the value is needed. Type errors jump
to one stub per function.

Calls: arguments in x0-x7, result in x0. Moving arguments into place is a
parallel move (`parallel_move`, cycles broken through x16); the prologue does
the same for incoming parameters. Calls between JIT functions are a single
`bl`, since all JIT code is in one 64 MB region. Calls to `rt_print` and
`rt_error` go through a trampoline, because the runtime may be out of `bl`
range:

```
bl   tramp
...
tramp:
ldr  x16, #8
br   x16
.quad rt_print
```

Memory: macOS doesn't allow writable and executable at once. The region is
mapped with `MAP_JIT`; `pthread_jit_write_protect_np(0)` makes it writable for
this thread, `(1)` executable again, then `sys_icache_invalidate` flushes the
instruction cache. Linux uses `mprotect` and `__builtin___clear_cache`.
Functions are packed back to back, 16-byte aligned.

A function is compiled after 100 calls along with everything it can call.
Functions that allocate are not compiled, since the GC can't find roots in
JIT frames without stack maps. There is no on-stack replacement, so a loop in
`main` only gets compiled with `--jit=eager`.

## Inlining and code size

`bench/gen_inline.py`: 16 leaf functions, 48 mid functions calling 8 leaves
each, and a loop that picks a mid pseudo-randomly. M5, best of 5:

| inline threshold | calls inlined | code bytes | run time | compile time |
|---:|---:|---:|---:|---:|
| 0 | 0 | 18,952 | 4.62 ms | 2.3 ms |
| 30 | 384 | 66,220 | 4.47 ms | 14.8 ms |
| 400 | 432 | 122,464 | 4.40 ms | 54.7 ms |

Inlining helps here; 120 KB still fits the large L1 instruction cache on
Apple's performance cores. With 512 mids
(`M=512 ITERS=200000 python3 bench/gen_inline.py`):

| inline threshold | calls inlined | code bytes | run time | compile time |
|---:|---:|---:|---:|---:|
| 0 | 0 | 169,300 | 16.84 ms | 47.5 ms |
| 30 | 4,096 | 672,988 | 17.77 ms | 185.9 ms |
| 400 | 4,172 | 762,132 | 17.40 ms | 304.4 ms |

Now the inlined code is several times the instruction cache, and each
iteration jumps to a different copy, so it is 3-6% slower and compiles 4-6x
slower. Real inliners cap callee and total size for this reason.
