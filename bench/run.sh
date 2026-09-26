#!/usr/bin/env bash
# Run all benchmarks. Uses perf if hardware counters work, else cachegrind.
set -euo pipefail
cd "$(dirname "$0")/.."
make -s all
python3 bench/bench.py
