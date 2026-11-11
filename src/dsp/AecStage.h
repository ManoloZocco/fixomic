#pragma once

#include <memory>

namespace fixomic {

// Mono acoustic echo cancellation (WebRTC AEC3) for the mic path, with the system output as the
// far-end reference. The stage processes fixed 10 ms frames internally and decouples them from
// the host block size with FIFOs, so the mic-to-output delay is constant: one frame of FIFO
// plus the APM's own algorithmic delay (430 samples at 48 kHz, 910 in total).
//
// Threading: prepare() and reset() allocate (reset() recreates and warms up the APM) and must be
// called from a non-audio thread while process() is not running. process() never allocates,
// logs or does I/O (the APM takes its own internal locks).
class AecStage {
public:
    AecStage();
    ~AecStage();
    AecStage(const AecStage&) = delete;
    AecStage& operator=(const AecStage&) = delete;

    // frame = sampleRate / 100. Preallocates the FIFOs, creates and warms up the APM and measures
    // its delay. maxBlockSize is the largest n later passed to process(). Sample rates outside
    // 8000..192000 Hz leave the stage in pass-through (latencySamples() == 0, erleDb() == NAN).
    void prepare(double sampleRate, int maxBlockSize);

    // Clears the FIFOs (output primed with one frame of silence) and recreates the APM state.
    // Not for the audio thread.
    void reset();

    // mono; `out` may alias `mic`. Any n >= 0 is accepted; blocks larger than the prepared
    // maxBlockSize are processed in maxBlockSize-sized chunks. If the stage is not prepared (or
    // was prepared with an unsupported rate) the mic is passed through unchanged.
    void process(const float* mic, const float* ref, float* out, int n) noexcept;

    // Total constant mic-to-output delay in samples: one FIFO frame plus apmDelaySamples()
    // (910 at 48 kHz). 0 when in pass-through.
    int latencySamples() const noexcept;

    // The APM's own algorithmic delay, measured with an impulse in prepare(). Diagnostic.
    int apmDelaySamples() const noexcept;

    // Our own echo-return-loss-enhancement estimate in dB: smoothed 10*log10(E_mic / E_out),
    // updated only while the reference is louder than -60 dBFS (otherwise the last value is
    // held). NAN until the first valid frame. Lock-free; safe to read from any thread.
    float erleDb() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic
