#pragma once

namespace fixomic {

// Where the mic-to-virtual-mic delay goes, all in frames at `sampleRate`.
struct LatencyBreakdown {
    double sampleRate;
    int inputDeviceFrames;
    int outputDeviceFrames;
    int bufferFrames;
    int aecFrames;
    int pluginFrames;

    // Sum of all frames, in milliseconds. 0 when sampleRate is not positive.
    double totalMs() const;
};

} // namespace fixomic
