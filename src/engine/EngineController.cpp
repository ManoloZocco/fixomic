#include "engine/EngineController.h"

namespace fixomic {

const char* toString(EngineState state) noexcept {
    switch (state) {
    case EngineState::Idle: return "Idle";
    case EngineState::Starting: return "Starting";
    case EngineState::Running: return "Running";
    case EngineState::MicMissing: return "MicMissing";
    case EngineState::DriverMissing: return "DriverMissing";
    case EngineState::UnsupportedSampleRate: return "UnsupportedSampleRate";
    case EngineState::Error: return "Error";
    }
    return "?";
}

EngineController::EngineController(EngineBackend& backend, std::function<double()> nowSeconds)
    : backend_(backend), now_(std::move(nowSeconds)) {}

EngineController::~EngineController() { close(); }

// Bypass keeps the tap: toggling it must be instant (applyFlags), not a rebuild.
bool EngineController::wantsTap(const Config& c) { return c.aecEnabled; }

void EngineController::setConfig(const Config& config) {
    const bool reshape = config.inputDeviceUid != config_.inputDeviceUid
                         || config.inputChannel != config_.inputChannel
                         || config.bufferSize != config_.bufferSize || wantsTap(config) != wantsTap(config_);
    const bool flagsChanged =
        config.aecEnabled != config_.aecEnabled || config.globalBypass != config_.globalBypass;
    config_ = config;
    if (reshape)
        close();   // reopened by evaluate() if there is demand
    else if (flagsChanged && ioOpen_)
        backend_.applyFlags(config_.aecEnabled, config_.globalBypass);
    evaluate();
}

void EngineController::onDemandChanged(bool inUse) {
    if (inUse == demand_) return;
    demand_ = inUse;
    if (!inUse) demandOffSince_ = now_();
    evaluate();
}

void EngineController::onMicListChanged() { evaluate(); }

void EngineController::onDefaultOutputChanged() {
    if (!ioOpen_ || !ioWithTap_) return;
    // The tap follows the default output device only when created: rebuild the IO in place. The
    // state stays Running unless the reopen fails.
    close();
    open();
}

void EngineController::onDeviceLost() {
    const bool wasOpen = ioOpen_;
    const double now = now_();
    close();
    // Lost again right after opening: do not rebuild in a tight loop, let tick() retry later.
    if (wasOpen && demand_ && now - lastOpened_ < kLostAfterOpenSeconds && blockedState() == EngineState::Idle) {
        lastError_ = "the audio device was lost again right after starting; retrying";
        lastAttempt_ = now;
        setState(EngineState::Error);
        return;
    }
    evaluate();
}

void EngineController::onSystemWake() {
    if (!ioOpen_) return;
    close();
    evaluate();
}

void EngineController::onServiceRestarted() {
    close();      // the IO's aggregate, tap and IOProc are gone with the old coreaudiod
    evaluate();   // the new one may not have published the devices yet: blocked until it has
}

void EngineController::tick() {
    const double now = now_();
    if (ioOpen_ && !demand_ && now - demandOffSince_ >= kStopGraceSeconds) {
        close();
        evaluate();
    } else if (state_ == EngineState::Error && (!demand_ || now - lastAttempt_ >= kRetrySeconds)) {
        evaluate();   // without demand: Idle
    }
}

EngineState EngineController::state() const { return state_; }
juce::String EngineController::lastError() const { return lastError_; }
bool EngineController::ioOpen() const { return ioOpen_; }

EngineState EngineController::blockedState() {
    if (!backend_.driverInstalled()) return EngineState::DriverMissing;
    if (config_.inputDeviceUid.isEmpty() || !backend_.micAvailable(config_.inputDeviceUid))
        return EngineState::MicMissing;
    if (!backend_.micSupports48k(config_.inputDeviceUid)) return EngineState::UnsupportedSampleRate;
    return EngineState::Idle;
}

void EngineController::evaluate() {
    const EngineState blocked = blockedState();
    if (blocked != EngineState::Idle) {
        close();
        lastError_.clear();
        setState(blocked);
        return;
    }
    if (ioOpen_) {   // in demand, or within the grace period
        setState(EngineState::Running);
        return;
    }
    if (!demand_) {
        if (state_ != EngineState::Error) lastError_.clear();   // keep why the last open failed
        setState(EngineState::Idle);
        return;
    }
    // A retry from Error goes straight to Running or stays in Error (no Starting flicker).
    if (state_ != EngineState::Error) setState(EngineState::Starting);
    open();
}

void EngineController::open() {
    const bool withTap = wantsTap(config_);
    lastAttempt_ = now_();
    juce::String error;
    if (backend_.openIO(withTap, error)) {
        ioOpen_ = true;
        ioWithTap_ = withTap;
        lastOpened_ = lastAttempt_;
        lastError_.clear();
        setState(EngineState::Running);
    } else {
        lastError_ = error.isNotEmpty() ? error : juce::String("could not open the audio IO");
        // Nobody needs the IO (e.g. a rebuild during the grace period): nothing to retry.
        setState(demand_ ? EngineState::Error : EngineState::Idle);
    }
}

void EngineController::close() {
    if (!ioOpen_) return;
    backend_.closeIO();
    ioOpen_ = false;
    ioWithTap_ = false;
}

void EngineController::setState(EngineState next) {
    if (next == state_) return;
    state_ = next;
    if (onStateChanged) onStateChanged(next);
}

} // namespace fixomic
