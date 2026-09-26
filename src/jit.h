// x86-64 JIT for MIR.
//
// Pipeline per function: MIR -> linear scan over 9 machine registers ->
// instruction selection straight to machine code -> mmap'd buffer, flipped
// from writable to executable (W^X) once the bytes are in.
//
// Compiled code follows the System V AMD64 ABI, so the interpreter can call
// it through an ordinary C function pointer and it can call the C runtime
// (rt_print, rt_error). A function is compiled together with every function
// it can reach, so JIT code never has to call back into the interpreter.
// Functions that allocate (cons) are left to the interpreter: the JIT emits
// no stack maps, so the collector could not find roots in JIT frames.
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
  size_t offset;  // of an 8-byte absolute address inside the code region
  std::string symbol;
};

class JIT {
 public:
  JIT(VM& vm, std::vector<MFunc>& mir);
  ~JIT();
  bool compile(int fidx);  // false if fidx (or something it calls) cannot be jitted
  void compile_all();

  bool write_elf(const std::string& path, std::string& err) const;
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

  bool collect_group(int f, std::vector<char>& in_group, std::vector<int>& group);
};
