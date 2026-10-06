#!/usr/bin/env python3
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("TINYJIT", os.path.join(ROOT, "build", "tinyjit")).split()

CONFIGS = [
    ["-O0", "--jit=off", "--dispatch=switch"],
    ["-O1", "--jit=off", "--dispatch=goto"],
    ["-O2", "--jit=off"],
    ["-O0", "--jit=eager"],
    ["-O1", "--jit=eager"],
    ["-O2", "--jit=eager"],
    ["-O2", "--jit=tiered", "--jit-threshold=3"],
    ["-O2", "--jit=eager", "--inline=500"],
    ["-O0", "--jit=off", "--gc-stress"],
    ["-O2", "--jit=tiered", "--jit-threshold=1", "--gc-stress"],
]


def parse_expectations(src):
    out, err = [], None
    for line in src.splitlines():
        m = re.search(r"//\s*expect:\s*(.*)$", line)
        if m:
            out.append(m.group(1).strip())
        m = re.search(r"//\s*expect-error:\s*(.*)$", line)
        if m:
            err = m.group(1).strip()
    return out, err


def check_aot():
    import platform
    import shutil
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        print("aot: skipped (needs an Apple Silicon Mac)")
        return 0
    cc = shutil.which("cc")
    src = os.path.join(ROOT, "tests", "cases", "phis.tiny")
    obj = os.path.join(ROOT, "build", "aot_test.o")
    exe = os.path.join(ROOT, "build", "aot_test")
    subprocess.run([*BIN, f"--emit-obj={obj}", src], check=True, capture_output=True)
    subprocess.run([cc, os.path.join(ROOT, "aot", "runtime.c"), obj, "-o", exe], check=True)
    got = subprocess.run([exe], capture_output=True, text=True).stdout.splitlines()
    want, _ = parse_expectations(open(src).read())
    ok = got == want
    print(f"aot: {'passed' if ok else 'FAILED'} (phis.tiny compiled to Mach-O, linked with cc, run natively)")
    return 0 if ok else 1


def main():
    files = sorted(glob.glob(os.path.join(ROOT, "tests", "cases", "*.tiny")))
    if len(sys.argv) > 1:
        files = [f for f in files if any(a in f for a in sys.argv[1:])]
    failures = 0
    runs = 0
    for f in files:
        want_out, want_err = parse_expectations(open(f).read())
        for cfg in CONFIGS:
            runs += 1
            p = subprocess.run([*BIN, *cfg, f], capture_output=True, text=True, timeout=120)
            got = p.stdout.splitlines()
            ok = got == want_out
            if want_err is None:
                ok = ok and p.returncode == 0
            else:
                ok = ok and p.returncode == 1 and want_err in p.stderr
            if not ok:
                failures += 1
                print(f"FAIL {os.path.basename(f)} {' '.join(cfg)} (exit {p.returncode})")
                for i in range(max(len(got), len(want_out))):
                    g = got[i] if i < len(got) else "<missing>"
                    w = want_out[i] if i < len(want_out) else "<extra>"
                    if g != w:
                        print(f"    line {i + 1}: got {g!r}, want {w!r}")
                        break
                if p.stderr.strip():
                    print("    stderr: " + p.stderr.strip().splitlines()[-1][:200])
    print(f"{runs - failures}/{runs} passed ({len(files)} programs x {len(CONFIGS)} configurations)")
    failures += check_aot()
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
