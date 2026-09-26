#!/usr/bin/env python3
"""Generate the inlining / instruction-cache benchmark.

    leaves:  L small functions (~20 IR instructions each)
    mids:    M functions, each calling K leaves
    main:    a hot loop that picks one mid per iteration at pseudo-random,
             through a binary tree of ifs (like a switch or a virtual call)

Without inlining, the code the loop can reach is L leaves + M small mids +
the dispatch tree: a few tens of KB, close to a 32 KB L1 instruction cache.
With a threshold large enough to inline leaves into mids and mids into main,
every arm of the dispatch tree carries its own copies of K leaves. Calls
disappear, but the reachable code grows by roughly M*K/L and each iteration
jumps somewhere different in it, so the icache keeps missing.

    python3 bench/gen_inline.py > build/inline_bench.tiny
"""
import os
import random
import sys

# Override with e.g. M=512 python3 bench/gen_inline.py
L, M, K, ITERS = (int(os.environ.get(k, d)) for k, d in (("L", 16), ("M", 48), ("K", 8), ("ITERS", 60000)))
rng = random.Random(7)
out = []
for i in range(L):
    a, b, c = rng.randint(2, 9), rng.randint(1, 50), rng.randint(3, 13)
    out.append(f"""fn leaf{i}(x) {{
  let t = x * {a} + {b};
  if t % {c} == 0 {{ t = t - x; }} else {{ t = t + {i}; }}
  let u = (t * t + {b}) % 1009;
  if u > 500 {{ u = u - {i + 1}; }}
  return u + (t % 17) * {a};
}}""")
for j in range(M):
    calls = " + ".join(f"leaf{rng.randrange(L)}(x + {k})" for k in range(K))
    out.append(f"fn mid{j}(x) {{ return {calls}; }}")


def tree(lo, hi, ind):
    pad = "  " * ind
    if hi - lo == 1:
        return f"{pad}s = (s + mid{lo}(i)) % 1000000007;\n"
    m = (lo + hi) // 2
    return (f"{pad}if r < {m} {{\n" + tree(lo, m, ind + 1) + f"{pad}}} else {{\n" + tree(m, hi, ind + 1) + f"{pad}}}\n")


out.append(f"""fn main() {{
  let i = 0;
  let s = 0;
  let seed = 12345;
  while i < {ITERS} {{
    seed = (seed * 1103515245 + 12345) % 2147483648;
    let r = (seed / 65536) % {M};
{tree(0, M, 2)}    i = i + 1;
  }}
  print(s);
}}""")
sys.stdout.write("\n\n".join(out) + "\n")
