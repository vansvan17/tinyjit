# Runtime

`src/value.h`, `src/vm.{h,cpp}`, `src/interp.inc`, `src/gc.{h,cpp}`, `src/runtime.cpp`

## Values

```
int:   n << 1         low bit 0, 63-bit, wraps
pair:  address | 1    low bit 1
nil:   1
```

With ints tagged by a 0 bit, add, subtract and compare work on the raw bits.
Multiply needs one shift, divide untags and retags. Checking that two values
are ints is `(a | b) & 1`.

`eval_binop` in `value.h` defines every primitive. The interpreter and the
constant folder both use it; the JIT reimplements it, and the fuzzer checks
they agree.

## VM

Register-based, like Lua 5:

```
jle   r3, r0 -> 6
add   r4, r4, r3
addk  r3, r3, 1
```

`s = s + i` is one instruction instead of four on a stack VM. A frame is a
window of registers on one array; the caller writes arguments past its own
frame with `ARG`, and `CALL` makes them the callee's r0..rN. Call frames live
in their own array, not on the C stack.

`interp.inc` is compiled twice: once as a `switch` loop and once with computed
goto (`goto *labels[op]`), where every handler ends in its own indirect jump.
On an M5 computed goto is about 30% faster (fib 27.1 ms vs 39.1 ms, loop
205 ms vs 295 ms). Part of that is fewer instructions per dispatch, part is
branch prediction; see Rohou, Swamy and Seznec, CGO 2015, on how much modern
predictors close the gap.

Every function starts interpreted. After 100 calls (`--jit-threshold`) the JIT
compiles it and `CALL` jumps to native code. `--jit=eager` compiles
everything up front, `--jit=off` nothing.

## GC

Stop-the-world mark-sweep over fixed-size pair cells.

- Allocation pops a free list threaded through `car`.
- Roots are the VM register stack up to the current frame. Values are tagged,
  so the scan is precise. New frames are zeroed so stale pointers from
  earlier calls don't look live.
- Marking uses an explicit stack, so long lists don't overflow the C stack.
- Sweep walks every cell; unmarked cells go back on the free list.
- The heap doubles when less than half is free after a collection.

`--gc-stress` collects on every allocation and aborts if it finds a reference
to a freed cell. All tests run this way.

`--gc-log` on `bench/gc_live.tiny` (every fourth line, M5):

```
[gc] #1  live=524     heap=2048 cells    pause=0.004 ms
[gc] #5  live=2096    heap=8192 cells    pause=0.019 ms
[gc] #9  live=8584    heap=32768 cells   pause=0.073 ms
[gc] #13 live=34636   heap=131072 cells  pause=0.301 ms
[gc] #17 live=138844  heap=524288 cells  pause=1.243 ms
```

Mark is proportional to live data and sweep to the whole heap, so pauses grow
with the heap.

## Go and Rust

The difference is when the cost of freeing is paid.

- This collector pays it in one pause proportional to the heap.
- Go marks concurrently with the program. A write barrier on pointer stores
  during marking keeps it correct, so global pauses are short. The cost moves
  to the barrier, to mutator assists (goroutines that allocate fast during a
  cycle do some marking), to about 25% of CPU during a cycle, and to heap
  headroom (`GOGC=100` lets the heap reach twice the live data).
- Rust has no tracing collector. Ownership decides where each value is
  dropped, and the free happens there, on the owning thread, proportional to
  what is freed. `Rc`/`Arc` add reference counting where ownership isn't
  static, and cycles leak unless broken with `Weak`.

Making this collector more like Go's would mean incremental marking (needs a
write barrier, which this language avoids by having no pair mutation), then
concurrent marking with safepoints, then lazy sweeping.
