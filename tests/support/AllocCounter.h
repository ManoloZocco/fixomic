#pragma once

namespace fixomic::test {

// Counts calls to the global operator new (replaced for the whole test binary in
// AllocCounter.cpp) between start and stop, on the calling thread only: allocations made by
// other threads (GCD queues, JUCE or driver background threads started by earlier tests) are
// ignored. Lets a test prove that a real-time code path never touches the heap. Start and stop
// must be called from the thread that runs the code under test. Not reentrant: one counting
// window at a time.
void startCountingAllocations() noexcept;
int stopCountingAllocations() noexcept;   // returns the allocations seen since start

} // namespace fixomic::test
