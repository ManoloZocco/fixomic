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
                                   std::function<Config()> configProvider, MicrophoneAccess micAccess)
    : graph_(graph), aec_(aec), chain_(chain), configProvider_(std::move(configProvider)),
      micAccess_(std::move(micAccess))
{
}

MacEngineBackend::~MacEngineBackend()
{
    *alive_ = false;
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

    // Never reach the HAL without microphone access: while the TCC prompt is up, creating the IO
    // proc blocks the calling (main) thread until the user answers.
    switch (micAccess_.status ? micAccess_.status() : Permission::Granted) {
    case Permission::Granted: break;
    case Permission::NotDetermined:
        if (!micRequested_ && micAccess_.request) {
            micRequested_ = true;
            micAccess_.request([this, alive = alive_](bool granted) {
                if (*alive && onMicrophonePermission)
                    onMicrophonePermission(granted);
            });
        }
        error = "In attesa del permesso per il microfono";
        return false;
    case Permission::Denied:
        error = juce::String::fromUTF8("Permesso microfono negato: abilitalo in Impostazioni di Sistema \u2192 "
                                       "Privacy e sicurezza \u2192 Microfono");
        return false;
    }

    const Config config = configProvider_ ? configProvider_() : Config{};
    const int block = std::max(config.bufferSize, 1);

    // The IO is closed: nothing runs process() while these (allocating) calls run. Re-preparing
    // the chain would call prepareToPlay on every plugin (slow, and it resets their state) at every
    // start, so it happens only when the block size changes (the rate is always 48 kHz). The AEC
    // is reset instead of re-prepared: same buffers, fresh APM state.
    if (block != preparedBlock_) {
        aec_.prepare(devices::kSampleRate, block);
        chain_.prepare(devices::kSampleRate, block);
        preparedBlock_ = block;   // prepares cannot fail: retries after a failed open skip them
    } else {
        aec_.reset();
    }
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

void MacEngineBackend::forgetPrepared()
{
    preparedBlock_ = 0;
}

void MacEngineBackend::applyFlags(bool aecEnabled, bool bypass)
{
    graph_.setAecEnabled(aecEnabled);
    graph_.setBypass(bypass);
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
