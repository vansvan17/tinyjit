# Frontend: text to optimized SSA

Files: `src/lexer.cpp`, `src/parser.cpp`, `src/ssa_builder.cpp`, `src/ir.{h,cpp}`, `src/opt.cpp`

## Lexing and parsing

The lexer is a single loop over characters. Nothing interesting happens there
except range-checking integer literals, because ints are 63 bits (see
`docs/03-runtime.md`).

Statements are parsed by plain recursive descent. Expressions use a Pratt
parser (`Parser::expr` in `src/parser.cpp`). The whole trick is one loop:

```cpp
auto lhs = prefix();                 // literal, variable, call, (expr), -x, !x
for (;;) {
  int bp = infix_bp(peek().kind);    // binding power of the next operator
  if (bp <= min_bp) break;           // it binds looser than our caller: stop
  next();
  auto rhs = expr(bp);               // parse the right side at this power
  lhs = binary(op, lhs, rhs);
}
```

Precedence lives in one table (`||` 10, `&&` 20, `== !=` 30, `< <= > >=` 40,
`+ -` 50, `* / %` 60, prefix 70). Left associativity falls out of the strict
`<=` comparison: in `10 - 3 - 2`, the recursive call for the right side of the
first `-` sees another `-` with the same power and stops, so the result is
`(10 - 3) - 2`. Right associativity would use `bp - 1` for the recursive call.

`--dump-ast` prints the tree as s-expressions.

## The IR

`src/ir.h`. A function is a list of basic blocks. A block is a list of
instructions ending in exactly one terminator (`jmp`, `br`, `ret`) and knows
its predecessors. Every instruction defines at most one value, and operands
are pointers to the instructions that define them. There are no variables and
no names, which is what makes it SSA: every value has exactly one definition,
so "where does this come from" is a pointer dereference.

Where control flow merges, a `phi` picks a value based on which predecessor
we came from: `%4 = phi %2 [b0], %11 [b2]` means "%2 if we arrived from b0,
%11 if from b2". Phi operand i always corresponds to `preds[i]`. Every pass
that edits the CFG has to keep that invariant, and `verify()` checks it (along
with "terminator last", "phis first", "no use of a deleted value", and that
pred and succ lists agree) after every pass.

## Building SSA: Braun et al. 2013

The classic construction (Cytron et al.) computes dominance frontiers, places
phis for every variable at every frontier, then renames. It needs the whole
CFG up front and produces many dead phis.

Braun, Buchwald, Hack, Leissa, Mallon, Zwinkau, "Simple and Efficient
Construction of Static Single Assignment Form" (CC 2013) builds SSA directly
while walking the AST, and `src/ssa_builder.cpp` follows the paper closely:

- `write_var(v, block, value)`: remember that variable v holds `value` at the
  end of `block`. An assignment `x = y + 1` emits one `add` and records it as
  x's current value. An assignment `x = y` emits nothing at all: x now maps
  to the same instruction as y. That is copy propagation for free.
- `read_var(v, block)`: if the block defines v, done. Otherwise look in the
  predecessors. One predecessor: recurse into it. Several: create a phi and
  read v in each predecessor to fill its operands.
- Loops are the catch: when lowering a `while` header, the back edge from the
  end of the body does not exist yet. So a block is only **sealed** once all
  its predecessors are known. Reading a variable in an unsealed block creates
  an operandless phi and remembers it; `seal()` fills those in later.
- A phi whose operands are all the same value (or the phi itself) is
  **trivial**. `try_remove_trivial_phi` replaces it with that value and then
  re-checks every phi that used it, since removing one can make another
  trivial. This is why the builder's output has so few phis.

The lowering order that makes sealing work, for `while c { body }`:

```
cur -> jmp header            header is NOT sealed (back edge unknown)
header: c = ...; br c, body, exit
seal(body)                   its only pred (header) is known
body: ...; jmp header
seal(header)                 now both preds are known: fill pending phis
seal(exit)
```

`&&` and `||` reuse the same machinery: the result is a temporary variable
written on both paths and read at the join, so the phi comes from `read_var`.

### A bug the fuzzer found here

The first version passed every hand-written test and failed on about 1 in 12
random programs with "uses removed value". The cause: in `f(a, b)` the builder
evaluates `a`, which returns a phi, and holds that pointer in a C++ local
while it evaluates `b`. Evaluating `b` can seal a block, which can make that
phi trivial and delete it. `replace_all_uses` fixes every *instruction* that
used the phi, but the pointer sitting in a local variable is not an
instruction yet. The fix (see `resolve()`) is a forwarding table from each
removed phi to its replacement, applied whenever a value is finally used. The
paper's pseudocode has the same hazard; implementations in languages with
handles or use lists tend to hide it.

## Optimization passes

`src/opt.cpp`. Run in a loop until nothing changes (at `-O1` and up).

**Constant folding.** An instruction whose operands are all constants is
replaced by its result, computed with the same `eval_binop` the interpreter
uses (`src/value.h`), so folding can never disagree with execution. An
operation that would trap (`1 / 0`, `nil + 1`) is left alone so the error still
happens at run time. A phi whose inputs are all the same constant also folds.

**CFG simplification.** A branch on a constant becomes a jump, and the dead
edge is removed from the target's predecessor list along with the matching
phi operands. Then unreachable blocks are deleted, trivial phis are removed,
and a block whose only predecessor jumps straight to it is merged into that
predecessor.

**Dead code elimination.** Mark every instruction with a side effect as live,
then everything they use, transitively; delete the rest. The subtle part is
deciding what has a side effect. In a dynamically typed language `a + b` can
raise a type error, so an unused `a + b` cannot be deleted unless both
operands are known to be ints. That is what `compute_known_int` is for: an
optimistic fixpoint that marks constants, arithmetic results, comparison
results and phis of those as ints.

**Guard elimination** (`prove_int_operands`). `x < n` traps unless both are
ints, so after it runs, `x` and `n` are ints in every block it dominates.
The pass walks the dominator tree carrying the set of values proven so far
and records, per instruction, which operands are proven. The backends use
this to drop type checks. In `fib(n)`, the check on `n` in `n < 2` makes the
checks in `n - 1` and `n - 2` unnecessary: the JIT emits one guard on `n`
instead of three.

Dominators come from Cooper, Harvey and Kennedy, "A Simple, Fast Dominance
Algorithm" (2001): number blocks in reverse postorder, then repeatedly set
each block's idom to the intersection of its processed predecessors' idoms,
walking up the partial tree by RPO number. A few lines, and fast in practice.

**Inlining** (`inline_module`). Functions are visited callees first (postorder
over the call graph), so a callee is already optimized, and has already had
its own callees inlined, when it is copied into a caller. `inline_call` splits
the caller's block at the call, clones the callee's blocks, replaces its
parameters with the arguments, turns each `ret v` into a jump to the
continuation block, and merges the returned values with a phi there. The
caller is then re-optimized, which is where inlining pays off: constant
arguments fold, and branches on them disappear.

`--dump-ir` shows SSA straight out of the builder; `--dump-opt` shows it after
these passes. `examples/sum.tiny` is a good one to compare.
