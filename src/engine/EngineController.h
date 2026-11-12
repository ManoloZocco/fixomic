#pragma once

#include "core/Config.h"

#include <juce_core/juce_core.h>

#include <functional>

namespace fixomic {

enum class EngineState { Idle, Starting, Running, MicMissing, DriverMissing, UnsupportedSampleRate, Error };

const char* toString(EngineState) noexcept;

// What the controller needs from the platform: device checks and opening / closing the audio IO.
// Injectable so the state machine is testable without Core Audio. Message thread only.
struct EngineBackend {
    virtual ~EngineBackend() = default;
    virtual bool driverInstalled() = 0;
    virtual bool micAvailable(const juce::String& uid) = 0;
    virtual bool micSupports48k(const juce::String& uid) = 0;
    // Creates the system audio tap (if requested) and the aggregate IO and starts it. False and
    // `error` filled on failure, with nothing left open.
    virtual bool openIO(bool withTap, juce::String& error) = 0;
    virtual void closeIO() = 0;   // no-op when nothing is open
    // Applies the AEC / bypass switches to the open IO without rebuilding it (no gap, no startup
    // mute). Called only while the IO is open.
    virtual void applyFlags(bool aecEnabled, bool bypass) = 0;
};

// On-demand engine state machine. The IO is open only while some app reads fixomic Mic:
//
//   demand on   -> Starting -> openIO(withTap = aecEnabled) -> Running
//   demand off  -> IO closed kStopGraceSeconds later, unless demand comes back first -> Idle
//
// Device conditions are checked on every event, with or without demand, in this order: driver
// missing -> DriverMissing, configured mic absent (or none selected) -> MicMissing, mic without
// 48 kHz -> UnsupportedSampleRate. In those states no IO is open (fixomic Mic is silent: the driver
// outputs silence without a writer) and the controller never falls back to another microphone.
// A failed openIO() -> Error (lastError()), retried every kRetrySeconds from tick() while there is
// demand, and on every event; without demand a failure ends in Idle (lastError() keeps the reason).
// A device lost within kLostAfterOpenSeconds of a successful open is not reopened at once (that
// could loop): Error, retried by tick() like a failed open. A Core Audio restart always closes the
// IO (it is dead, whatever the state says) and re-evaluates.
//
// Global bypass keeps the IO and its tap (the graph just skips AEC and chain), so toggling it is
// instant: applied through EngineBackend::applyFlags().
//
// The plugin chain is not the controller's business: stopping never unloads plugins.
//
// Threading: everything, onStateChanged included, runs on the message thread. onStateChanged fires
// only when the state actually changes. Its handler (like the backend's warning / device-lost
// handlers) must not call back into the controller synchronously: defer such calls (e.g.
// juce::MessageManager::callAsync).
class EngineController {
public:
    EngineController(EngineBackend&, std::function<double()> nowSeconds);
    ~EngineController();   // closes the IO if open: the backend must outlive the controller
    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // Input device, channel, buffer size or aecEnabled (tap use) changed while the IO is open ->
    // closeIO + openIO. globalBypass changed while open -> applyFlags() only. Other fields are
    // only stored.
    void setConfig(const Config&);
    void onDemandChanged(bool inUse);
    void onMicListChanged();         // re-checks the devices: a mic that came back restarts on demand
    void onDefaultOutputChanged();   // IO open with a tap -> rebuilt in place, state stays Running
    void onDeviceLost();             // IO closed, devices re-checked, reopened on demand
    void onSystemWake();             // IO open -> closed and reopened; Idle -> nothing
    // Core Audio restarted (coreaudiod: a driver install / uninstall, or any other cause): the open
    // IO died with it. Closed and the devices re-checked; reopened on demand. Unlike onDeviceLost()
    // never throttled: the restart is a one-off event, not something the open caused.
    void onServiceRestarted();
    void tick();                     // every ~200 ms: grace period and Error retries

    EngineState state() const;
    juce::String lastError() const;
    bool ioOpen() const;

    std::function<void(EngineState)> onStateChanged;

    static constexpr double kStopGraceSeconds = 3.0;   // plan decision
    static constexpr double kRetrySeconds = 2.0;
    static constexpr double kLostAfterOpenSeconds = 1.0;

private:
    static bool wantsTap(const Config&);
    EngineState blockedState();   // DriverMissing / MicMissing / UnsupportedSampleRate, or Idle
    void evaluate();
    void open();
    void close();
    void setState(EngineState);

    EngineBackend& backend_;
    std::function<double()> now_;
    Config config_;
    bool demand_ = false;
    double demandOffSince_ = 0.0;
    bool ioOpen_ = false;
    bool ioWithTap_ = false;
    double lastAttempt_ = 0.0;
    double lastOpened_ = 0.0;
    EngineState state_ = EngineState::Idle;
    juce::String lastError_;
};

} // namespace fixomic
