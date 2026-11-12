#pragma once

#include "core/Config.h"
#include "core/LatencyBudget.h"
#include "engine/EngineController.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>

namespace fixomic {
class AecStage;
class PluginChain;
class ProcessingGraph;
}

namespace fixomic::mac {

class AggregateIO;
class SystemAudioTap;

// EngineBackend on Core Audio: the private aggregate (mic + fixomic Bridge + optional system audio
// tap, Task 6) whose IOProc runs the ProcessingGraph.
//
// openIO() reads the config through `configProvider`, prepares the AEC stage, the plugin chain
// and the graph for 48 kHz and the configured buffer size (plugins are re-prepared, never
// unloaded: closeIO() leaves the chain alone), applies the AEC / bypass flags, creates the tap
// when asked (if that fails the IO opens without it, AEC inactive, and onWarning says why),
// creates the aggregate, arms the graph's startup mute (kStartupMuteSamples + kStartupFadeSamples)
// and starts it.
//
// Threading: main thread only. The AEC stage, chain and graph must outlive the backend; nothing
// else may prepare them while the IO is open.
class MacEngineBackend : public EngineBackend {
public:
    MacEngineBackend(ProcessingGraph& graph, AecStage& aec, PluginChain& chain,
                     std::function<Config()> configProvider);
    ~MacEngineBackend() override;   // closes the IO
    MacEngineBackend(const MacEngineBackend&) = delete;
    MacEngineBackend& operator=(const MacEngineBackend&) = delete;

    bool driverInstalled() override;                         // fixomic Mic is present
    bool micAvailable(const juce::String& uid) override;     // present, has inputs, not a fixomic device
    bool micSupports48k(const juce::String& uid) override;   // offers 48 kHz among its nominal rates
    bool openIO(bool withTap, juce::String& error) override;
    void closeIO() override;

    // Of the open IO (nullopt when closed): device and buffer frames from the aggregate, aecFrames
    // when the AEC ran on the last block, pluginFrames unless globally bypassed.
    std::optional<LatencyBreakdown> latency() const;
    bool ioOpen() const;
    bool tapActive() const;            // the open IO has a system audio reference
    const AggregateIO* io() const;     // diagnostics; nullptr when closed

    // The tap could not be created: the IO runs without echo cancellation. Main thread.
    std::function<void(juce::String)> onWarning;
    // The aggregate reported a lost / reconfigured device (forward to EngineController::onDeviceLost,
    // which may call closeIO() from inside the handler). Main thread.
    std::function<void()> onDeviceLost;

    static constexpr int kStartupMuteSamples = 14400;   // 300 ms at 48 kHz: hides the mic's start pop
    static constexpr int kStartupFadeSamples = 960;     // 20 ms fade-in after it

private:
    ProcessingGraph& graph_;
    AecStage& aec_;
    PluginChain& chain_;
    std::function<Config()> configProvider_;
    std::unique_ptr<SystemAudioTap> tap_;
    std::unique_ptr<AggregateIO> io_;   // destroyed before tap_
};

} // namespace fixomic::mac
