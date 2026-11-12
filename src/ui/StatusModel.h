#pragma once

#include "core/LatencyBudget.h"
#include "engine/EngineController.h"
#include "platform/mac/Permissions.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace fixomic::ui {

// The UI's pure logic: engine state -> status sentence, icon and route strip, plus the number
// formatting of the panel. No JUCE GUI, no AppController: everything comes in as plain values.

// The four looks of the menu-bar icon, also the ink family of the panel's state line.
enum class StatusKind { Active, Waiting, Bypass, Problem };

// The engine as the UI sees it at one moment (filled from AppController).
struct EngineSnapshot {
    EngineState state = EngineState::Idle;
    PendingPermission pendingPermission = PendingPermission::None;   // meaningful in Error
    juce::String lastError;
    bool globalBypass = false;
    bool micSelected = false;   // config().inputDeviceUid is not empty
};

struct StatusPresentation {
    StatusKind kind = StatusKind::Waiting;
    juce::String text;
    EngineState state = EngineState::Starting;   // the state this presents
    bool operator==(const StatusPresentation&) const = default;
};

// The state -> presentation mapping (no DriverMissing hold). Precedence: a problem state
// (MicMissing, DriverMissing, UnsupportedSampleRate, Error not caused by a pending permission
// prompt), then a pending permission prompt (waiting), then global bypass, then Running (active)
// or Idle / Starting (waiting).
StatusPresentation presentStatus(const EngineSnapshot&);

// presentStatus() with the DriverMissing hold: during a Core Audio restart the engine reports
// DriverMissing for 15 ms - 0.34 s before reopening, so DriverMissing is shown only once it has
// lasted `driverMissingHoldSeconds`; until then the previous presentation stays. The first
// presentation, before any update, is the one of Starting ("Avvio…").
class StatusModel {
public:
    static constexpr double kDriverMissingHoldSeconds = 1.5;

    explicit StatusModel(double driverMissingHoldSeconds = kDriverMissingHoldSeconds);

    // The engine as it is at `nowSeconds` (any monotonic clock); returns what to show.
    const StatusPresentation& update(const EngineSnapshot&, double nowSeconds);
    const StatusPresentation& current() const;
    // While DriverMissing is held back: the time (same clock) at which update() must be called
    // again, even if nothing else changes, for it to appear. nullopt otherwise.
    std::optional<double> recheckAt() const;

private:
    double hold_;
    StatusPresentation shown_;
    std::optional<double> driverMissingSince_;
};

// ---- route strip: Microfono -> Eco -> Catena -> fixomic Mic -----------------------------------

enum class StationState {
    InService,   // the voice passes through and this stage processes it (filled marker)
    Skipped,     // not processing now: idle, off, bypassed, nothing loaded (hollow marker)
    Closed,      // the problem is here: no mic / no 48 kHz, no driver (problem ring)
};

struct RouteStrip {
    StatusKind line = StatusKind::Waiting;
    StationState microphone = StationState::Skipped;
    StationState echo = StationState::Skipped;
    StationState chain = StationState::Skipped;
    StationState output = StationState::Skipped;
    bool operator==(const RouteStrip&) const = default;
};

struct RouteInputs {
    bool aecEnabled = true;
    bool aecWarning = false;      // a warning says the AEC has no (working) reference
    bool chainProcessing = false; // at least one loaded, not bypassed plugin
};

RouteStrip routeFor(const StatusPresentation&, const RouteInputs&);

// ---- notices -----------------------------------------------------------------------------------

struct Notice {
    juce::String text;
    std::optional<mac::PrivacyPane> settingsPane;   // the System Settings pane that fixes it
    bool operator==(const Notice&) const = default;
};

// The backend warning first (with its pane), then the plugins that could not be restored.
std::vector<Notice> collectNotices(const juce::String& lastWarning, std::optional<mac::PrivacyPane> warningPane,
                                   const std::vector<juce::String>& startupWarnings);

// ---- chain ----------------------------------------------------------------------------------------

struct ChainEntry {
    juce::String name;
    bool bypassed = false;
    bool missing = false;   // could not be restored
};

// The chain section's read-only line until the chain editor (Task 10b) takes its place: the plugins
// in signal order joined by " → ", each one not processing marked "(in bypass)" or "(non trovato)".
// "" for an empty chain (the panel then shows its empty state).
juce::String chainSummary(const std::vector<ChainEntry>&);

// ---- formatting (Italian: decimal comma) --------------------------------------------------------

juce::String formatMs(double milliseconds);                // "12,7 ms"
juce::String formatLatencyTotal(const LatencyBreakdown&);  // formatMs(totalMs())
// "Buffer 5,3 · eco 19,0 · plugin 0,0 · dispositivi 1,2 ms"
juce::String formatLatencyDetail(const LatencyBreakdown&);
juce::String formatErle(float erleDb);                      // "24 dB"; "—" while NAN (not measured)

// Meter scale: kMeterFloorDb..0 dBFS -> 0..1, clamped; NAN -> 0.
inline constexpr float kMeterFloorDb = -60.0f;
float meterFraction(float peakDb);

} // namespace fixomic::ui
