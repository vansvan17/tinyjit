# Binaries

`src/jit.cpp` (`write_object`), `tools/machodump.cpp`, `tools/compare_asm.sh`, `aot/runtime.c`

## Looking at JIT output

`--emit-obj=out.o` compiles every function and writes a Mach-O object:

```
build/tinyjit --emit-obj=fib.o bench/fib.tiny
objdump -d -r fib.o
otool -tvV fib.o
nm fib.o
```

`-r` shows which `bl` calls the runtime:

```
      70:  mov   x0, #0x1
      74:  bl    0x74 <_tiny_fib+0x74>
           0000000000000074:  ARM64_RELOC_BRANCH26  _rt_error
```

`--dump-jit=code.bin` writes the raw code region from a normal run.

`make asm` prints the JIT's `fib` next to clang's. clang `-O0` keeps `n` on
the stack and reloads it; the JIT keeps it in x19 across the calls. The JIT
has type guards C doesn't need, and its constants are doubled (`n < 2` is
`cmp x19, #4`). clang `-O2` turns one of the two recursive calls into a loop,
which is most of the speed difference.

## Mach-O

A 32-byte header (magic `0xfeedfacf`, CPU type, file type, number of load
commands) followed by load commands, each starting with `{cmd, cmdsize}`:

- `LC_SEGMENT_64`: a range to map and its sections. Executables have
  `__TEXT`, `__DATA`, `__LINKEDIT`; an object has one unnamed segment.
- `LC_SYMTAB`: `nlist_64` symbols and their strings.
- `LC_DYSYMTAB`: which symbol ranges are local, defined, undefined.
- Executables also have `LC_MAIN`, `LC_LOAD_DYLIB`, `LC_UUID`,
  `LC_CODE_SIGNATURE` and dyld fixups.

What `write_object` writes:

| part | contents |
|---|---|
| header | arm64, `MH_OBJECT`, 4 load commands, `MH_SUBSECTIONS_VIA_SYMBOLS` |
| `LC_SEGMENT_64` | one section, `__TEXT,__text`, 16-byte aligned |
| `LC_BUILD_VERSION` | macOS 11.0 |
| `LC_SYMTAB`, `LC_DYSYMTAB` | `_tiny_<name>` per function, then undefined `_rt_print`, `_rt_error` |
| relocations | `ARM64_RELOC_BRANCH26` per runtime call |

Symbols must be ordered local, defined, undefined, and relocations refer to
them by index in that order. C symbols get a leading underscore on Apple
platforms.

In memory, runtime calls go through a trampoline. In the object the `bl`
instead carries a `BRANCH26` relocation and the linker resolves it, adding a
branch island if the target is out of range. The trampolines stay in the file
as dead code with zeroed addresses. Calls between compiled functions are
already PC-relative.

`build/machodump` reads headers, load commands, symbols and relocations. On a
universal binary like `/bin/ls` it picks the arm64 slice.

## Linking

```
build/tinyjit --emit-obj=fib.o bench/fib.tiny
cc aot/runtime.c fib.o -o fib
./fib
```

`aot/runtime.c` provides `rt_print`, `rt_error` and a `main` that calls
`tiny_main`. Only programs whose functions are all JIT-compiled (no `cons`)
can be linked this way.

## Calling convention

Apple arm64 (AAPCS64 with Apple's changes):

| | |
|---|---|
| arguments | x0-x7 |
| return value | x0 |
| return address | x30, set by `bl` |
| caller-saved | x0-x17 |
| callee-saved | x19-x28, x29, sp |
| reserved | x18 |
| stack | sp 16-byte aligned at all times |

How the JIT follows it:

- The interpreter calls JIT code through a C function pointer, so arguments
  arrive in x0-x7 and the prologue moves them where the allocator wants them.
- `bl` puts the return address in x30, so every prologue saves x29/x30, and
  x29 always points at a valid frame record for debuggers and profilers.
- Callee-saved registers the allocator used are saved and restored.
- Values live across a call only get callee-saved registers or stack slots,
  so nothing is saved around calls.
- x16 and x17 may be clobbered by linker veneers; the JIT uses x16 for its
  own trampolines.
- Frames are multiples of 16 bytes.

Apple differs from standard AAPCS64 in that variadic arguments always go on
the stack, small stack arguments are packed, and x18 is reserved. None of
that affects this JIT.
