#pragma once

#include "core/Config.h"
#include "core/LatencyBudget.h"
#include "engine/EngineController.h"
#include "platform/mac/Permissions.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

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

// Where openIO() gets the System Audio Recording state (injectable for tests).
struct SystemAudioAccess {
    std::function<Permission()> status = systemAudioPermission;
    std::function<void(std::function<void(bool ok, juce::String error)>)> request = requestSystemAudio;
    // The silence fallback (see MacEngineBackend): whether the default output device is running.
    std::function<bool()> outputRunning;   // empty = the real default output device
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
// Permissions, checked before any HAL call (the TCC prompts block the thread that triggers them,
// see docs/notes/permissions.md):
//   microphone not determined -> the prompt is raised once, openIO() fails with
//     kWaitingForMicrophone ("In attesa del permesso per il microfono") until
//     onMicrophonePermission; denied -> fails. pendingPermission() says which prompt it waits for.
//   system audio (only with a tap) not determined -> requestSystemAudio() raises the prompt once on
//     a background queue, openIO() fails with kWaitingForSystemAudio until
//     onSystemAudioPermission; afterwards the tap is created normally. If that throwaway IO failed
//     (the prompt may not have been shown) the main thread never creates the tap in its place: the
//     next openIO() opens without it (AEC inactive, onWarning says why) and the one after asks
//     again. Denied -> the IO opens without the tap (AEC inactive) and onWarning tells where to
//     enable it.
//   Unless the permission is known to be granted, a tap that delivers only digital silence for
//     TapSilenceWatch::kSilentSeconds while the default output device is running raises onWarning
//     ("probably denied"), once per open.
//   requestMicrophone() / requestSystemAudio() (the first-run guide) share this state with openIO():
//     a prompt raised by either is the one the other waits for, never a second one.
//
// Threading: main thread only. The AEC stage, chain and graph must outlive the backend; nothing
// else may prepare them while the IO is open.
class MacEngineBackend : public EngineBackend {
public:
    using MicrophoneAccess = mac::MicrophoneAccess;

    MacEngineBackend(ProcessingGraph& graph, AecStage& aec, PluginChain& chain,
                     std::function<Config()> configProvider, MicrophoneAccess = {}, SystemAudioAccess = {});
    ~MacEngineBackend() override;   // closes the IO
    MacEngineBackend(const MacEngineBackend&) = delete;
    MacEngineBackend& operator=(const MacEngineBackend&) = delete;

    bool driverInstalled() override;                         // mac::driverInstalled()
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
    // The permission prompt that made the last openIO() fail (its error is then
    // kWaitingForMicrophone / kWaitingForSystemAudio); None after a success or any other failure.
    PendingPermission pendingPermission() const;
    const AggregateIO* io() const;     // diagnostics; nullptr when closed

    // The permission prompts, for the first-run guide: raised here unless openIO() already raised
    // them (or the other way round), then `done` runs once the user answered (main thread). At once
    // when there is nothing to ask: the microphone already decided or answered; the system audio
    // preflight known, or a prompt raised by this backend answered. The microphone prompt is raised
    // at most once per backend; the system audio one again after a failed throwaway IO (ok = false).
    void requestMicrophone(std::function<void(bool granted)> done);
    void requestSystemAudio(std::function<void(bool ok)> done);
    bool microphoneRequestPending() const;   // the microphone prompt raised here is up
    bool systemAudioRequestPending() const;  // the System Audio Recording prompt raised here is up
    // A System Audio Recording prompt raised here was answered (the outcome is unknown: the preflight
    // stays stale in this process, see Permissions.h).
    bool systemAudioAnswered() const;

    // The user answered the microphone prompt that openIO() raised. Main thread; re-evaluate the
    // engine (e.g. EngineController::onMicListChanged(), deferred).
    std::function<void(bool granted)> onMicrophonePermission;

    // The System Audio Recording prompt raised by openIO() was answered (outcome unknown), or the
    // throwaway IO that raises it failed (the next openIO() then goes without the tap). Main thread;
    // re-evaluate the engine, deferred.
    std::function<void()> onSystemAudioPermission;

    // The tap could not be created, the system audio permission is denied, or the tap stays silent
    // (permission probably denied): the IO runs without (working) echo cancellation. The kind says
    // which, the text says it in Italian. Main thread.
    std::function<void(WarningKind, juce::String)> onWarning;
    // The aggregate reported a lost / reconfigured device (forward to EngineController::onDeviceLost,
    // which may call closeIO() from inside the handler). Main thread.
    std::function<void()> onDeviceLost;

    static constexpr int kStartupMuteSamples = 14400;   // 300 ms at 48 kHz: hides the mic's start pop
    static constexpr int kStartupFadeSamples = 960;     // 20 ms fade-in after it
    static constexpr double kSilenceCheckSeconds = 1.0;

    static const char* const kWaitingForMicrophone;
    static const char* const kWaitingForSystemAudio;
    static juce::String systemAudioDeniedWarning();
    static juce::String systemAudioSilentWarning();
    static juce::String systemAudioUnavailableWarning(const juce::String& reason);

private:
    enum class Request { None, Pending, Done, Failed };

    // Applies the system audio permission to a tap request: false = do not open yet (`error` set).
    bool checkSystemAudio(bool& withTap, juce::String& error);
    void askMicrophone();     // raises the microphone prompt, unless it was raised already
    void askSystemAudio();    // raises the System Audio Recording prompt (not while one is up)
    void warn(WarningKind, const juce::String&);
    void startSilenceWatch();
    void stopSilenceWatch();

    ProcessingGraph& graph_;
    AecStage& aec_;
    PluginChain& chain_;
    std::function<Config()> configProvider_;
    MicrophoneAccess micAccess_;
    SystemAudioAccess audioAccess_;
    Request micRequest_ = Request::None;     // the microphone prompt: raised at most once per backend
    std::vector<std::function<void(bool)>> micWaiters_;     // requestMicrophone() callers
    Request audioRequest_ = Request::None;   // the system audio prompt, until it was raised once
    juce::String audioRequestError_;         // why the last throwaway IO failed (Request::Failed)
    std::vector<std::function<void(bool)>> audioWaiters_;   // requestSystemAudio() callers
    std::atomic<bool> refHeard_{false};      // set by the IO thread on a non-zero tap sample
    struct SilenceTimer;
    std::unique_ptr<SilenceTimer> silenceTimer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // guards the prompt completion
    std::unique_ptr<SystemAudioTap> tap_;
    std::unique_ptr<AggregateIO> io_;   // destroyed before tap_
    int preparedBlock_ = 0;             // buffer size of the last successful prepare, 0 = none
    PendingPermission pending_ = PendingPermission::None;   // of the last openIO()
};

} // namespace fixomic::mac
