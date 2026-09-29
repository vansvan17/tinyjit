# Binary analysis: objdump, Mach-O, and the calling convention

Files: `src/jit.cpp` (`write_object`), `tools/machodump.cpp`, `tools/compare_asm.sh`, `aot/runtime.c`

## Reading your own JIT output

`--emit-obj=out.o` compiles every function with the JIT backend and writes a
relocatable Mach-O object instead of running. Because it is a real object
file with a symbol table, the standard tools work on it:

```
build/tinyjit --emit-obj=fib.o bench/fib.tiny
objdump -d -r fib.o          # Xcode's objdump is llvm-objdump
otool -tvV fib.o             # Apple's own disassembler
nm fib.o                     # symbols: T _tiny_fib, U _rt_print, ...
```

`-r` prints relocations inline, which is how you see which `bl` is a call
into the runtime:

```
      70:  mov   x0, #0x1
      74:  bl    0x74 <_tiny_fib+0x74>
           0000000000000074:  ARM64_RELOC_BRANCH26  _rt_error
```

`--dump-jit=code.bin` writes the raw bytes of whatever tiered mode compiled
during a run, without symbols.

`tools/compare_asm.sh` (or `make asm`) prints the JIT's `fib` next to clang's
at `-O0` and `-O2`. Worth noticing:

- clang `-O0` keeps `n` on the stack (`str x0, [sp, #0x10]`) and reloads it
  before every use. The JIT keeps it in x19 for the whole function, because
  x19 survives the recursive calls.
- The JIT's code has type guards (`tst x19, #1; b.ne`) that C does not need,
  and its constants are doubled: `n < 2` is `cmp x19, #4` because 2 is
  stored as 4.
- clang `-O2` turns the second recursive call into a loop with an
  accumulator (`fib(n-1) + fib(n-2)` becomes "call fib(n-1), add, repeat with
  n-2"), so it makes half as many calls. That transformation, not better
  register allocation, is most of the gap in the benchmark table.

## Mach-O in one page

A Mach-O file starts with a 32-byte header: the magic `0xfeedfacf` (64-bit),
the CPU type (`0x0100000c`, arm64), the file type (`MH_OBJECT` for a `.o`,
`MH_EXECUTE` for a program, `MH_DYLIB` for a library), and how many load
commands follow. Everything else is found through load commands, each of
which starts with `{cmd, cmdsize}` so a reader can skip the ones it does not
know:

- `LC_SEGMENT_64` describes a range of the file to map into memory and the
  sections inside it. An executable has `__TEXT` (code, read+execute),
  `__DATA` (writable), `__LINKEDIT` (symbols, fixups, code signature). An
  object file has one unnamed segment holding all its sections.
- `LC_SYMTAB` points at the symbol table (an array of `nlist_64`) and its
  string table.
- `LC_DYSYMTAB` says which ranges of the symbol table are local, defined
  externally visible, and undefined.
- Executables add `LC_MAIN` (the entry point), `LC_LOAD_DYLIB` (each
  library to load, such as `/usr/lib/libSystem.B.dylib`), `LC_UUID`,
  `LC_CODE_SIGNATURE` and the fixup tables dyld applies at load time.

What `write_object` produces (compare with `build/machodump fib.o`):

| part | what it holds |
|---|---|
| `mach_header_64` | arm64, `MH_OBJECT`, 4 load commands, `MH_SUBSECTIONS_VIA_SYMBOLS` (the linker may split the section at symbols and drop unused functions) |
| `LC_SEGMENT_64` | one section, `__TEXT,__text`, all the code, 16-byte aligned |
| `LC_BUILD_VERSION` | platform macOS, minimum 11.0, so the linker does not warn |
| `LC_SYMTAB`, `LC_DYSYMTAB` | `_tiny_<name>` for each function (defined, external), then `_rt_print` and `_rt_error` (undefined) |
| relocations | one `ARM64_RELOC_BRANCH26` per call into the runtime |

C symbols get a leading underscore on Apple platforms, so the runtime's
`rt_print` is `_rt_print` in the symbol table, and `tiny_main` is
`_tiny_main`.

`tools/machodump.cpp` is a ~200 line reader that walks the header, the load
commands, symbols and relocations, with the structs written out in the file
rather than included from `<mach-o/loader.h>`. Try it on `/bin/ls`: that is a
universal ("fat") file, a big-endian table of per-architecture slices (x86_64
and arm64), each a normal Mach-O file. machodump picks the arm64 slice and
shows the load commands a real executable has.

**Relocations.** Inside the JIT, a call into the runtime is a `bl` to a
trampoline that loads the runtime function's full 64-bit address (see
`docs/02-backend.md`). In the object file the `bl` instead gets a
`ARM64_RELOC_BRANCH26` relocation naming `_rt_print`: "put the distance to
this symbol in the 26-bit offset of this branch". The linker resolves it
like any other call. If the target ends up further than `bl` can reach
(+-128 MB), the linker inserts its own trampoline, a branch island, which is
the same trick the JIT uses. Calls between compiled functions are already
PC-relative `bl`s within `__text` and need no relocation. The trampolines
stay in the object as dead code, with their address bytes zeroed.

A detail that is easy to get wrong: symbols must be ordered local, then
defined external, then undefined, with `LC_DYSYMTAB` giving each range, and a
relocation refers to its symbol by index in that order.

## Linking it: an AOT compiler for free

Since the object is real, it can be linked:

```
build/tinyjit --emit-obj=fib.o bench/fib.tiny
cc aot/runtime.c fib.o -o fib
./fib
```

(`make aot` does this.) `aot/runtime.c` supplies `rt_print`, `rt_error` and a
C `main` that calls `tiny_main`. The linker also ad-hoc code signs the
result, which macOS on Apple Silicon requires of every executable.

## The Apple arm64 calling convention

Apple follows the standard ARM procedure call standard (AAPCS64) with a few
changes. What the JIT relies on:

| | registers |
|---|---|
| integer arguments 1-8 | x0-x7 (more go on the stack) |
| return value | x0 |
| return address | x30 (lr), set by `bl` |
| caller-saved (a call may clobber) | x0-x17 |
| callee-saved (must be restored) | x19-x28, x29 (fp), sp |
| reserved | x18 on Apple platforms: never read or write it |
| stack | sp is 16-byte aligned at all times (the hardware checks) |

Where each rule shows up in the code:

- **Arguments.** The interpreter calls compiled code through a plain
  function pointer (`VM::call_native`), so the C compiler puts arguments in
  x0-x7, and the JIT's prologue moves them to wherever the register
  allocator wants them (a parallel move).
- **Return address.** Unlike x86, `bl` does not push anything; it puts the
  return address in x30. A function that makes calls must save x30 before
  the next `bl` overwrites it, which is what `stp x29, x30, [sp, #-16]!` in
  every prologue does. A leaf function could skip it.
- **Frame pointer.** Apple requires x29 to point at a valid frame record
  (the saved x29 and x30), so debuggers, profilers and crash reports can
  walk the stack. The JIT always sets it up.
- **Callee-saved.** Any of x19-x28 the allocator used are stored in the
  prologue and reloaded in the epilogue.
- **Caller-saved.** Values that must survive a call are only ever given
  callee-saved registers or stack slots, so the JIT never saves anything
  around a call.
- **x16, x17.** These are the "intra-procedure-call" scratch registers: a
  linker veneer or branch island may overwrite them between a `bl` and its
  target. The JIT uses x16 in its own trampolines for the same reason.
- **Alignment.** sp must stay 16-byte aligned or the next memory access
  through it faults. The prologue reserves frames in multiples of 16.

Apple's changes to AAPCS64 that matter to other code but not here:
variadic arguments (as in `printf`) always go on the stack rather than in
registers, arguments smaller than 64 bits are packed on the stack, and x18
is off-limits.
