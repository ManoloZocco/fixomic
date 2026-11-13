#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
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
// only while active); the graph never resets the AEC on the audio thread. When the AEC resumes
// after blocks that skipped it (global bypass, AEC switched off, mic absent, no reference), its
// FIFOs and the APM still hold the audio of its last active block: the first latencySamples() of
// output, plus one AEC frame for the APM's filter tail, are muted and then faded in over
// kResumeFadeSamples, so that old audio is never heard.
// The AEC's first activation after prepare() is not muted (it starts from a reset state).
//
// process() runs with denormals flushed to zero (juce::ScopedNoDenormals: the caller's mode is
// restored on return), and its output is always finite and within +-1: NaN and +-inf (a misbehaving
// plugin) become 0, larger samples are clamped, before the output meter and the caller see them.
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

    // The next process() calls output silence for `muteSamples`, then fade in linearly over
    // `fadeSamples` (gain k / fadeSamples for the k-th fade sample), then run at unity. Hides the
    // pop some microphones produce right after the IO starts. Applied to the final output (bypass
    // included) before the output meter; the AEC and the chain keep running underneath. Lock-free:
    // callable from any thread, also while process() runs; arming again starts over.
    void armStartupMute(int muteSamples, int fadeSamples) noexcept;

    // Fade-in after the muted stretch that hides the AEC's stale audio when it resumes (20 ms at
    // 48 kHz, as after the startup mute).
    static constexpr int kResumeFadeSamples = 960;

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

    // Startup mute request: bit 63 = pending, bits 32..62 = mute samples, bits 0..31 = fade samples.
    // One word so a request is never seen half-written; the audio thread takes it with exchange().
    std::atomic<std::uint64_t> startupRequest_{0};
    // Audio thread only.
    int muteLeft_ = 0;
    int fadeLength_ = 0;
    int fadePosition_ = 0;
    bool aecRan_ = false;      // the AEC processed a block since prepare() (which clears both)
    bool aecPaused_ = false;   // ... and a later block skipped it: its FIFOs / APM hold old audio
    int resumeTail_ = 480;     // one AEC frame (10 ms), muted on resume after latencySamples()
    void applyStartupGain(float* left, float* right, int n) noexcept;
};

} // namespace fixomic
