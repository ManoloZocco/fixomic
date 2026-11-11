#include "support/AllocCounter.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<int> gAllocations{0};
} // namespace

namespace fixomic::test {

void startCountingAllocations() noexcept {
    gAllocations = 0;
    gCountAllocations = true;
}

int stopCountingAllocations() noexcept {
    gCountAllocations = false;
    return gAllocations.load();
}

} // namespace fixomic::test

// Replacement global allocation functions. They must not be inline, so they live in this one
// translation unit. Only the plain forms are replaced: libc++'s nothrow forms forward to them,
// over-aligned allocations are not counted.
void* operator new(std::size_t size) {
    if (gCountAllocations.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
