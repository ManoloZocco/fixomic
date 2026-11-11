#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <vector>

#include "dsp/Meter.h"

namespace fixomic {

class AecStage;
class PluginChain;

// The per-block pipeline: mic -> AEC (when a reference is available) -> plugin chain -> stereo
// out. No Core Audio in here; the audio callback owns the buffers and just calls process().
//
// The graph does not own or prepare the AEC stage and the chain: the caller prepares them (message
// thread) with the same sample rate and maxBlockSize before calling prepare() on the graph.
//
// Threading: prepare() and the setters are message-thread; process() is the audio thread and never
// allocates, locks, logs or throws. The flags are atomics read once per process() call. When the
// AEC toggles between blocks the stream has a short discontinuity (the AEC adds its fixed delay
// only while active); the graph never resets the AEC on the audio thread.
class ProcessingGraph {
public:
    ProcessingGraph(AecStage&, PluginChain&);

    // Preallocates the stereo work buffer and primes the meters. Not concurrent with process().
    void prepare(double sampleRate, int maxBlockSize);

    // `mic` and `ref` are mono (`ref == nullptr` when there is no system-output tap), `outL`/`outR`
    // are written; `outL` may alias `mic`. Blocks longer than the prepared maxBlockSize are
    // processed in chunks. With mic absent, `mic` is not read.
    void process(const float* mic, const float* ref, float* outL, float* outR, int n) noexcept;

    void setAecEnabled(bool) noexcept;   // the AEC runs only if enabled && ref != nullptr
    void setBypass(bool) noexcept;       // mic -> out directly (L = R), skips AEC and chain
    void setMicPresent(bool) noexcept;   // false -> out = 0
    bool aecActive() const noexcept;     // state of the last block

    Meter inputMeter, outputMeter;

private:
    AecStage& aec_;
    PluginChain& chain_;
    int maxBlock_ = 0;
    juce::AudioBuffer<float> stereo_;
    juce::MidiBuffer midi_;
    std::atomic<bool> aecEnabled_{false};
    std::atomic<bool> bypass_{false};
    std::atomic<bool> micPresent_{true};
    std::atomic<bool> aecActive_{false};
};

} // namespace fixomic
