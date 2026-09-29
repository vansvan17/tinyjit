#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "ast.h"
#include "ir.h"
#include "jit.h"
#include "mir.h"
#include "passes.h"
#include "regalloc.h"
#include "vm.h"

static void usage() {
  fprintf(stderr,
          "usage: tinyjit [options] file.tiny\n"
          "\n"
          "  -O0 | -O1 | -O2       optimization level (default -O2)\n"
          "                        -O1: constant folding, CFG cleanup, DCE, isel, guard removal\n"
          "                        -O2: -O1 plus inlining\n"
          "  --inline=N            inline callees of at most N IR instructions (default 40 at -O2)\n"
          "  --dispatch=goto|switch interpreter dispatch (default goto)\n"
          "  --jit=off|tiered|eager JIT mode (default tiered)\n"
          "  --jit-threshold=N     calls before a function is compiled (default 100)\n"
          "  --gc-stress           collect on every allocation\n"
          "  --gc-log              one line per collection on stderr\n"
          "  --stats               timing, optimizer, JIT and GC summary on stderr\n"
          "\n"
          "  --dump-ast            print the AST and exit\n"
          "  --dump-ir             print SSA before optimization and exit\n"
          "  --dump-opt            print SSA after optimization and exit\n"
          "  --dump-mir            print MIR (after isel and out-of-SSA) and exit\n"
          "  --dump-bc             print bytecode and exit\n"
          "  --dump-regalloc       print JIT live intervals on stderr while compiling\n"
          "  --dump-jit=FILE       after running, write the raw JIT code region to FILE\n"
          "  --emit-obj=FILE       compile every function with the JIT backend into a\n"
          "                        Mach-O .o and exit (link with aot/runtime.c)\n");
  exit(2);
}

int main(int argc, char** argv) {
  int level = 2;
  int inline_threshold = -1;
  bool goto_dispatch = true;
  std::string jit_mode = "tiered";
  uint64_t jit_threshold = 100;
  bool gc_stress = false, gc_log = false, stats = false;
  bool dump_ast = false, dump_ir = false, dump_opt = false, dump_mir = false, dump_bc = false, dump_ra = false;
  std::string dump_jit, emit_obj, path;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&](const char* prefix) -> const char* {
      size_t n = strlen(prefix);
      return a.compare(0, n, prefix) == 0 ? a.c_str() + n : nullptr;
    };
    if (a == "-O0") level = 0;
    else if (a == "-O1") level = 1;
    else if (a == "-O2") level = 2;
    else if (auto v = val("--inline=")) inline_threshold = atoi(v);
    else if (auto v = val("--dispatch=")) goto_dispatch = strcmp(v, "switch") != 0;
    else if (auto v = val("--jit=")) jit_mode = v;
    else if (auto v = val("--jit-threshold=")) jit_threshold = strtoull(v, nullptr, 10);
    else if (a == "--gc-stress") gc_stress = true;
    else if (a == "--gc-log") gc_log = true;
    else if (a == "--stats") stats = true;
    else if (a == "--dump-ast") dump_ast = true;
    else if (a == "--dump-ir") dump_ir = true;
    else if (a == "--dump-opt") dump_opt = true;
    else if (a == "--dump-mir") dump_mir = true;
    else if (a == "--dump-bc") dump_bc = true;
    else if (a == "--dump-regalloc") dump_ra = true;
    else if (auto v = val("--dump-jit=")) dump_jit = v;
    else if (auto v = val("--emit-obj=")) emit_obj = v;
    else if (a[0] == '-') usage();
    else path = a;
  }
  if (path.empty()) usage();
  if (jit_mode != "off" && jit_mode != "tiered" && jit_mode != "eager") usage();
  if (inline_threshold < 0) inline_threshold = level >= 2 ? 40 : 0;

  std::ifstream in(path);
  if (!in) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
  std::stringstream ss;
  ss << in.rdbuf();

  auto tc0 = std::chrono::steady_clock::now();
  Program prog = parse_program(ss.str());
  if (dump_ast) { fputs(dump_ast_text(prog).c_str(), stdout); return 0; }

  Module mod = build_ssa(prog);
  for (auto& f : mod.fns) verify(*f);
  if (dump_ir) { fputs(print_module(mod).c_str(), stdout); return 0; }

  OptStats ost;
  for (auto& f : mod.fns) optimize_function(*f, level, ost);
  if (level >= 1) inline_module(mod, inline_threshold, ost);
  for (auto& f : mod.fns) {
    if (level >= 1) {
      compute_known_int(*f);
      prove_int_operands(*f);
    }
    verify(*f);
  }
  if (dump_opt) { fputs(print_module(mod).c_str(), stdout); return 0; }

  std::vector<MFunc> mir;
  int coalesced = 0;
  for (auto& f : mod.fns) {
    mir.push_back(lower_to_mir(*f, level >= 1));
    if (level >= 1) coalesced += coalesce_moves(mir.back());
  }
  if (dump_mir) {
    for (auto& m : mir) printf("%s\n", print_mfunc(m).c_str());
    return 0;
  }

  VM vm;
  vm.mir = &mir;
  vm.funcs.resize(mir.size());
  size_t bc_count = 0;
  for (size_t i = 0; i < mir.size(); i++) {
    std::string err;
    if (!compile_bytecode(mir[i], vm.funcs[i], err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    bc_count += vm.funcs[i].code.size();
    if (mir[i].has_cons) vm.clear_frames = true;
  }
  if (dump_bc) {
    for (auto& f : vm.funcs) printf("%s\n", disasm(f).c_str());
    return 0;
  }

  vm.goto_dispatch = goto_dispatch;
  vm.heap.stress = gc_stress;
  vm.heap.log = gc_log;
  JIT jit(vm, mir);
  if (!jit_supported() && jit_mode != "off" && stats)
    fprintf(stderr, "[stats] note            JIT not available on this platform, running interpreted\n");
  jit.print_intervals = dump_ra;
  if (!emit_obj.empty()) {
    jit.compile_all();
    std::string err;
    if (!jit.write_object(emit_obj, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    fprintf(stderr, "wrote %s: %d functions, %zu bytes of code\n", emit_obj.c_str(), jit.functions_compiled,
            jit.code_bytes());
    for (auto& f : vm.funcs)
      if (f.jit_state == 2) fprintf(stderr, "  skipped %s (allocates or calls something that does)\n", f.name.c_str());
    return 0;
  }
  if (jit_mode != "off") {
    vm.jit = &jit;
    vm.jit_threshold = jit_threshold;
  }
  if (jit_mode == "eager") jit.compile_all();
  double front_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc0).count();

  setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  auto t0 = std::chrono::steady_clock::now();
  vm.run(mod.index["main"]);
  auto t1 = std::chrono::steady_clock::now();
  fflush(stdout);

  if (!dump_jit.empty() && !jit.write_raw(dump_jit)) fprintf(stderr, "cannot write %s\n", dump_jit.c_str());

  if (stats) {
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    fprintf(stderr, "[stats] run time        %.2f ms\n", ms);
    fprintf(stderr, "[stats] compile time    %.2f ms before running (parse, SSA, opt, bytecode%s), %.2f ms in the JIT\n",
            front_ms, jit_mode == "eager" ? ", eager JIT" : "", jit.compile_ms);
    fprintf(stderr, "[stats] optimizer       folded=%d dce=%d blocks_removed=%d inlined=%d moves_coalesced=%d\n",
            ost.folded, ost.dce_removed, ost.blocks_removed, ost.inlined, coalesced);
    fprintf(stderr, "[stats] bytecode        %zu instructions in %zu functions (%s dispatch)\n", bc_count,
            vm.funcs.size(), goto_dispatch ? "goto" : "switch");
    fprintf(stderr, "[stats] jit             %d functions, %zu bytes of code, %d spills\n",
            jit.functions_compiled, jit.function_bytes(), jit.spills);
    auto& g = vm.heap.stats;
    if (g.collections) {
      auto p = g.pauses_ns;
      std::sort(p.begin(), p.end());
      auto pct = [&](double q) { return p[std::min(p.size() - 1, (size_t)(q * p.size()))] / 1e6; };
      fprintf(stderr, "[stats] gc              %llu collections, %llu allocated, %llu freed, heap %zu cells\n",
              (unsigned long long)g.collections, (unsigned long long)g.allocated, (unsigned long long)g.freed,
              vm.heap.heap_cells());
      fprintf(stderr, "[stats] gc pauses       total %.2f ms, p50 %.3f ms, p99 %.3f ms, max %.3f ms\n",
              g.total_ns / 1e6, pct(0.5), pct(0.99), g.max_ns / 1e6);
    } else {
      fprintf(stderr, "[stats] gc              %llu allocated, no collections\n", (unsigned long long)g.allocated);
    }
  }
  return 0;
}
