# The JIT emits x86-64 machine code and uses Linux mmap/mprotect, and the
# ELF tooling targets Linux. On macOS (including Apple Silicon) run it here:
#   docker build --platform linux/amd64 -t tinyjit .
#   docker run --rm -it --platform linux/amd64 tinyjit
# Hardware counters (perf) are usually unavailable inside containers;
# bench/bench.py falls back to valgrind's cachegrind.
FROM --platform=linux/amd64 ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ make python3 binutils valgrind gdb ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /tinyjit
COPY . .
RUN make -j && python3 tests/run_tests.py
CMD ["bash"]
