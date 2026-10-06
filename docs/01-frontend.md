# Frontend

`src/lexer.cpp`, `src/parser.cpp`, `src/ssa_builder.cpp`, `src/ir.{h,cpp}`, `src/opt.cpp`

## Parsing

Statements use recursive descent. Expressions use a Pratt parser:

```cpp
auto lhs = prefix();
for (;;) {
  int bp = infix_bp(peek().kind);
  if (bp <= min_bp) break;
  next();
  auto rhs = expr(bp);
  lhs = binary(op, lhs, rhs);
}
```

Binding powers: `||` 10, `&&` 20, `== !=` 30, `< <= > >=` 40, `+ -` 50,
`* / %` 60, prefix 70. The `<=` makes operators left associative:
`10 - 3 - 2` parses as `(10 - 3) - 2`.

## IR

A function is a list of basic blocks; each block ends in one `jmp`, `br` or
`ret` and records its predecessors. Each instruction defines at most one
value and its operands point at the instructions that define them. Phi
operand `i` belongs to `preds[i]`. `verify()` runs after every pass and checks
that invariant, terminators, phi placement, and that no removed value is used.

## SSA construction

Braun et al., "Simple and Efficient Construction of Static Single Assignment
Form" (CC 2013). SSA is built while walking the AST, with no dominance
frontiers.

- `write_var(v, block, value)` records the value of `v` at the end of a block.
  `x = y` emits nothing; `x` just maps to `y`'s value.
- `read_var(v, block)` uses the local definition if there is one, otherwise
  asks the predecessor, or creates a phi when there are several.
- A block is sealed once all its predecessors are known. Reads in an unsealed
  block (a loop header) create an empty phi that `seal()` fills in later.
- A phi whose operands are all the same value is replaced by that value, and
  phis that used it are rechecked.

`while c { body }` is lowered as:

```
jmp header             header not sealed yet
header: br c, body, exit
seal(body)
body: ...; jmp header
seal(header)           back edge known, fill pending phis
seal(exit)
```

`&&` and `||` write a temporary on both paths and read it at the join.

The fuzzer found a bug here: in `f(a, b)` the phi returned for `a` could be
removed as trivial while `b` was being lowered, leaving a stale pointer in a
local. Removed phis now forward to their replacement (`resolve()`).

## Passes

Run until nothing changes, at `-O1` and up.

- **Constant folding** uses `eval_binop` from `value.h`, the same code the
  interpreter uses. Operations that would trap are not folded.
- **CFG simplification**: constant branches become jumps, unreachable blocks
  are removed, trivial phis go, and a block is merged into its only
  predecessor when that predecessor jumps straight to it.
- **DCE** keeps instructions with side effects and what they use. `a + b` can
  raise a type error, so it only counts as dead when both operands are known
  ints (`compute_known_int`).
- **Guard elimination** (`prove_int_operands`): after `x < n` runs, `x` and `n`
  are ints in every block it dominates. The backends skip type checks on
  operands proven this way, so `fib` has one guard on `n` instead of three.
  Dominators use Cooper, Harvey and Kennedy (2001).
- **Inlining** visits callees first, clones the callee into the caller,
  replaces `ret` with jumps to a join block with a phi, then re-optimizes the
  caller.

Compare `--dump-ir` and `--dump-opt` on `examples/sum.tiny`.
