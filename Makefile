# tinyjit targets Apple Silicon Macs. `make` needs only the Xcode command
# line tools (xcode-select --install).
CXX      ?= c++
CC       ?= cc
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-implicit-fallthrough
BUILD    ?= build

SRC  := $(wildcard src/*.cpp)
OBJ  := $(SRC:src/%.cpp=$(BUILD)/%.o)
HDRS := $(wildcard src/*.h) src/interp.inc

.PHONY: all clean test fuzz bench asm aot

all: $(BUILD)/tinyjit $(BUILD)/machodump

$(BUILD)/tinyjit: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/machodump: tools/machodump.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

test: all
	python3 tests/run_tests.py

fuzz: all
	python3 tools/fuzz.py --count 300

bench: all
	python3 bench/bench.py

asm: all
	bash tools/compare_asm.sh

# Ahead-of-time: compile bench/fib.tiny to a Mach-O object and link it.
aot: all
	$(BUILD)/tinyjit --emit-obj=$(BUILD)/fib_aot.o bench/fib.tiny
	$(CC) aot/runtime.c $(BUILD)/fib_aot.o -o $(BUILD)/fib_aot
	$(BUILD)/fib_aot

clean:
	rm -rf $(BUILD)
