#include "dsp/ProcessingGraph.h"

#include <algorithm>
#include <cstring>

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"

namespace fixomic {

ProcessingGraph::ProcessingGraph(AecStage& aec, PluginChain& chain) : aec_(aec), chain_(chain) {}

void ProcessingGraph::prepare(double sampleRate, int maxBlockSize) {
    maxBlock_ = std::max(1, maxBlockSize);
    stereo_.setSize(2, maxBlock_);
    stereo_.clear();
    midi_.ensureSize(256);
    inputMeter.prepare(sampleRate);
    outputMeter.prepare(sampleRate);
}

void ProcessingGraph::setAecEnabled(bool enabled) noexcept { aecEnabled_.store(enabled); }
void ProcessingGraph::setBypass(bool bypass) noexcept { bypass_.store(bypass); }
void ProcessingGraph::setMicPresent(bool present) noexcept { micPresent_.store(present); }
bool ProcessingGraph::aecActive() const noexcept { return aecActive_.load(); }

void ProcessingGraph::process(const float* mic, const float* ref, float* outL, float* outR, int n) noexcept {
    if (n <= 0) return;

    const bool micPresent = micPresent_.load();
    const bool bypass = bypass_.load();
    const bool aecActive = micPresent && !bypass && aecEnabled_.load() && ref != nullptr;
    aecActive_.store(aecActive);

    if (!micPresent) {
        std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
        inputMeter.push(outL, n);
        outputMeter.push(outL, n);
        return;
    }

    if (maxBlock_ <= 0) {   // not prepared: behave like bypass rather than touching empty buffers
        for (int i = 0; i < n; ++i) {
            const float s = mic[i];
            outL[i] = s;
            outR[i] = s;
        }
        inputMeter.push(mic, n);
        outputMeter.push(outL, n);
        return;
    }

    for (int pos = 0; pos < n; pos += maxBlock_) {
        const int len = std::min(maxBlock_, n - pos);
        const float* micIn = mic + pos;
        float* left = outL + pos;
        float* right = outR + pos;

        inputMeter.push(micIn, len);

        if (bypass) {
            // `left` may alias `micIn`; memmove-safe copy order: right first, then left.
            std::memcpy(right, micIn, sizeof(float) * static_cast<size_t>(len));
            if (left != micIn) std::memcpy(left, micIn, sizeof(float) * static_cast<size_t>(len));
        } else {
            float* work0 = stereo_.getWritePointer(0);
            float* work1 = stereo_.getWritePointer(1);
            if (aecActive)
                aec_.process(micIn, ref + pos, work0, len);
            else
                std::memcpy(work0, micIn, sizeof(float) * static_cast<size_t>(len));
            std::memcpy(work1, work0, sizeof(float) * static_cast<size_t>(len));

            // A view over the first `len` samples of the work buffer; no allocation for 2 channels.
            float* channels[2] = {work0, work1};
            juce::AudioBuffer<float> view(channels, 2, len);
            midi_.clear();
            chain_.process(view, midi_);

            std::memcpy(left, work0, sizeof(float) * static_cast<size_t>(len));
            std::memcpy(right, work1, sizeof(float) * static_cast<size_t>(len));
        }

        outputMeter.push(left, len);
    }
}

} // namespace fixomic
