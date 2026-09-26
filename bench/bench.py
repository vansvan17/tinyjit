#!/usr/bin/env python3
"""Benchmarks. Prints markdown tables (also saved to build/bench_results.md).

Timings are the program's run time as reported by --stats (compile time is
reported separately), best of N runs. Cache and branch numbers come from
`perf stat` when hardware counters are available, and otherwise from
valgrind's cachegrind, which simulates the caches and branch predictor. The
cachegrind runs use --instr-at-start=no, so only the program run is counted,
not the compiler.
"""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "tinyjit")
BUILD = os.path.join(ROOT, "build")
RUNS = int(os.environ.get("RUNS", "5"))
out_lines = []


def emit(s=""):
    print(s)
    out_lines.append(s)


def run_stats(args):
    p = subprocess.run([BIN, "--stats", *args], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    st = {}
    m = re.search(r"run time\s+([\d.]+) ms", p.stderr)
    st["run"] = float(m.group(1))
    m = re.search(r"compile time\s+([\d.]+) ms before running.*?, ([\d.]+) ms in the JIT", p.stderr)
    st["front"], st["jit_ms"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"jit\s+(\d+) functions, (\d+) bytes of code, (\d+) spills", p.stderr)
    st["jit_fns"], st["code"], st["spills"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
    m = re.search(r"inlined=(\d+)", p.stderr)
    st["inlined"] = int(m.group(1))
    m = re.search(r"gc pauses\s+total ([\d.]+) ms, p50 ([\d.]+) ms, p99 ([\d.]+) ms, max ([\d.]+) ms", p.stderr)
    if m:
        st["gc"] = tuple(float(x) for x in m.groups())
    m = re.search(r"gc\s+(\d+) collections, (\d+) allocated", p.stderr)
    if m:
        st["collections"], st["allocated"] = int(m.group(1)), int(m.group(2))
    st["stdout"] = p.stdout
    return st


def best(args):
    runs = [run_stats(args) for _ in range(RUNS)]
    return min(runs, key=lambda r: r["run"])


def wall(cmd):
    t = []
    for _ in range(RUNS):
        t0 = time.perf_counter()
        subprocess.run(cmd, capture_output=True, check=True)
        t.append((time.perf_counter() - t0) * 1000)
    return min(t)


def perf_available():
    if not shutil.which("perf"):
        return False
    p = subprocess.run(["perf", "stat", "-e", "branch-misses", "true"], capture_output=True, text=True)
    return p.returncode == 0 and "not supported" not in p.stderr


def cachegrind(args, branch=False):
    cmd = ["valgrind", "--tool=cachegrind", "--instr-at-start=no", "--cache-sim=yes",
           "--cachegrind-out-file=/dev/null"]
    if branch:
        cmd.append("--branch-sim=yes")
    p = subprocess.run(cmd + [BIN, *args], capture_output=True, text=True)
    def num(label):
        m = re.search(label + r":\s+([\d,]+)", p.stderr)
        return int(m.group(1).replace(",", "")) if m else None
    return {"instr": num("I\\s+refs"), "i1_miss": num("I1\\s+misses"), "mispred": num("Mispredicts"),
            "branches": num("Branches")}


def perf_stat(args, events):
    p = subprocess.run(["perf", "stat", "-x,", "-e", ",".join(events), BIN, *args], capture_output=True, text=True)
    res = {}
    for line in p.stderr.splitlines():
        parts = line.split(",")
        if len(parts) > 2 and parts[0].replace(".", "").isdigit():
            res[parts[2]] = int(float(parts[0]))
    return res


def fmt(n):
    return f"{n:,}" if isinstance(n, int) else (f"{n:.2f}" if isinstance(n, float) else str(n))


def main():
    have_perf = perf_available()
    have_vg = shutil.which("valgrind") is not None
    emit(f"# tinyjit benchmarks\n")
    emit(f"Best of {RUNS} runs. Counters from: "
         f"{'perf (hardware)' if have_perf else 'cachegrind (simulated)' if have_vg else 'none available'}.\n")

    # 1. Interpreter dispatch and JIT speedup
    emit("## Execution modes (run time, ms)\n")
    emit("| program | switch interp | goto interp | JIT tiered | JIT eager |")
    emit("|---|---:|---:|---:|---:|")
    for prog in ["fib.tiny", "loop.tiny", "sieve.tiny"]:
        path = os.path.join(ROOT, "bench", prog)
        row = [best(["--jit=off", "--dispatch=switch", path])["run"], best(["--jit=off", path])["run"],
               best(["--jit=tiered", path])["run"], best(["--jit=eager", path])["run"]]
        emit(f"| {prog} | " + " | ".join(fmt(x) for x in row) + " |")
    emit("\nloop.tiny at -O2 inlines `collatz` into `main`, which runs once, so tiered mode (which counts calls "
         "and has no on-stack replacement) never compiles it. Eager mode does. sieve.tiny allocates in its hot "
         "functions, which the JIT declines, so it stays in the interpreter.\n")

    # 2. C comparison
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc:
        emit("## fib(32) against C\n")
        emit("| build | wall time, ms |")
        emit("|---|---:|")
        src = os.path.join(ROOT, "bench", "c", "fib.c")
        for opt in ["-O0", "-O2"]:
            exe = os.path.join(BUILD, f"fib_c{opt}")
            subprocess.run([cc, opt, src, "-o", exe], check=True)
            emit(f"| C {opt} | {fmt(wall([exe]))} |")
        fib = os.path.join(ROOT, "bench", "fib.tiny")
        emit(f"| tinyjit, JIT (process wall time) | {fmt(wall([BIN, '--jit=eager', fib]))} |")
        emit(f"| tinyjit, interpreter (process wall time) | {fmt(wall([BIN, '--jit=off', fib]))} |")
        emit("")

    # 3. Dispatch: branch prediction
    fib = os.path.join(ROOT, "bench", "fib.tiny")
    loop = os.path.join(ROOT, "bench", "loop.tiny")
    if have_perf or have_vg:
        emit("## switch vs computed goto: branch mispredictions\n")
        emit("| program | dispatch | instructions | branches | mispredicted |")
        emit("|---|---|---:|---:|---:|")
        for path in [fib, loop]:
            for d in ["switch", "goto"]:
                args = ["--jit=off", f"--dispatch={d}", path]
                if have_perf:
                    r = perf_stat(args, ["instructions", "branches", "branch-misses"])
                    row = [r.get("instructions"), r.get("branches"), r.get("branch-misses")]
                else:
                    r = cachegrind(args, branch=True)
                    row = [r["instr"], r["branches"], r["mispred"]]
                emit(f"| {os.path.basename(path)} | {d} | " + " | ".join(fmt(x) for x in row) + " |")
        emit("")

    # 4. Inlining vs instruction cache
    gen = os.path.join(BUILD, "inline_bench.tiny")
    with open(gen, "w") as f:
        subprocess.run([sys.executable, os.path.join(ROOT, "bench", "gen_inline.py")], stdout=f, check=True)
    emit("## Inlining and the instruction cache (bench/gen_inline.py, JIT eager)\n")
    emit("| inline threshold | calls inlined | code bytes | instructions | L1i misses | run time, ms |")
    emit("|---:|---:|---:|---:|---:|---:|")
    for t in [0, 10, 30, 100, 400]:
        args = ["--jit=eager", f"--inline={t}", gen]
        st = best(args)
        instr = miss = None
        if have_perf:
            r = perf_stat(args, ["instructions", "L1-icache-load-misses"])
            instr, miss = r.get("instructions"), r.get("L1-icache-load-misses")
        elif have_vg:
            r = cachegrind(args)
            instr, miss = r["instr"], r["i1_miss"]
        emit(f"| {t} | {st['inlined']} | {fmt(st['code'])} | {fmt(instr)} | {fmt(miss)} | {fmt(st['run'])} |")
    emit("")

    # 5. GC pauses
    emit("## Garbage collector pauses\n")
    emit("| program | allocated cells | collections | total pause, ms | p50 | p99 | max |")
    emit("|---|---:|---:|---:|---:|---:|---:|")
    for prog in ["gc_live.tiny", "sieve.tiny"]:
        st = best(["--jit=off", os.path.join(ROOT, "bench", prog)])
        g = st.get("gc", (0, 0, 0, 0))
        emit(f"| {prog} | {fmt(st.get('allocated', 0))} | {st.get('collections', 0)} | "
             + " | ".join(fmt(x) for x in g) + " |")
    emit("")

    with open(os.path.join(BUILD, "bench_results.md"), "w") as f:
        f.write("\n".join(out_lines) + "\n")


if __name__ == "__main__":
    main()
