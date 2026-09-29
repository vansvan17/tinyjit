CXX      ?= g++
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-implicit-fallthrough
CC       ?= cc

SRC  := $(wildcard src/*.cpp)
OBJ  := $(SRC:src/%.cpp=build/%.o)
HDRS := $(wildcard src/*.h) src/interp.inc

.PHONY: all clean test fuzz bench asm aot

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# The JIT and the ELF tools are x86-64 Linux only. On macOS (or arm64 Linux)
# everything else builds and runs, with every function interpreted.
ifeq ($(UNAME_S)-$(UNAME_M),Linux-x86_64)
all: build/tinyjit build/elfdump
else
all: build/tinyjit
endif

build/tinyjit: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp $(HDRS) | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/elfdump: tools/elfdump.cpp | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build:
	mkdir -p build

test: all
	python3 tests/run_tests.py

fuzz: all
	python3 tools/fuzz.py --count 300

bench: all
	bash bench/run.sh

asm: all
	bash tools/compare_asm.sh

# Ahead-of-time: compile bench/fib.tiny to an ELF object and link it.
aot: all
	build/tinyjit --emit-elf=build/fib_aot.o bench/fib.tiny
	$(CC) -no-pie aot/runtime.c build/fib_aot.o -o build/fib_aot
	build/fib_aot

clean:
	rm -rf build
