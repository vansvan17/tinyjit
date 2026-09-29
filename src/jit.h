// ARM64 JIT for MIR (Apple Silicon; Linux arm64 works too, for testing).
//
// Pipeline per function: MIR -> linear scan over 22 machine registers ->
// instruction selection straight to A64 machine code -> a MAP_JIT region,
// writable or executable for this thread but never both (W^X).
//
// Compiled code follows the Apple arm64 calling convention, so the
// interpreter can call it through an ordinary C function pointer and it can
// call the C runtime (rt_print, rt_error). A function is compiled together
// with every function it can reach, so JIT code never has to call back into
// the interpreter. Functions that allocate (cons) are left to the
// interpreter: the JIT emits no stack maps, so the collector could not find
// roots in JIT frames.
#pragma once
#include <string>
#include <vector>
#include "mir.h"
#include "vm.h"

struct JitSymbol {
  std::string name;
  size_t offset;  // from the start of the code region
  size_t size;
};

struct JitReloc {
  size_t offset;  // of a `bl` to a runtime helper, from the start of the code region
  std::string symbol;
};

// True on arm64; false where jit.cpp builds its stub (every function interpreted).
bool jit_supported();

class JIT {
 public:
  JIT(VM& vm, std::vector<MFunc>& mir);
  ~JIT();
  bool compile(int fidx);  // false if fidx (or something it calls) cannot be jitted
  void compile_all();

  bool write_object(const std::string& path, std::string& err) const;  // Mach-O .o
  bool write_raw(const std::string& path) const;

  size_t code_bytes() const { return used_; }
  size_t function_bytes() const {
    size_t n = 0;
    for (auto& s : syms_) n += s.size;
    return n;
  }
  int functions_compiled = 0;
  int spills = 0;
  double compile_ms = 0;  // time spent in the JIT itself
  bool print_intervals = false;

 private:
  VM& vm_;
  std::vector<MFunc>& mir_;
  uint8_t* mem_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  std::vector<JitSymbol> syms_;
  std::vector<JitReloc> relocs_;
  std::vector<size_t> literals_;  // 8-byte runtime addresses inside trampolines

  bool collect_group(int f, std::vector<char>& in_group, std::vector<int>& group);
};
