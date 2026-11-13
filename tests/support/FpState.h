#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace fixomic::test {

// The calling thread's flush-to-zero mode as the CPU reports it: FPCR.FZ on arm64, MXCSR's FTZ and
// DAZ bits on x86. False on any other platform (see denormalResultFlushed() for a portable check).
inline bool flushToZeroFlagSet() noexcept {
#if defined(__aarch64__)
    unsigned long long fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr & (1ull << 24)) != 0;
#elif defined(__x86_64__) || defined(__i386__)
    return (_mm_getcsr() & 0x8040u) == 0x8040u;
#else
    return false;
#endif
}

// Whether a product that should be denormal comes out as zero on the calling thread: true only
// while denormals are flushed (ScopedNoDenormals), on any platform.
inline bool denormalResultFlushed() noexcept {
    volatile float tiny = 1.0e-37f;   // normal (FLT_MIN is about 1.18e-38)
    volatile float scale = 1.0e-3f;
    const float product = tiny * scale;   // 1e-40: denormal
    return product == 0.0f;
}

} // namespace fixomic::test
