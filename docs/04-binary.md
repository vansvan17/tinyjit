# Binary analysis: objdump, ELF, and the calling convention

Files: `src/jit.cpp` (`write_elf`), `tools/elfdump.cpp`, `tools/compare_asm.sh`, `aot/runtime.c`

## Reading your own JIT output

`--emit-elf=out.o` compiles every function with the JIT backend and writes a
relocatable ELF object instead of running. Because it is a real object file
with a symbol table, the normal tools work on it:

```
build/tinyjit --emit-elf=fib.o bench/fib.tiny
objdump -d -r -M intel fib.o
```

`-r` shows relocations inline, which is how you see which absolute address
was a call into the runtime. `--dump-jit=code.bin` writes the raw bytes of
whatever tiered mode compiled during a run, for
`objdump -D -b binary -m i386:x86-64 code.bin`, which has no symbols.

`tools/compare_asm.sh` (or `make asm`) prints the JIT's `fib` next to gcc's
at `-O0` and `-O2`. Things worth noticing:

- gcc `-O0` keeps `n` in memory (`[rbp-0x18]`) and reloads it each time. The
  JIT keeps it in `rbx` for the whole function, because `rbx` survives the
  recursive calls.
- The JIT's code has type guards (`test reg, 1; jne`) that gcc does not need,
  and a `sar`-free tagged add. Its constants are doubled: `n < 2` is
  `cmp rbx, 4` because 2 is stored as 4.
- gcc `-O2` does something no toy will: it partially unrolls the recursion,
  inlining fib into itself several levels deep. That, not better register
  allocation, is most of the gap in the benchmark table.

## ELF in one page

An ELF file starts with a fixed 64-byte header (`Elf64_Ehdr`): the magic
`\x7fELF`, class (64-bit), endianness, type (`ET_REL` for an object,
`ET_EXEC`/`ET_DYN` for executables and PIEs), machine (`EM_X86_64`), and the
offsets of two tables:

- **Section headers** describe the file for the linker: named regions such
  as `.text`, `.data`, `.symtab`. Section names are themselves strings in a
  section (`.shstrtab`), whose index is in the header.
- **Program headers** (segments) describe the file for the loader: which
  byte ranges to map at which addresses with which permissions. Objects
  (`.o`) have none; executables need them.

What `write_elf` produces (compare with `build/elfdump fib.o` or
`readelf -a fib.o`):

| # | section | what it holds |
|---|---|---|
| 1 | `.text` | all compiled code, flags `AX` (allocated, executable) |
| 2 | `.rela.text` | fixups to apply to section 1 (`sh_info`) using symbols from section 3 (`sh_link`) |
| 3 | `.symtab` | `tiny_<name>` for each function (`STT_FUNC`, `STB_GLOBAL`, value = offset in `.text`) and undefined `rt_print`, `rt_error` |
| 4 | `.strtab` | symbol names |
| 5 | `.shstrtab` | section names |
| 6 | `.note.GNU-stack` | empty; tells the linker this code does not need an executable stack |

Two details that are easy to get wrong: the symbol table must list all local
symbols before global ones, with `sh_info` holding the index of the first
global; and with RELA relocations the bytes at the fixup location should be
zero because the value comes from `r_addend`.

**Relocations.** JIT code calls the runtime with `movabs rax, <address>;
call rax`. In memory the JIT writes the real address. In the object file those
8 bytes are zeroed and a `R_X86_64_64` relocation says "put the absolute
address of `rt_print` here". Calls between compiled functions are
PC-relative (`call rel32`) and already correct wherever `.text` ends up, so
they need no relocation.

`tools/elfdump.cpp` is a ~200 line reader that walks the header, the section
table, symbol tables and RELA sections. It works on `/bin/ls` too, which is
a good way to see `.dynsym`, `.plt`, `.got` and the other sections a dynamic
executable has that this object does not.

## Linking it: an AOT compiler for free

Since the object is real, it can be linked:

```
build/tinyjit --emit-elf=fib.o bench/fib.tiny
cc -no-pie aot/runtime.c fib.o -o fib
./fib
```

(`make aot` does this.) `aot/runtime.c` supplies `rt_print`, `rt_error` and a
C `main` that calls `tiny_main`. `-no-pie` is needed because an absolute
`R_X86_64_64` relocation inside `.text` cannot be resolved at link time in a
position-independent executable without text relocations. The fix a real
compiler uses is to call through the PLT with a PC-relative relocation
(`R_X86_64_PLT32`), which is what `call rt_print` compiles to in C.

## The System V AMD64 calling convention

What the JIT has to follow to call C, and be called from C:

| | registers |
|---|---|
| integer arguments 1-6 | rdi, rsi, rdx, rcx, r8, r9 (more go on the stack) |
| return value | rax (rdx:rax for 128-bit) |
| caller-saved (a call may clobber) | rax, rcx, rdx, rsi, rdi, r8-r11 |
| callee-saved (must be restored) | rbx, rbp, r12-r15 |
| stack | rsp is 16-byte aligned at the `call` instruction |

Where each rule shows up in the code:

- **Arguments.** The interpreter calls compiled code through a plain
  function pointer (`VM::call_native`), so the C compiler puts arguments in
  rdi, rsi, ... and the JIT's prologue moves them to wherever the register
  allocator wants them (a parallel move).
- **Callee-saved.** Any of rbx, r12-r15 the allocator used are stored in the
  prologue and reloaded in the epilogue.
- **Caller-saved.** Values that must survive a call are only ever given
  callee-saved registers or stack slots, so the JIT never has to save
  anything around a call.
- **Alignment.** On entry rsp is 8 mod 16 (the `call` pushed a return
  address). `push rbp` makes it 0 mod 16, and the frame size is rounded to a
  multiple of 16, so rsp is aligned for the whole body. Get this wrong and
  the first `printf` that uses SSE instructions on the stack (`movaps`)
  crashes, which is the classic symptom.
