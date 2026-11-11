#include "core/LatencyBudget.h"

namespace fixomic {

double LatencyBreakdown::totalMs() const {
    if (!(sampleRate > 0.0)) return 0.0;
    const double frames = static_cast<double>(inputDeviceFrames) + outputDeviceFrames + bufferFrames +
                          aecFrames + pluginFrames;
    return frames / sampleRate * 1000.0;
}

} // namespace fixomic
