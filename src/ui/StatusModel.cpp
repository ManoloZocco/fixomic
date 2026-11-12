#include "ui/StatusModel.h"

#include <algorithm>
#include <cmath>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

StatusPresentation make(StatusKind kind, const juce::String& text, EngineState state) {
    return {kind, text, state};
}

juce::String italianDecimal(double value) {
    return juce::String(value, 1).replaceCharacter('.', ',');
}

EngineSnapshot starting() {
    EngineSnapshot snapshot;
    snapshot.state = EngineState::Starting;
    return snapshot;
}

double framesToMs(int frames, double sampleRate) {
    return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate * 1000.0 : 0.0;
}

} // namespace

StatusPresentation presentStatus(const EngineSnapshot& engine) {
    const auto state = engine.state;
    switch (state) {
    case EngineState::MicMissing:
        return make(StatusKind::Problem,
                    engine.micSelected ? utf8("Microfono non collegato \u2014 fixomic Mic \u00e8 in silenzio")
                                       : utf8("Nessun microfono scelto \u2014 fixomic Mic \u00e8 in silenzio"),
                    state);
    case EngineState::DriverMissing:
        return make(StatusKind::Problem, "Driver non installato", state);
    case EngineState::UnsupportedSampleRate:
        return make(StatusKind::Problem, "Questo microfono non supporta 48 kHz", state);
    case EngineState::Error:
        switch (engine.pendingPermission) {
        case PendingPermission::Microphone:
            return make(StatusKind::Waiting, "In attesa del permesso per il microfono", state);
        case PendingPermission::SystemAudio:
            return make(StatusKind::Waiting, "In attesa del permesso per l'audio di sistema", state);
        case PendingPermission::None:
            break;
        }
        return make(StatusKind::Problem,
                    engine.lastError.isEmpty() ? juce::String("Errore") : utf8("Errore \u2014 ") + engine.lastError,
                    state);
    case EngineState::Idle:
    case EngineState::Starting:
    case EngineState::Running:
        break;
    }

    if (engine.globalBypass) return make(StatusKind::Bypass, utf8("Bypass \u2014 audio non elaborato"), state);
    switch (state) {
    case EngineState::Running: return make(StatusKind::Active, "Attivo", state);
    case EngineState::Starting: return make(StatusKind::Waiting, utf8("Avvio\u2026"), state);
    case EngineState::Idle:
    case EngineState::MicMissing:
    case EngineState::DriverMissing:
    case EngineState::UnsupportedSampleRate:
    case EngineState::Error:
        break;
    }
    return make(StatusKind::Waiting, utf8("In attesa \u2014 nessuna app usa fixomic Mic"), state);
}

// ---- StatusModel ------------------------------------------------------------------------------

StatusModel::StatusModel(double driverMissingHoldSeconds)
    : hold_(driverMissingHoldSeconds), shown_(presentStatus(starting())) {}

const StatusPresentation& StatusModel::update(const EngineSnapshot& engine, double nowSeconds) {
    if (engine.state != EngineState::DriverMissing) {
        driverMissingSince_.reset();
        shown_ = presentStatus(engine);
        return shown_;
    }
    if (!driverMissingSince_) driverMissingSince_ = nowSeconds;
    if (nowSeconds - *driverMissingSince_ >= hold_) shown_ = presentStatus(engine);
    return shown_;
}

const StatusPresentation& StatusModel::current() const { return shown_; }

std::optional<double> StatusModel::recheckAt() const {
    if (!driverMissingSince_ || shown_.state == EngineState::DriverMissing) return std::nullopt;
    return *driverMissingSince_ + hold_;
}

// ---- route strip ------------------------------------------------------------------------------

RouteStrip routeFor(const StatusPresentation& status, const RouteInputs& inputs) {
    RouteStrip route;
    route.line = status.kind;
    switch (status.kind) {
    case StatusKind::Problem:
        if (status.state == EngineState::MicMissing || status.state == EngineState::UnsupportedSampleRate)
            route.microphone = StationState::Closed;
        else if (status.state == EngineState::DriverMissing)
            route.output = StationState::Closed;
        break;
    case StatusKind::Active:
        route.microphone = route.output = StationState::InService;
        if (inputs.aecEnabled && !inputs.aecWarning) route.echo = StationState::InService;
        if (inputs.chainProcessing) route.chain = StationState::InService;
        break;
    case StatusKind::Bypass:
        if (status.state == EngineState::Running) route.microphone = route.output = StationState::InService;
        break;
    case StatusKind::Waiting:
        break;
    }
    return route;
}

// ---- notices ----------------------------------------------------------------------------------

std::vector<Notice> collectNotices(const juce::String& lastWarning, std::optional<mac::PrivacyPane> warningPane,
                                   const std::vector<juce::String>& startupWarnings) {
    std::vector<Notice> notices;
    if (lastWarning.isNotEmpty()) notices.push_back({lastWarning, warningPane});
    for (const auto& warning : startupWarnings)
        if (warning.isNotEmpty()) notices.push_back({warning, std::nullopt});
    return notices;
}

// ---- chain ------------------------------------------------------------------------------------

juce::String chainSummary(const std::vector<ChainEntry>& chain) {
    juce::StringArray parts;
    for (const auto& entry : chain)
        parts.add(entry.name + (entry.missing ? " (non trovato)" : entry.bypassed ? " (in bypass)" : ""));
    return parts.joinIntoString(utf8(" \u2192 "));
}

// ---- formatting -------------------------------------------------------------------------------

juce::String formatMs(double milliseconds) { return italianDecimal(milliseconds) + " ms"; }

juce::String formatLatencyTotal(const LatencyBreakdown& latency) { return formatMs(latency.totalMs()); }

juce::String formatLatencyDetail(const LatencyBreakdown& latency) {
    const double rate = latency.sampleRate;
    const auto part = [rate](int frames) { return italianDecimal(framesToMs(frames, rate)); };
    const juce::String dot = utf8(" \u00b7 ");
    return "Buffer " + part(latency.bufferFrames) + dot + "eco " + part(latency.aecFrames) + dot + "plugin "
           + part(latency.pluginFrames) + dot + "dispositivi "
           + part(latency.inputDeviceFrames + latency.outputDeviceFrames) + " ms";
}

juce::String formatErle(float erleDb) {
    if (!std::isfinite(erleDb)) return utf8("\u2014");
    return juce::String(static_cast<int>(std::lround(std::max(0.0f, erleDb)))) + " dB";
}

float meterFraction(float peakDb) {
    if (!(peakDb > kMeterFloorDb)) return 0.0f;   // also NAN
    return std::min(1.0f, (peakDb - kMeterFloorDb) / -kMeterFloorDb);
}

} // namespace fixomic::ui
