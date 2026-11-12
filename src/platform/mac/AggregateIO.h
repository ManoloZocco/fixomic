#pragma once

#include "core/LatencyBudget.h"
#include "platform/mac/SystemAudioTap.h"

#include <CoreAudio/AudioHardware.h>
#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace fixomic::mac {

// One private aggregate device that does the whole audio IO of the engine in a single IOProc:
//
//   main sub-device (clock)  the selected microphone, switched to 48 kHz first
//   sub-device               fixomic Bridge (hidden output of the driver), drift compensated
//   tap                      the SystemAudioTap (optional), drift compensated
//
// The IOProc picks the mic channel and the tap reference out of the aggregate's input buffers,
// deinterleaves them into buffers allocated in create(), calls the Callback and interleaves its
// stereo output into the Bridge buffer. Every other output buffer is zeroed. How the aggregate's
// buffers are matched to mic / tap / Bridge is described in docs/notes/aggregate.md.
//
// Threading: create(), start(), stop(), the destructor and every accessor are for the main
// thread. The Callback runs on the HAL IO thread and must be real-time safe; it may be called
// more than once per IO cycle (in chunks) if the HAL ever delivers more frames than were
// preallocated, so `n` is not guaranteed to equal the buffer size.
class AggregateIO {
public:
    using Callback = std::function<void(const float* mic, const float* ref, float* outL, float* outR, int n)>;

    struct Params {
        juce::String micUid;
        int micChannel;              // 0-based, over all input channels of the mic
        const SystemAudioTap* tap;   // nullable: then `ref` is silence; must outlive the AggregateIO
        int bufferSize;              // requested IO buffer, clamped to the aggregate's range
    };

    // Returns nullptr and fills `error` on failure; an aggregate created on the way is destroyed.
    // A mic without 48 kHz fails with an error containing "48 kHz".
    static std::unique_ptr<AggregateIO> create(const Params& params, Callback callback, juce::String& error);

    bool start();  // false on failure, see lastError()
    void stop();
    ~AggregateIO();  // stops and destroys the aggregate

    // Input/output device frames = kAudioDevicePropertyLatency + kAudioDevicePropertySafetyOffset
    // of the aggregate + kAudioStreamPropertyLatency of the mic / Bridge stream it uses.
    // bufferFrames = 2 x IO buffer: a sample waits one buffer to be delivered and its output is
    // presented one buffer later. aecFrames and pluginFrames are left 0 (the engine fills them).
    LatencyBreakdown latency() const;

    // Mic or fixomic Bridge disappeared, or the aggregate was invalidated or reconfigured
    // (IsAlive / DeviceHasChanged / device list). Called once, on the main queue; the owner is
    // expected to destroy this object and build a new one (it may do so inside the handler).
    std::function<void()> onDeviceLost;

    // ---- diagnostics (main thread) ----
    struct Stats {
        std::uint64_t callbacks = 0;       // IOProc invocations
        std::uint64_t frames = 0;          // mic frames delivered to the Callback
        std::uint64_t sampleTimeGaps = 0;  // input sample time != previous + previous frames
        std::uint64_t overloads = 0;       // kAudioDeviceProcessorOverload notifications
        std::uint32_t lastFrames = 0;
        std::uint32_t maxFrames = 0;
    };
    Stats stats() const;
    AudioObjectID deviceId() const;
    int bufferFrames() const;          // IO buffer actually set on the aggregate
    double nominalSampleRate() const;  // of the aggregate
    double actualSampleRate() const;   // kAudioDevicePropertyActualSampleRate, 0 when stopped
    juce::String describe() const;     // stream mapping and latency figures, for logs and fxprobe
    juce::String lastError() const;

    AggregateIO(const AggregateIO&) = delete;
    AggregateIO& operator=(const AggregateIO&) = delete;

private:
    AggregateIO();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic::mac
