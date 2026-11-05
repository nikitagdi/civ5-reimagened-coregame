// Portable replacement for <intrin.h>: the compiler builtins the gamecore uses.
#pragma once
#include "lekmod_portable/prelude.h"
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif
