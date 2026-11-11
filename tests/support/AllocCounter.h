#pragma once

namespace fixomic::test {

// Counts calls to the global operator new (replaced for the whole test binary in
// AllocCounter.cpp) between start and stop, on every thread. Lets a test prove that a
// real-time code path never touches the heap. Not reentrant: one counting window at a time.
void startCountingAllocations() noexcept;
int stopCountingAllocations() noexcept;   // returns the allocations seen since start

} // namespace fixomic::test
