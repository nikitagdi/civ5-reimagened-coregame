// WASM: alloca is the compiler builtin.
#pragma once
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif
