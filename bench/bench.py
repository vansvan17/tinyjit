#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("TINYJIT", os.path.join(ROOT, "build", "tinyjit")).split()
BUILD = os.path.join(ROOT, "build")
RUNS = int(os.environ.get("RUNS", "5"))
out_lines = []


def emit(s=""):
    print(s)
    out_lines.append(s)


def run_stats(args):
    p = subprocess.run([*BIN, "--stats", *args], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    st = {"run": float(re.search(r"run time\s+([\d.]+) ms", p.stderr).group(1))}
    m = re.search(r"compile time\s+([\d.]+) ms before running.*?, ([\d.]+) ms in the JIT", p.stderr)
    st["front"], st["jit_ms"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"jit\s+(\d+) functions, (\d+) bytes of code", p.stderr)
    st["code"] = int(m.group(2))
    st["inlined"] = int(re.search(r"inlined=(\d+)", p.stderr).group(1))
    m = re.search(r"gc pauses\s+total ([\d.]+) ms, p50 ([\d.]+) ms, p99 ([\d.]+) ms, max ([\d.]+) ms", p.stderr)
    if m:
        st["gc"] = tuple(float(x) for x in m.groups())
    m = re.search(r"gc\s+(\d+) collections, (\d+) allocated", p.stderr)
    if m:
        st["collections"], st["allocated"] = int(m.group(1)), int(m.group(2))
    return st


def best(args):
    return min((run_stats(args) for _ in range(RUNS)), key=lambda r: r["run"])


def wall(cmd):
    t = []
    for _ in range(RUNS):
        t0 = time.perf_counter()
        subprocess.run(cmd, capture_output=True, check=True)
        t.append((time.perf_counter() - t0) * 1000)
    return min(t)


def fmt(x):
    return f"{x:,}" if isinstance(x, int) else f"{x:.2f}"


def bench_path(name):
    return os.path.join(ROOT, "bench", name)


def main():
    os.makedirs(BUILD, exist_ok=True)
    emit("# tinyjit benchmarks\n")
    emit(f"Best of {RUNS} runs, program run time only (compile time listed separately).\n")

    emit("## Execution modes (ms)\n")
    emit("| program | switch interp | goto interp | JIT tiered | JIT eager |")
    emit("|---|---:|---:|---:|---:|")
    for prog in ["fib.tiny", "loop.tiny", "sieve.tiny"]:
        path = bench_path(prog)
        row = [best(["--jit=off", "--dispatch=switch", path])["run"], best(["--jit=off", path])["run"],
               best(["--jit=tiered", path])["run"], best(["--jit=eager", path])["run"]]
        emit(f"| {prog} | " + " | ".join(fmt(x) for x in row) + " |")
    emit("")

    cc = shutil.which("cc")
    if cc:
        emit("## fib(32) against C (whole-process wall time, ms)\n")
        emit("| build | ms |")
        emit("|---|---:|")
        for opt in ["-O0", "-O2"]:
            exe = os.path.join(BUILD, f"fib_c{opt}")
            subprocess.run([cc, opt, bench_path("c/fib.c"), "-o", exe], check=True)
            emit(f"| C {opt} | {fmt(wall([exe]))} |")
        emit(f"| tinyjit, JIT | {fmt(wall([*BIN, '--jit=eager', bench_path('fib.tiny')]))} |")
        emit(f"| tinyjit, interpreter | {fmt(wall([*BIN, '--jit=off', bench_path('fib.tiny')]))} |")
        emit("")

    emit("## Inlining vs code size (bench/gen_inline.py, JIT eager)\n")
    for label, env in [("48 mid functions", {}), ("512 mid functions", {"M": "512", "ITERS": "200000"})]:
        gen = os.path.join(BUILD, f"inline_{label.split()[0]}.tiny")
        with open(gen, "w") as f:
            subprocess.run([sys.executable, bench_path("gen_inline.py")], stdout=f, check=True,
                           env={**os.environ, **env})
        emit(f"**{label}**\n")
        emit("| inline threshold | calls inlined | code bytes | run time, ms | compile time, ms |")
        emit("|---:|---:|---:|---:|---:|")
        for t in [0, 30, 400]:
            st = best(["--jit=eager", f"--inline={t}", gen])
            emit(f"| {t} | {st['inlined']} | {fmt(st['code'])} | {fmt(st['run'])} | {fmt(st['front'])} |")
        emit("")

    emit("## Garbage collector pauses (interpreter)\n")
    emit("| program | allocated cells | collections | total pause, ms | p50 | p99 | max |")
    emit("|---|---:|---:|---:|---:|---:|---:|")
    for prog in ["gc_live.tiny", "sieve.tiny"]:
        st = best(["--jit=off", bench_path(prog)])
        g = st.get("gc", (0.0, 0.0, 0.0, 0.0))
        emit(f"| {prog} | {fmt(st.get('allocated', 0))} | {st.get('collections', 0)} | "
             + " | ".join(fmt(x) for x in g) + " |")
    emit("")

    with open(os.path.join(BUILD, "bench_results.md"), "w") as f:
        f.write("\n".join(out_lines) + "\n")


if __name__ == "__main__":
    main()
