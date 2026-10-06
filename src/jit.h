#pragma once
#include <string>
#include <vector>
#include "mir.h"
#include "vm.h"

struct JitSymbol {
  std::string name;
  size_t offset;
  size_t size;
};

struct JitReloc {
  size_t offset;
  std::string symbol;
};

bool jit_supported();

class JIT {
 public:
  JIT(VM& vm, std::vector<MFunc>& mir);
  ~JIT();
  bool compile(int fidx);
  void compile_all();

  bool write_object(const std::string& path, std::string& err) const;
  bool write_raw(const std::string& path) const;

  size_t code_bytes() const { return used_; }
  size_t function_bytes() const {
    size_t n = 0;
    for (auto& s : syms_) n += s.size;
    return n;
  }
  int functions_compiled = 0;
  int spills = 0;
  double compile_ms = 0;
  bool print_intervals = false;

 private:
  VM& vm_;
  std::vector<MFunc>& mir_;
  uint8_t* mem_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  std::vector<JitSymbol> syms_;
  std::vector<JitReloc> relocs_;
  std::vector<size_t> literals_;

  bool collect_group(int f, std::vector<char>& in_group, std::vector<int>& group);
};
