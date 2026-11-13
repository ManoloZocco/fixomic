#include "dsp/ProcessingGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"

namespace fixomic {

namespace {

constexpr float kOutputLimit = 1.0f;

// NaN and +-inf -> 0, everything else clamped to +-kOutputLimit. Real-time safe.
void sanitize(float* samples, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        const float s = samples[i];
        samples[i] = std::isfinite(s) ? std::clamp(s, -kOutputLimit, kOutputLimit) : 0.0f;
    }
}

} // namespace

ProcessingGraph::ProcessingGraph(AecStage& aec, PluginChain& chain) : aec_(aec), chain_(chain) {}

void ProcessingGraph::prepare(double sampleRate, int maxBlockSize) {
    maxBlock_ = std::max(1, maxBlockSize);
    stereo_.setSize(2, maxBlock_);
    stereo_.clear();
    midi_.ensureSize(256);
    inputMeter.prepare(sampleRate);
    outputMeter.prepare(sampleRate);
    aecRan_ = aecPaused_ = false;   // the caller prepared or reset the AEC with it
    resumeTail_ = std::max(1, static_cast<int>(std::lround(sampleRate / 100.0)));   // one AEC frame
}

void ProcessingGraph::setAecEnabled(bool enabled) noexcept { aecEnabled_.store(enabled); }
void ProcessingGraph::setBypass(bool bypass) noexcept { bypass_.store(bypass); }
void ProcessingGraph::setMicPresent(bool present) noexcept { micPresent_.store(present); }
bool ProcessingGraph::aecActive() const noexcept { return aecActive_.load(); }

void ProcessingGraph::armStartupMute(int muteSamples, int fadeSamples) noexcept {
    const auto mute = static_cast<std::uint64_t>(std::max(muteSamples, 0)) & 0x7fffffffu;
    const auto fade = static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::max(fadeSamples, 0)));
    startupRequest_.store((std::uint64_t{1} << 63) | (mute << 32) | fade, std::memory_order_release);
}

void ProcessingGraph::applyStartupGain(float* left, float* right, int n) noexcept {
    if (muteLeft_ == 0 && fadePosition_ >= fadeLength_) return;
    int i = 0;
    if (muteLeft_ > 0) {
        i = std::min(muteLeft_, n);
        std::memset(left, 0, sizeof(float) * static_cast<size_t>(i));
        std::memset(right, 0, sizeof(float) * static_cast<size_t>(i));
        muteLeft_ -= i;
    }
    for (; i < n && fadePosition_ < fadeLength_; ++i, ++fadePosition_) {
        const float gain = static_cast<float>(fadePosition_) / static_cast<float>(fadeLength_);
        left[i] *= gain;
        right[i] *= gain;
    }
}

void ProcessingGraph::process(const float* mic, const float* ref, float* outL, float* outR, int n) noexcept {
    if (n <= 0) return;
    const juce::ScopedNoDenormals noDenormals;   // the AEC and the plugins; restored on return

    if (const std::uint64_t request = startupRequest_.exchange(0, std::memory_order_acquire); request != 0) {
        muteLeft_ = static_cast<int>((request >> 32) & 0x7fffffffu);
        fadeLength_ = static_cast<int>(static_cast<std::uint32_t>(request));
        fadePosition_ = 0;
    }

    const bool micPresent = micPresent_.load();
    const bool bypass = bypass_.load();
    const bool aecActive = micPresent && !bypass && aecEnabled_.load() && ref != nullptr;
    aecActive_.store(aecActive);
    if (aecActive) {
        if (aecPaused_) {
            // Resumed: its first latencySamples() of output are audio from before the pause, and the
            // APM's filters ring for about one more frame (measured: -54 dBFS after a -6 dBFS tone,
            // -91 dBFS one frame later). Muted (a longer startup mute in progress is kept), then
            // faded in.
            muteLeft_ = std::max(muteLeft_, aec_.latencySamples() + resumeTail_);
            fadeLength_ = std::max(fadeLength_, kResumeFadeSamples);
            fadePosition_ = 0;
            aecPaused_ = false;
        }
        aecRan_ = true;
    } else if (aecRan_) {
        aecPaused_ = true;
    }

    if (!micPresent) {
        std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
        applyStartupGain(outL, outR, n);   // only advances the mute / fade position
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
        inputMeter.push(mic, n);   // before the gain: `outL` may alias `mic`
        applyStartupGain(outL, outR, n);
        sanitize(outL, n);
        sanitize(outR, n);
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
            if (pos == 0) chain_.processSkipped();   // VST3 parameter flushes still get served
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

        applyStartupGain(left, right, len);
        sanitize(left, len);   // what the Bridge receives: never NaN / inf, never beyond +-1
        sanitize(right, len);
        outputMeter.push(left, len);
    }
}

} // namespace fixomic
