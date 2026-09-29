#!/usr/bin/env python3
"""Differential fuzzer.

Generates random tiny programs and runs each one under several
configurations. The plain interpreter at -O0 is the reference; every other
configuration (optimizer levels, inlining, eager and tiered JIT, GC stress)
must print exactly the same output and exit with the same status.

Programs always terminate: functions only call functions defined before them,
and every loop has a counter the body cannot assign. Division uses
`x / (y * y + 1)` because a square is never -1 modulo 2^63. A small fraction of
programs deliberately hit a type error or division by zero to check that all
backends fail the same way.

    python3 tools/fuzz.py --count 500 --seed 1
"""
import argparse
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# TINYJIT overrides the command, e.g. TINYJIT="qemu-aarch64 build-arm64/tinyjit"
# to test the arm64 JIT under emulation on another machine.
BIN = os.environ.get("TINYJIT", os.path.join(ROOT, "build", "tinyjit")).split()

REFERENCE = ["-O0", "--jit=off", "--dispatch=switch"]
CONFIGS = [
    ["-O1", "--jit=off"],
    ["-O2", "--jit=off"],
    ["-O0", "--jit=eager"],
    ["-O2", "--jit=eager"],
    ["-O2", "--jit=eager", "--inline=300"],
    ["-O1", "--jit=tiered", "--jit-threshold=2"],
    ["-O2", "--jit=tiered", "--jit-threshold=1", "--gc-stress"],
]


class Gen:
    def __init__(self, rng, allow_errors):
        self.r = rng
        self.allow_errors = allow_errors
        self.funcs = []  # (name, nparams, uses_lists)

    def const(self):
        r = self.r.random()
        if r < 0.6:
            return str(self.r.randint(0, 20))
        if r < 0.8:
            return str(self.r.randint(-1000, 1000))
        if r < 0.9:
            return str(self.r.choice([2**31 - 1, 2**31, 2**40 + 3, 4611686018427387903, 123456789012]))
        return self.r.choice(["true", "false"])

    def expr(self, vars, depth):
        r = self.r.random()
        if depth <= 0 or r < 0.25:
            if vars and self.r.random() < 0.7:
                return self.r.choice(vars)
            return self.const()
        r = self.r.random()
        if r < 0.45:
            op = self.r.choice(["+", "-", "*", "+", "-", "<", "<=", ">", ">=", "==", "!="])
            return f"({self.expr(vars, depth - 1)} {op} {self.expr(vars, depth - 1)})"
        if r < 0.55:
            op = self.r.choice(["/", "%"])
            d = self.expr(vars, depth - 2)
            if self.allow_errors and self.r.random() < 0.03:
                return f"({self.expr(vars, depth - 1)} {op} ({d} - {d}))"
            return f"({self.expr(vars, depth - 1)} {op} ({d} * {d} + 1))"
        if r < 0.65:
            op = self.r.choice(["&&", "||"])
            return f"({self.expr(vars, depth - 1)} {op} {self.expr(vars, depth - 1)})"
        if r < 0.72:
            return f"{self.r.choice(['-', '!'])}{self.expr(vars, depth - 1)}"
        if r < 0.9 and self.funcs:
            name, n, _ = self.r.choice(self.funcs)
            args = ", ".join(self.expr(vars, depth - 2) for _ in range(n))
            return f"{name}({args})"
        if self.allow_errors and self.r.random() < 0.02:
            return f"({self.expr(vars, depth - 1)} + nil)"
        return self.expr(vars, depth - 1)

    def block(self, vars, assignable, depth, lines, indent, lists):
        pad = "  " * indent
        n = self.r.randint(1, 5)
        local = []
        for _ in range(n):
            r = self.r.random()
            if r < 0.3:
                v = f"v{self.r.randint(0, 999)}"
                lines.append(f"{pad}let {v} = {self.expr(vars, 3)};")
                vars = vars + [v]
                assignable = assignable + [v]
                local.append(v)
            elif r < 0.5 and assignable:
                v = self.r.choice(assignable)
                lines.append(f"{pad}{v} = {self.expr(vars, 3)};")
            elif r < 0.62:
                lines.append(f"{pad}print({self.expr(vars, 3)});")
            elif r < 0.75 and depth > 0:
                lines.append(f"{pad}if {self.expr(vars, 2)} {{")
                self.block(vars, assignable, depth - 1, lines, indent + 1, lists)
                if self.r.random() < 0.5:
                    lines.append(f"{pad}}} else {{")
                    self.block(vars, assignable, depth - 1, lines, indent + 1, lists)
                lines.append(f"{pad}}}")
            elif r < 0.87 and depth > 0:
                c = f"c{self.r.randint(0, 999)}"
                bound = self.r.randint(1, 12)
                lines.append(f"{pad}let {c} = 0;")
                cond = f"{c} < {bound}"
                if self.r.random() < 0.3:
                    cond += f" && {self.expr(vars, 2)}"
                lines.append(f"{pad}while {cond} {{")
                self.block(vars + [c], assignable, depth - 1, lines, indent + 1, lists)
                lines.append(f"{pad}  {c} = {c} + 1;")
                lines.append(f"{pad}}}")
            elif r < 0.93 and lists:
                a, b = self.expr(vars, 2), self.expr(vars, 2)
                l = f"l{self.r.randint(0, 999)}"
                lines.append(f"{pad}let {l} = cons({a}, cons({b}, nil));")
                lines.append(f"{pad}print({l});")
                lines.append(f"{pad}print(car(cdr({l})) + car({l}));")
            elif r < 0.97:
                lines.append(f"{pad}if {self.expr(vars, 2)} {{ return {self.expr(vars, 2)}; }}")
            else:
                lines.append(f"{pad}{{")
                self.block(vars, assignable, depth - 1, lines, indent + 1, lists)
                lines.append(f"{pad}}}")

    def function(self, idx):
        name = f"f{idx}"
        n = self.r.choice([0, 1, 1, 2, 2, 3, 4, 6, 7])
        params = [f"p{i}" for i in range(n)]
        lists = self.r.random() < 0.15
        lines = [f"fn {name}({', '.join(params)}) {{"]
        self.block(params, params, 2, lines, 1, lists)
        lines.append(f"  return {self.expr(params, 3)};")
        lines.append("}")
        self.funcs.append((name, n, lists))
        return "\n".join(lines)

    def program(self):
        parts = [self.function(i) for i in range(self.r.randint(1, 6))]
        lines = ["fn main() {", "  let i = 0;", "  while i < 25 {"]
        for name, n, _ in self.funcs:
            args = ", ".join(self.r.choice(["i", "(i * 3 - 7)", str(self.r.randint(-50, 50))]) for _ in range(n))
            lines.append(f"    print({name}({args}));")
        lines.append("    i = i + 1;")
        lines.append("  }")
        lines.append("}")
        parts.append("\n".join(lines))
        return "\n\n".join(parts) + "\n"


def run(cfg, path):
    try:
        p = subprocess.run([*BIN, *cfg, path], capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return ("timeout", -1)
    err = p.stderr.strip().splitlines()
    return (p.stdout, p.returncode, err[-1] if p.returncode and err else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--errors", type=float, default=0.3, help="fraction of programs allowed to trap")
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    out_dir = os.path.join(ROOT, "build", "fuzz")
    os.makedirs(out_dir, exist_ok=True)
    failures = 0
    traps = 0
    for k in range(args.count):
        rng = random.Random(seed + k)
        src = Gen(rng, rng.random() < args.errors).program()
        path = os.path.join(out_dir, f"prog_{seed + k}.tiny")
        with open(path, "w") as f:
            f.write(src)
        ref = run(REFERENCE, path)
        if ref[1] == 1:
            traps += 1
        bad = [cfg for cfg in CONFIGS if run(cfg, path) != ref]
        if ref[1] not in (0, 1) or bad:
            failures += 1
            print(f"MISMATCH seed={seed + k} file={path}")
            print(f"  reference exit={ref[1]} {ref[2] if len(ref) > 2 else ''}")
            for cfg in bad:
                print(f"  differs: {' '.join(cfg)}")
        else:
            os.remove(path)
    print(f"{args.count - failures}/{args.count} programs agree across {len(CONFIGS) + 1} configurations "
          f"(seed {seed}, {traps} ended in a runtime error)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
