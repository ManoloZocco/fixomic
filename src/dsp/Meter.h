#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace fixomic {

// Peak level meter with a 20 dB/s fall-off. push() is called by the single audio thread; peakDb()
// may be read from any thread. Lock-free, never allocates.
class Meter {
public:
    static constexpr float kFloorDb = -100.0f;
    static constexpr float kDecayDbPerSecond = 20.0f;

    // Sample rate used to turn block lengths into decay time. Message thread, before audio runs.
    void prepare(double sampleRate) noexcept { sampleRate_.store(sampleRate > 0.0 ? sampleRate : 48000.0); }

    // Audio thread. The held peak decays by the block's duration, then takes the block's own peak
    // if that is higher.
    void push(const float* samples, int n) noexcept {
        if (n <= 0) return;
        float blockPeak = 0.0f;
        if (samples != nullptr)
            for (int i = 0; i < n; ++i) blockPeak = std::max(blockPeak, std::abs(samples[i]));

        const double seconds = static_cast<double>(n) / sampleRate_.load(std::memory_order_relaxed);
        const float decayed =
            peak_.load(std::memory_order_relaxed) *
            static_cast<float>(std::pow(10.0, -static_cast<double>(kDecayDbPerSecond) * seconds / 20.0));
        peak_.store(std::max(blockPeak, decayed), std::memory_order_relaxed);
    }

    // Held peak in dBFS, never below -100.
    float peakDb() const noexcept {
        const float p = peak_.load(std::memory_order_relaxed);
        if (!(p > 1e-5f)) return kFloorDb;   // 1e-5 == -100 dB; also catches NaN
        return std::max(kFloorDb, 20.0f * std::log10(p));
    }

private:
    std::atomic<float> peak_{0.0f};   // linear amplitude
    std::atomic<double> sampleRate_{48000.0};
};

} // namespace fixomic
