# Runtime: values, the VM, and the garbage collector

Files: `src/value.h`, `src/vm.{h,cpp}`, `src/interp.inc`, `src/gc.{h,cpp}`, `src/runtime.cpp`

## Tagged values

Every value is 64 bits:

```
int:   n << 1          low bit 0   (so ints are 63-bit and wrap around)
pair:  address | 1     low bit 1   (cells are 8-byte aligned, so bit 0 is free)
nil:   1               the "null pair"
```

Tagging ints with a 0 bit means the common operations work on the raw bits:
`2a + 2b = 2(a + b)`, `2a - 2b = 2(a - b)`, and comparing `2a < 2b` is the
same as `a < b`. Multiply needs one shift (`(2a >> 1) * 2b = 2ab`), divide
needs to untag both sides and retag the result. The type check before an add
is `(a | b) & 1`: one `or` and one `test`. This is the same trick as OCaml
(with the bits the other way round) and V8's Smis.

`src/value.h` defines every primitive in one place (`eval_binop`), and the
interpreter, the constant folder and the tests are all checked against it.
The JIT reimplements the same rules in machine code, which is why the
differential fuzzer exists.

## The bytecode VM

Register-based, in the style of Lua 5. Each instruction names its operands
directly:

```
jle   r3, r0 -> 6       if r3 <= r0 goto 6
add   r4, r4, r3        r4 = r4 + r3
addk  r3, r3, 1         r3 = r3 + 1 (constant operand)
```

A stack VM would spend a push for each operand and a pop per result; for
`s = s + i` that is 4 instructions (`load s, load i, add, store s`) against 1.
Fewer instructions means fewer trips through the dispatch branch, which is
most of an interpreter's overhead.

A frame is a window of `frame_size` registers on one big Value array. For a
call, the caller writes arguments with `ARG` into the slots just past its
own frame, and `CALL` makes them r0..rN of the callee's window. Nothing is
copied on entry. Frames (return pc, base, function) live on a separate array,
not the C stack, so deep recursion in tiny code does not overflow C's stack.

### switch vs computed goto

`src/interp.inc` is compiled twice (see the bottom of `src/vm.cpp`):

- `switch`: a `for (;;) switch (op)` loop. Every handler jumps back to the
  top, where one indirect jump dispatches the next instruction. The branch
  predictor sees a single indirect branch whose target depends on whatever
  instruction comes next: hard to predict.
- computed goto (`goto *labels[op]`, a GCC/Clang extension): every handler
  ends with its own indirect jump. The predictor now tracks one branch per
  opcode, and "what comes after a compare-and-branch" is much more
  predictable than "what comes next, anywhere".

On an M5, computed goto runs `bench/fib.tiny` in 27.1 ms against 39.1 ms for
`switch`, and `bench/loop.tiny` in 205 ms against 295 ms: about 30% faster on
both. Two things contribute. Each dispatch is fewer instructions (the
`switch` version also bounds-checks the opcode and jumps back to a shared
loop head), and each opcode gets its own indirect branch for the predictor
to learn.

How much of the gap is prediction depends on the CPU. The classic result is
Ertl and Gregg, "The Structure and Performance of Efficient Interpreters"
(2003). Rohou, Swamy and Seznec, "Branch Prediction and the Performance of
Interpreters: Don't Trust Folklore" (CGO 2015) found that modern predictors,
which use global branch history, predict even the single `switch` branch
well, so on recent cores much of what is left is instruction count. To split
the two on a Mac, Instruments' CPU Counters template can count branch
mispredictions for each build.

## Tiering

Every function starts in the interpreter. Each call bumps a counter; at 100
(`--jit-threshold`) the JIT compiles it plus everything it can call, and the
`CALL` instruction jumps to native code from then on through an ordinary C
function pointer. `--jit=eager` compiles everything up front, `--jit=off`
never compiles.

## The garbage collector

`src/gc.cpp`. Stop-the-world mark and sweep over fixed-size pair cells.

- **Allocation.** Cells come from chunks and are handed out from a free list
  threaded through their `car` field. Allocation is a pointer pop.
- **Roots.** The VM's register stack, from the bottom to the end of the
  current frame. Because every slot holds a tagged Value, the collector can
  tell pointers from ints exactly: this is a precise collector, not a
  conservative one. There is one catch: a newly pushed frame could contain
  stale pointers left by an earlier, deeper call, pointing at cells freed
  since. The VM zeroes each new frame (only when the program allocates at
  all), so the root set is always valid.
- **Mark.** An explicit stack instead of recursion, so a 10,000-cell list
  does not overflow the C stack.
- **Sweep.** Visit every cell in every chunk. Unmarked cells go back on the
  free list; marked ones get their mark cleared.
- **Growth.** If fewer than half the cells are free after a collection, the
  heap doubles.

`--gc-stress` collects on every single allocation. If any root is ever missed,
the next collection frees a live cell, and the collector aborts the moment it
finds a live reference to a freed cell. The test suite runs every program
this way.

`--gc-log` prints one line per collection. From `bench/gc_live.tiny`, which
grows a live list while making garbage (every fourth line, some columns
trimmed, on an M5):

```
[gc] #1  live=524     heap=2048 cells    pause=0.004 ms
[gc] #5  live=2096    heap=8192 cells    pause=0.019 ms
[gc] #9  live=8584    heap=32768 cells   pause=0.073 ms
[gc] #13 live=34636   heap=131072 cells  pause=0.301 ms
[gc] #17 live=138844  heap=524288 cells  pause=1.243 ms
```

Pause time grows linearly with the heap. Mark is proportional to live data,
sweep to the whole heap, and the program is stopped for both. That is the
fundamental problem with a simple tracing collector: the pause scales with
memory, not with how much garbage you made.

## Why Go pauses and Rust does not (and why "pause vs zero cost" is the wrong frame)

The honest comparison is about **when and where the cost of freeing memory
is paid**, not whether there is a cost.

**This collector** pays in one lump: every allocation is fast, and then the
program stops for a time proportional to the heap.

**Go** also traces, but concurrently. Marking runs on other cores while the
program keeps going, using the tri-color abstraction: objects are white
(not yet seen), grey (seen, children not scanned) or black (done). The danger
is the program storing a pointer to a white object into a black one behind
the collector's back, so Go's compiler inserts a **write barrier** on pointer
stores while marking is on, which shades the relevant objects grey. The
stop-the-world phases shrink to short windows (typically well under a
millisecond) to turn the barrier on and off. The cost moves elsewhere:

- the write barrier on every pointer store during a cycle;
- **mutator assists**: a goroutine that allocates quickly during marking is
  made to do some marking itself, so allocation can outrun the collector
  only so far. This shows up as latency on allocating goroutines, not as a
  global pause;
- CPU: roughly 25% of GOMAXPROCS is dedicated to marking during a cycle;
- memory headroom: `GOGC=100` lets the heap grow to twice the live data
  before the next cycle. Less headroom means more frequent collections.

**Rust** has no tracing collector. Ownership decides at compile time where
each value dies, and the compiler inserts the `drop` call there. Freeing
still happens at run time (it is a call into the allocator, and dropping a
big `Vec<Box<T>>` walks all of it), but it happens at a predictable point, on
the thread that owned the value, proportional to what is being freed rather
than to everything that is alive. Nothing scans the heap. When ownership
cannot be static, you opt into run-time cost explicitly: `Rc`/`Arc` pay a
counter update (atomic for `Arc`) on every clone and drop, and cycles leak
unless broken with `Weak`.

"Zero-cost abstractions" means you do not pay for what you do not use, and
what you use costs no more than writing it by hand. It does not mean freeing
memory is free: a program that builds and drops a million-node tree does a
million frees in Rust too, just spread out and deterministic instead of in
one pause. The tradeoff Go makes is throughput and memory headroom for
programmer time and low pauses; the one Rust makes is compile-time
constraints (the borrow checker) for predictability.

To make this collector behave more like Go's, the steps would be: make
marking incremental (do a bit per allocation, which needs a write barrier on
`setcar`/`setcdr` style mutation, which this language happens not to have),
then concurrent (mark on another thread, which also needs the barrier and a
way to pause at safepoints), then lazy sweeping (sweep a chunk when
allocation needs one, instead of all at once).
