#pragma once

#include "core/Config.h"
#include "core/LatencyBudget.h"
#include "engine/EngineController.h"
#include "platform/mac/Permissions.h"

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

// Where MacEngineBackend::openIO() gets the microphone authorization (injectable for tests).
struct MicrophoneAccess {
    std::function<Permission()> status = microphonePermission;
    std::function<void(std::function<void(bool)>)> request = requestMicrophone;
};

// EngineBackend on Core Audio: the private aggregate (mic + fixomic Bridge + optional system audio
// tap, Task 6) whose IOProc runs the ProcessingGraph.
//
// openIO() reads the config through `configProvider`. The AEC stage and the plugin chain are
// prepared for 48 kHz and the configured buffer size only when that differs from the last
// successful open (plugins keep their state and are never unloaded: closeIO() leaves the chain
// alone); otherwise the AEC is just reset (fresh echo estimate). The graph is prepared, the AEC /
// bypass flags applied, then openIO() creates the tap
// when asked (if that fails the IO opens without it, AEC inactive, and onWarning says why),
// creates the aggregate, arms the graph's startup mute (kStartupMuteSamples + kStartupFadeSamples)
// and starts it.
//
// Threading: main thread only. The AEC stage, chain and graph must outlive the backend; nothing
// else may prepare them while the IO is open.
class MacEngineBackend : public EngineBackend {
public:
    using MicrophoneAccess = mac::MicrophoneAccess;

    MacEngineBackend(ProcessingGraph& graph, AecStage& aec, PluginChain& chain,
                     std::function<Config()> configProvider, MicrophoneAccess = {});
    ~MacEngineBackend() override;   // closes the IO
    MacEngineBackend(const MacEngineBackend&) = delete;
    MacEngineBackend& operator=(const MacEngineBackend&) = delete;

    bool driverInstalled() override;                         // fixomic Mic is present
    bool micAvailable(const juce::String& uid) override;     // present, has inputs, not a fixomic device
    bool micSupports48k(const juce::String& uid) override;   // offers 48 kHz among its nominal rates
    bool openIO(bool withTap, juce::String& error) override;
    void closeIO() override;
    void applyFlags(bool aecEnabled, bool bypass) override;   // graph flags, no rebuild

    // Call after other code prepared the AEC stage or the chain with other settings: the next
    // openIO() prepares them again.
    void forgetPrepared();

    // Of the open IO (nullopt when closed): device and buffer frames from the aggregate, aecFrames
    // when the AEC ran on the last block, pluginFrames unless globally bypassed.
    std::optional<LatencyBreakdown> latency() const;
    bool ioOpen() const;
    bool tapActive() const;            // the open IO has a system audio reference
    const AggregateIO* io() const;     // diagnostics; nullptr when closed

    // The user answered the microphone prompt that openIO() raised. Main thread; re-evaluate the
    // engine (e.g. EngineController::onMicListChanged(), deferred).
    std::function<void(bool granted)> onMicrophonePermission;

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
    MicrophoneAccess micAccess_;
    bool micRequested_ = false;         // the prompt is raised at most once per backend
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // guards the prompt completion
    std::unique_ptr<SystemAudioTap> tap_;
    std::unique_ptr<AggregateIO> io_;   // destroyed before tap_
    int preparedBlock_ = 0;             // buffer size of the last successful prepare, 0 = none
};

} // namespace fixomic::mac
