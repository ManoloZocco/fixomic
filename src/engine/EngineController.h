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
};

// On-demand engine state machine. The IO is open only while some app reads fixomic Mic:
//
//   demand on   -> Starting -> openIO(withTap = aecEnabled && !globalBypass) -> Running
//   demand off  -> IO closed kStopGraceSeconds later, unless demand comes back first -> Idle
//
// Device conditions are checked on every event, with or without demand, in this order: driver
// missing -> DriverMissing, configured mic absent (or none selected) -> MicMissing, mic without
// 48 kHz -> UnsupportedSampleRate. In those states no IO is open (fixomic Mic is silent: the driver
// outputs silence without a writer) and the controller never falls back to another microphone.
// A failed openIO() -> Error (lastError()), retried every kRetrySeconds from tick() while there is
// demand, and on every event.
//
// The plugin chain is not the controller's business: stopping never unloads plugins.
//
// Threading: everything, onStateChanged included, runs on the message thread. onStateChanged fires
// only when the state actually changes.
class EngineController {
public:
    EngineController(EngineBackend&, std::function<double()> nowSeconds);
    ~EngineController();   // closes the IO if open: the backend must outlive the controller
    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // Input device, channel, buffer size or tap use (aecEnabled && !globalBypass) changed while the
    // IO is open -> closeIO + openIO. Other fields only get stored.
    void setConfig(const Config&);
    void onDemandChanged(bool inUse);
    void onMicListChanged();         // re-checks the devices: a mic that came back restarts on demand
    void onDefaultOutputChanged();   // IO open with a tap -> rebuilt in place, state stays Running
    void onDeviceLost();             // IO closed, devices re-checked, reopened on demand
    void onSystemWake();             // IO open -> closed and reopened; Idle -> nothing
    void tick();                     // every ~200 ms: grace period and Error retries

    EngineState state() const;
    juce::String lastError() const;
    bool ioOpen() const;

    std::function<void(EngineState)> onStateChanged;

    static constexpr double kStopGraceSeconds = 3.0;   // plan decision
    static constexpr double kRetrySeconds = 2.0;

private:
    static bool wantsTap(const Config&);
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
    EngineState state_ = EngineState::Idle;
    juce::String lastError_;
};

} // namespace fixomic
