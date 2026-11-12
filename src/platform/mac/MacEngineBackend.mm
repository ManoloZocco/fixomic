#include "platform/mac/MacEngineBackend.h"

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "platform/mac/AggregateIO.h"
#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/CoreAudioUtil.h"
#include "platform/mac/FixomicDevices.h"
#include "platform/mac/SystemAudioTap.h"

#include <algorithm>
#include <cmath>

namespace fixomic::mac {

MacEngineBackend::MacEngineBackend(ProcessingGraph& graph, AecStage& aec, PluginChain& chain,
                                   std::function<Config()> configProvider)
    : graph_(graph), aec_(aec), chain_(chain), configProvider_(std::move(configProvider))
{
}

MacEngineBackend::~MacEngineBackend()
{
    closeIO();
}

bool MacEngineBackend::driverInstalled()
{
    return findDeviceByUid(devices::kMicUid).has_value();
}

bool MacEngineBackend::micAvailable(const juce::String& uid)
{
    if (uid.isEmpty() || uid.startsWith("fixomic_"))
        return false;
    const auto device = findDeviceByUid(uid);
    return device && util::channelCount(*device, kAudioObjectPropertyScopeInput) > 0;
}

bool MacEngineBackend::micSupports48k(const juce::String& uid)
{
    const auto device = findDeviceByUid(uid);
    if (!device)
        return false;
    const auto rates = util::nominalRates(*device);
    return std::any_of(rates.begin(), rates.end(),
                       [](double rate) { return std::abs(rate - devices::kSampleRate) < 0.5; });
}

bool MacEngineBackend::openIO(bool withTap, juce::String& error)
{
    closeIO();
    const Config config = configProvider_ ? configProvider_() : Config{};
    const int block = std::max(config.bufferSize, 1);

    // The IO is closed: nothing runs process() while these (allocating) prepares run. The chain
    // keeps its plugins; they are only prepared again.
    aec_.prepare(devices::kSampleRate, block);
    chain_.prepare(devices::kSampleRate, block);
    graph_.prepare(devices::kSampleRate, block);
    graph_.setAecEnabled(config.aecEnabled);
    graph_.setBypass(config.globalBypass);
    graph_.setMicPresent(true);

    if (withTap) {
        juce::String tapError;
        tap_ = SystemAudioTap::create(tapError);
        if (!tap_ && onWarning)
            onWarning("system audio tap unavailable, echo cancellation is off: " + tapError);
    }

    // Without a tap the aggregate hands the callback a silent reference: pass none to the graph so
    // the AEC stays inactive (and adds no delay).
    ProcessingGraph* graph = &graph_;
    const bool hasReference = tap_ != nullptr;
    auto io = AggregateIO::create(
        {config.inputDeviceUid, config.inputChannel, tap_.get(), block},
        [graph, hasReference](const float* mic, const float* ref, float* outL, float* outR, int n) {
            graph->process(mic, hasReference ? ref : nullptr, outL, outR, n);
        },
        error);
    if (!io) {
        tap_.reset();
        return false;
    }
    io->onDeviceLost = [this] {
        const auto handler = onDeviceLost;   // a copy: the handler may close the IO
        if (handler)
            handler();
    };

    graph_.armStartupMute(kStartupMuteSamples, kStartupFadeSamples);
    if (!io->start()) {
        error = io->lastError();
        io.reset();
        tap_.reset();
        return false;
    }
    io_ = std::move(io);
    return true;
}

void MacEngineBackend::closeIO()
{
    if (io_) {
        io_->stop();
        io_.reset();
    }
    tap_.reset();
}

std::optional<LatencyBreakdown> MacEngineBackend::latency() const
{
    if (!io_)
        return std::nullopt;
    LatencyBreakdown breakdown = io_->latency();
    breakdown.aecFrames = graph_.aecActive() ? aec_.latencySamples() : 0;
    const bool bypassed = configProvider_ && configProvider_().globalBypass;
    breakdown.pluginFrames = bypassed ? 0 : chain_.totalLatencySamples();
    return breakdown;
}

bool MacEngineBackend::ioOpen() const
{
    return io_ != nullptr;
}

bool MacEngineBackend::tapActive() const
{
    return io_ != nullptr && tap_ != nullptr;
}

const AggregateIO* MacEngineBackend::io() const
{
    return io_.get();
}

} // namespace fixomic::mac
