#!/usr/bin/env python3
import os
import random
import sys

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
