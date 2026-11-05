// WASM replacement for <xmmintrin.h>: only the prefetch hint the gamecore uses (a no-op without SIMD).
#pragma once
#define _MM_HINT_T0 3
#define _MM_HINT_T1 2
#define _MM_HINT_T2 1
#define _MM_HINT_NTA 0
#define _mm_prefetch(p, hint) ((void)(p), (void)(hint))
