#include "gc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

static constexpr uint64_t MARK = 1;
static constexpr uint64_t FREE = 2;
static constexpr size_t kFirstChunk = 1024;

Heap::Heap() { add_chunk(kFirstChunk); }

Heap::~Heap() {
  for (auto& c : chunks_) free(c.cells);
}

void Heap::add_chunk(size_t n) {
  Pair* cells = (Pair*)aligned_alloc(16, ((n * sizeof(Pair) + 15) / 16) * 16);
  if (!cells) { fprintf(stderr, "out of memory\n"); exit(1); }
  for (size_t i = 0; i < n; i++) {
    cells[i].header = FREE;
    cells[i].car = (Value)free_list_;
    cells[i].cdr = NIL;
    free_list_ = &cells[i];
  }
  chunks_.push_back({cells, n});
  total_ += n;
  free_count_ += n;
}

Value Heap::alloc(Value car, Value cdr) {
  if (stress || !free_list_) collect();
  if (!free_list_) add_chunk(total_);
  Pair* p = free_list_;
  free_list_ = (Pair*)p->car;
  free_count_--;
  p->header = 0;
  p->car = car;
  p->cdr = cdr;
  stats.allocated++;
  return mk_pair(p);
}

void Heap::collect() {
  auto t0 = std::chrono::steady_clock::now();
  auto push = [&](Value v) {
    if (!is_pair(v)) return;
    Pair* p = as_pair(v);
    if (p->header & FREE) {
      // A root or field points at a cell we already freed. This is exactly
      // the bug class --gc-stress exists to catch, so fail loudly.
      fprintf(stderr, "gc: live reference to a freed cell %p\n", (void*)p);
      abort();
    }
    if (p->header & MARK) return;
    p->header |= MARK;
    mark_stack_.push_back(p);
  };
  size_t roots = 0;
  for (Value* r = roots_begin; r < roots_end; r++, roots++) push(*r);
  while (!mark_stack_.empty()) {
    Pair* p = mark_stack_.back();
    mark_stack_.pop_back();
    push(p->car);
    push(p->cdr);
  }
  size_t live = 0, freed = 0;
  for (auto& c : chunks_)
    for (size_t i = 0; i < c.n; i++) {
      Pair* p = &c.cells[i];
      if (p->header & FREE) continue;
      if (p->header & MARK) {
        p->header &= ~MARK;
        live++;
      } else {
        p->header = FREE;
        p->car = (Value)free_list_;
        p->cdr = NIL;
        free_list_ = p;
        freed++;
      }
    }
  free_count_ += freed;
  if (free_count_ < total_ / 2) add_chunk(total_);
  auto ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  stats.collections++;
  stats.freed += freed;
  stats.total_ns += ns;
  if (ns > stats.max_ns) stats.max_ns = ns;
  stats.pauses_ns.push_back(ns);
  if (log)
    fprintf(stderr, "[gc] #%llu roots=%zu live=%zu freed=%zu heap=%zu cells pause=%.3f ms\n",
            (unsigned long long)stats.collections, roots, live, freed, total_, ns / 1e6);
}
