// Stop-the-world mark-sweep collector for pair cells.
//
// All heap objects are 24-byte pairs carved out of chunks. Free cells form a
// linked list threaded through `car`. Collection happens when the free list
// is empty (or on every allocation with --gc-stress):
//   mark:  every Value in [roots_begin, roots_end) that is a pointer is
//          pushed on an explicit stack (no recursion, so long lists are fine)
//          and traced through car/cdr.
//   sweep: walk every chunk; unmarked cells go back on the free list.
// If less than half the heap is free afterwards, the heap doubles.
//
// Roots are precise: the VM's register stack holds only tagged Values, so a
// pointer can be told apart from an int by its low bit. The VM clears each
// new frame so stale pointers from dead frames never look like roots.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "value.h"

struct GCStats {
  uint64_t collections = 0;
  uint64_t allocated = 0;
  uint64_t freed = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  std::vector<uint64_t> pauses_ns;
};

class Heap {
 public:
  Heap();
  ~Heap();
  Value alloc(Value car, Value cdr);
  void collect();
  size_t heap_cells() const { return total_; }
  size_t free_cells() const { return free_count_; }

  Value* roots_begin = nullptr;
  Value* roots_end = nullptr;
  bool stress = false;
  bool log = false;
  GCStats stats;

 private:
  struct Chunk { Pair* cells; size_t n; };
  std::vector<Chunk> chunks_;
  Pair* free_list_ = nullptr;
  size_t total_ = 0;
  size_t free_count_ = 0;
  std::vector<Pair*> mark_stack_;
  void add_chunk(size_t n);
};
