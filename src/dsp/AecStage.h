#pragma once

#include <memory>

namespace fixomic {

// Mono acoustic echo cancellation (WebRTC AEC3) for the mic path, with the system output as the
// far-end reference. The stage processes fixed 10 ms frames internally and decouples them from
// the host block size with FIFOs, so latency is constant: exactly one frame.
//
// prepare()/reset() allocate and must not run on the audio thread; process() never allocates,
// logs or does I/O (the APM takes its own internal locks).
class AecStage {
public:
    AecStage();
    ~AecStage();
    AecStage(const AecStage&) = delete;
    AecStage& operator=(const AecStage&) = delete;

    // frame = sampleRate / 100 (supported: 8, 16, 32 and 48 kHz). Preallocates the FIFOs and
    // creates the APM. maxBlockSize is the largest n later passed to process().
    void prepare(double sampleRate, int maxBlockSize);

    // Clears the FIFOs (output primed with one frame of silence) and recreates the APM state.
    void reset();

    // mono; `out` may alias `mic`. Any n >= 0 is accepted; blocks larger than the prepared
    // maxBlockSize are processed in maxBlockSize-sized chunks.
    void process(const float* mic, const float* ref, float* out, int n) noexcept;

    // Latency added by the frame FIFOs: exactly one frame (480 at 48 kHz).
    int latencySamples() const noexcept;

    // Additional constant delay inherent to the APM (filter banks and AEC3 block processing),
    // measured with an impulse in prepare(). Total mic-to-output delay is
    // latencySamples() + apmDelaySamples().
    int apmDelaySamples() const noexcept;

    // Latest ERLE in dB from the APM statistics, NAN if not available (yet).
    float erleDb() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic
