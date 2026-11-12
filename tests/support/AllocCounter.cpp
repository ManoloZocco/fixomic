#include "support/AllocCounter.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
// Armed only on the thread that called startCountingAllocations(). A trivial thread_local needs
// no allocation or lock to read, so it is safe inside the replacement operator new. The count is
// only ever touched by the armed thread.
thread_local bool tCountAllocations = false;
thread_local int tAllocations = 0;
} // namespace

namespace fixomic::test {

void startCountingAllocations() noexcept {
    tAllocations = 0;
    tCountAllocations = true;
}

int stopCountingAllocations() noexcept {
    tCountAllocations = false;
    return tAllocations;
}

} // namespace fixomic::test

// Replacement global allocation functions. They must not be inline, so they live in this one
// translation unit. Only the plain forms are replaced: libc++'s nothrow forms forward to them,
// over-aligned allocations are not counted.
void* operator new(std::size_t size) {
    if (tCountAllocations) ++tAllocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
