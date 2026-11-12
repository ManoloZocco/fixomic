#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/StatusModel.h"

#include <cmath>
#include <limits>

using fixomic::EngineState;
using fixomic::LatencyBreakdown;
using fixomic::PendingPermission;
using fixomic::mac::PrivacyPane;
using namespace fixomic::ui;

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

EngineSnapshot engine(EngineState state) {
    EngineSnapshot s;
    s.state = state;
    s.micSelected = true;
    return s;
}

} // namespace

// ---- presentStatus ----------------------------------------------------------------------------

TEST_CASE("presentStatus uses the decided status texts", "[ui]") {
    REQUIRE(presentStatus(engine(EngineState::Idle)).text == utf8("In attesa \u2014 nessuna app usa fixomic Mic"));
    REQUIRE(presentStatus(engine(EngineState::Running)).text == "Attivo");
    REQUIRE(presentStatus(engine(EngineState::MicMissing)).text
            == utf8("Microfono non collegato \u2014 fixomic Mic \u00e8 in silenzio"));
    REQUIRE(presentStatus(engine(EngineState::DriverMissing)).text == "Driver non installato");
    REQUIRE(presentStatus(engine(EngineState::UnsupportedSampleRate)).text == "Questo microfono non supporta 48 kHz");

    auto bypassed = engine(EngineState::Running);
    bypassed.globalBypass = true;
    REQUIRE(presentStatus(bypassed).text == utf8("Bypass \u2014 audio non elaborato"));
}

TEST_CASE("presentStatus covers the states the plan leaves open", "[ui]") {
    REQUIRE(presentStatus(engine(EngineState::Starting)).text == utf8("Avvio\u2026"));

    auto failed = engine(EngineState::Error);
    failed.lastError = "aggregate device not found";
    REQUIRE(presentStatus(failed).text == utf8("Errore \u2014 aggregate device not found"));
    failed.lastError = {};
    REQUIRE(presentStatus(failed).text == "Errore");

    auto noMic = engine(EngineState::MicMissing);
    noMic.micSelected = false;
    REQUIRE(presentStatus(noMic).text == utf8("Nessun microfono scelto \u2014 fixomic Mic \u00e8 in silenzio"));
}

TEST_CASE("presentStatus maps every state to one of the four icons", "[ui]") {
    REQUIRE(presentStatus(engine(EngineState::Running)).kind == StatusKind::Active);
    REQUIRE(presentStatus(engine(EngineState::Idle)).kind == StatusKind::Waiting);
    REQUIRE(presentStatus(engine(EngineState::Starting)).kind == StatusKind::Waiting);
    for (auto state : {EngineState::MicMissing, EngineState::DriverMissing, EngineState::UnsupportedSampleRate,
                       EngineState::Error})
        REQUIRE(presentStatus(engine(state)).kind == StatusKind::Problem);
    REQUIRE(presentStatus(engine(EngineState::Running)).state == EngineState::Running);
}

TEST_CASE("a pending permission prompt is a waiting state, not an error", "[ui]") {
    auto waiting = engine(EngineState::Error);
    waiting.lastError = "whatever the backend wrote";

    waiting.pendingPermission = PendingPermission::Microphone;
    auto shown = presentStatus(waiting);
    REQUIRE(shown.kind == StatusKind::Waiting);
    REQUIRE(shown.text == "In attesa del permesso per il microfono");

    waiting.pendingPermission = PendingPermission::SystemAudio;
    shown = presentStatus(waiting);
    REQUIRE(shown.kind == StatusKind::Waiting);
    REQUIRE(shown.text == "In attesa del permesso per l'audio di sistema");

    // Only Error carries the meaning: a stale value never hides another state.
    auto idle = engine(EngineState::Idle);
    idle.pendingPermission = PendingPermission::Microphone;
    REQUIRE(presentStatus(idle).text == utf8("In attesa \u2014 nessuna app usa fixomic Mic"));
}

TEST_CASE("global bypass shows unless a problem or a permission prompt matters more", "[ui]") {
    auto s = engine(EngineState::Running);
    s.globalBypass = true;
    REQUIRE(presentStatus(s).kind == StatusKind::Bypass);

    s.state = EngineState::Idle;   // the next call goes out raw too
    REQUIRE(presentStatus(s).kind == StatusKind::Bypass);
    REQUIRE(presentStatus(s).text == utf8("Bypass \u2014 audio non elaborato"));

    s.state = EngineState::MicMissing;
    REQUIRE(presentStatus(s).kind == StatusKind::Problem);

    s.state = EngineState::Error;
    s.pendingPermission = PendingPermission::SystemAudio;
    REQUIRE(presentStatus(s).kind == StatusKind::Waiting);
}

// ---- StatusModel: DriverMissing hold ----------------------------------------------------------

TEST_CASE("StatusModel shows DriverMissing only after it lasted the hold time", "[ui]") {
    StatusModel model;   // 1.5 s
    REQUIRE(StatusModel::kDriverMissingHoldSeconds == 1.5);
    REQUIRE(model.update(engine(EngineState::Running), 10.0).text == "Attivo");

    SECTION("a Core Audio restart: DriverMissing for 0.34 s never shows") {
        REQUIRE(model.update(engine(EngineState::DriverMissing), 20.0).text == "Attivo");
        REQUIRE(model.current().kind == StatusKind::Active);
        REQUIRE(model.recheckAt() == 21.5);
        REQUIRE(model.update(engine(EngineState::Starting), 20.34).text == utf8("Avvio\u2026"));
        REQUIRE_FALSE(model.recheckAt().has_value());
        REQUIRE(model.update(engine(EngineState::Running), 20.5).text == "Attivo");
        REQUIRE(model.update(engine(EngineState::Running), 22.0).text == "Attivo");
    }
    SECTION("a driver that is really gone shows once the hold elapsed") {
        model.update(engine(EngineState::DriverMissing), 20.0);
        REQUIRE(model.update(engine(EngineState::DriverMissing), 21.49).kind == StatusKind::Active);
        const auto& shown = model.update(engine(EngineState::DriverMissing), 21.5);
        REQUIRE(shown.text == "Driver non installato");
        REQUIRE(shown.kind == StatusKind::Problem);
        REQUIRE(shown.state == EngineState::DriverMissing);
        REQUIRE_FALSE(model.recheckAt().has_value());
        REQUIRE(model.update(engine(EngineState::DriverMissing), 30.0).kind == StatusKind::Problem);
        // Back: shown at once.
        REQUIRE(model.update(engine(EngineState::Idle), 31.0).kind == StatusKind::Waiting);
    }
    SECTION("an interrupted DriverMissing restarts the hold") {
        model.update(engine(EngineState::DriverMissing), 20.0);
        model.update(engine(EngineState::Running), 21.0);
        model.update(engine(EngineState::DriverMissing), 21.2);
        REQUIRE(model.recheckAt() == Catch::Approx(22.7));
        REQUIRE(model.update(engine(EngineState::DriverMissing), 22.0).kind == StatusKind::Active);
        REQUIRE(model.update(engine(EngineState::DriverMissing), 22.7).kind == StatusKind::Problem);
    }
}

TEST_CASE("StatusModel starts on the Starting presentation and holds DriverMissing at launch too", "[ui]") {
    StatusModel model;
    REQUIRE(model.current().text == utf8("Avvio\u2026"));
    REQUIRE(model.current().kind == StatusKind::Waiting);
    REQUIRE(model.update(engine(EngineState::DriverMissing), 0.0).text == utf8("Avvio\u2026"));
    REQUIRE(model.update(engine(EngineState::DriverMissing), 1.5).text == "Driver non installato");
}

TEST_CASE("StatusModel hold time is injectable", "[ui]") {
    StatusModel immediate(0.0);
    REQUIRE(immediate.update(engine(EngineState::DriverMissing), 5.0).kind == StatusKind::Problem);

    StatusModel slow(3.0);
    slow.update(engine(EngineState::DriverMissing), 5.0);
    REQUIRE(slow.recheckAt() == 8.0);
    REQUIRE(slow.update(engine(EngineState::DriverMissing), 7.9).kind == StatusKind::Waiting);
    REQUIRE(slow.update(engine(EngineState::DriverMissing), 8.0).kind == StatusKind::Problem);
}

TEST_CASE("StatusModel passes other states through at once", "[ui]") {
    StatusModel model;
    auto bypassed = engine(EngineState::Running);
    bypassed.globalBypass = true;
    REQUIRE(model.update(bypassed, 1.0).kind == StatusKind::Bypass);
    REQUIRE(model.update(engine(EngineState::MicMissing), 1.1).kind == StatusKind::Problem);
    REQUIRE_FALSE(model.recheckAt().has_value());
}

// ---- route strip ------------------------------------------------------------------------------

TEST_CASE("routeFor marks the stations the voice passes through", "[ui]") {
    const RouteInputs all{true, false, true};

    SECTION("running: every stage in service, in the active ink") {
        const auto route = routeFor(presentStatus(engine(EngineState::Running)), all);
        REQUIRE(route == RouteStrip{StatusKind::Active, StationState::InService, StationState::InService,
                                    StationState::InService, StationState::InService});
    }
    SECTION("running without AEC, without a working reference, or with an empty chain") {
        const auto running = presentStatus(engine(EngineState::Running));
        REQUIRE(routeFor(running, {false, false, true}).echo == StationState::Skipped);
        REQUIRE(routeFor(running, {true, true, true}).echo == StationState::Skipped);
        REQUIRE(routeFor(running, {true, false, false}).chain == StationState::Skipped);
        REQUIRE(routeFor(running, {true, false, false}).output == StationState::InService);
    }
    SECTION("bypass while running: AEC and chain are skipped, the voice still goes out") {
        auto s = engine(EngineState::Running);
        s.globalBypass = true;
        const auto route = routeFor(presentStatus(s), all);
        REQUIRE(route == RouteStrip{StatusKind::Bypass, StationState::InService, StationState::Skipped,
                                    StationState::Skipped, StationState::InService});
    }
    SECTION("idle, starting or waiting for a permission: nothing in service") {
        const auto route = routeFor(presentStatus(engine(EngineState::Idle)), all);
        REQUIRE(route == RouteStrip{StatusKind::Waiting, StationState::Skipped, StationState::Skipped,
                                    StationState::Skipped, StationState::Skipped});
    }
    SECTION("the problem station is closed") {
        REQUIRE(routeFor(presentStatus(engine(EngineState::MicMissing)), all).microphone == StationState::Closed);
        REQUIRE(routeFor(presentStatus(engine(EngineState::UnsupportedSampleRate)), all).microphone
                == StationState::Closed);
        const auto noDriver = routeFor(presentStatus(engine(EngineState::DriverMissing)), all);
        REQUIRE(noDriver.output == StationState::Closed);
        REQUIRE(noDriver.microphone == StationState::Skipped);
        REQUIRE(noDriver.line == StatusKind::Problem);
        const auto error = routeFor(presentStatus(engine(EngineState::Error)), all);
        REQUIRE(error == RouteStrip{StatusKind::Problem, StationState::Skipped, StationState::Skipped,
                                    StationState::Skipped, StationState::Skipped});
    }
}

// ---- notices ----------------------------------------------------------------------------------

TEST_CASE("collectNotices lists the warning with its pane, then the startup warnings", "[ui]") {
    REQUIRE(collectNotices({}, std::nullopt, {}).empty());

    const auto notices = collectNotices("tap silent", PrivacyPane::SystemAudio, {"Nectar missing", "Gate missing"});
    REQUIRE(notices.size() == 3);
    REQUIRE(notices[0] == Notice{"tap silent", PrivacyPane::SystemAudio});
    REQUIRE(notices[1] == Notice{"Nectar missing", std::nullopt});
    REQUIRE(notices[2] == Notice{"Gate missing", std::nullopt});

    REQUIRE(collectNotices({}, PrivacyPane::SystemAudio, {}).empty());   // no text, no row
}

// ---- formatting -------------------------------------------------------------------------------

TEST_CASE("latency is formatted in milliseconds with a decimal comma", "[ui]") {
    REQUIRE(formatMs(12.66) == "12,7 ms");
    REQUIRE(formatMs(0.0) == "0,0 ms");
    REQUIRE(formatMs(5.333) == "5,3 ms");

    const LatencyBreakdown b{48000.0, 24, 32, 256, 912, 0};
    REQUIRE(formatLatencyTotal(b) == formatMs(b.totalMs()));
    REQUIRE(formatLatencyTotal(b) == "25,5 ms");
    REQUIRE(formatLatencyDetail(b) == utf8("Buffer 5,3 \u00b7 eco 19,0 \u00b7 plugin 0,0 \u00b7 dispositivi 1,2 ms"));

    REQUIRE(formatLatencyTotal(LatencyBreakdown{0.0, 0, 0, 0, 0, 0}) == "0,0 ms");
}

TEST_CASE("formatErle shows a neutral placeholder until the AEC measured something", "[ui]") {
    REQUIRE(formatErle(std::numeric_limits<float>::quiet_NaN()) == utf8("\u2014"));
    REQUIRE(formatErle(std::numeric_limits<float>::infinity()) == utf8("\u2014"));
    REQUIRE(formatErle(23.6f) == "24 dB");
    REQUIRE(formatErle(0.2f) == "0 dB");
    REQUIRE(formatErle(-3.0f) == "0 dB");   // the output louder than the mic is "nothing removed"
}

TEST_CASE("meterFraction maps -60..0 dBFS to the bar", "[ui]") {
    REQUIRE(kMeterFloorDb == -60.0f);
    REQUIRE(meterFraction(-60.0f) == 0.0f);
    REQUIRE(meterFraction(-100.0f) == 0.0f);
    REQUIRE(meterFraction(0.0f) == 1.0f);
    REQUIRE(meterFraction(6.0f) == 1.0f);
    REQUIRE(meterFraction(-30.0f) == Catch::Approx(0.5f));
    REQUIRE(meterFraction(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("chainSummary lists the plugins in order until the chain editor exists", "[ui]") {
    REQUIRE(chainSummary({}).isEmpty());
    REQUIRE(chainSummary({{"Nectar 4 Gate", false, false}}) == "Nectar 4 Gate");
    REQUIRE(chainSummary({{"Gate", false, false}, {"Nectar 4", true, false}, {"Old EQ", false, true}})
            == utf8("Gate \u2192 Nectar 4 (in bypass) \u2192 Old EQ (non trovato)"));
}
