#pragma once
#include <cstdio>
#include "value.h"

// These two are called from JIT-compiled code, so they use the C ABI and
// have stable symbol names (they also show up as relocations in --emit-obj).
extern "C" void rt_print(Value v);
extern "C" [[noreturn]] void rt_error(int code);

enum RtError { RT_TYPE = 1, RT_DIVZERO = 2 };

[[noreturn]] void runtime_error(const char* msg);
void print_value(FILE* out, Value v);
