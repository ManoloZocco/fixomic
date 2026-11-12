#include <catch2/catch_test_macros.hpp>

#include "engine/EngineController.h"

#include <algorithm>
#include <vector>

using fixomic::Config;
using fixomic::EngineBackend;
using fixomic::EngineController;
using fixomic::EngineState;

namespace {

struct FakeBackend : EngineBackend {
    bool driver = true;
    bool mic = true;
    bool mic48k = true;
    bool openFails = false;
    juce::String micAsked;            // last uid passed to micAvailable / micSupports48k
    int opens = 0, closes = 0;
    std::vector<bool> openTaps;       // withTap of every openIO call
    bool open = false;

    bool driverInstalled() override { return driver; }
    bool micAvailable(const juce::String& uid) override { micAsked = uid; return mic; }
    bool micSupports48k(const juce::String& uid) override { micAsked = uid; return mic48k; }
    bool openIO(bool withTap, juce::String& error) override {
        ++opens;
        openTaps.push_back(withTap);
        if (openFails) {
            error = "aggregate exploded";
            return false;
        }
        open = true;
        return true;
    }
    void closeIO() override {
        ++closes;
        open = false;
    }
};

Config micConfig(bool aec = true, bool bypass = false) {
    Config c;
    c.inputDeviceUid = "HyperX";
    c.aecEnabled = aec;
    c.globalBypass = bypass;
    return c;
}

// Controller + fake backend + fake clock + a record of every state change.
struct Rig {
    FakeBackend backend;
    double now = 0.0;
    EngineController engine{backend, [this] { return now; }};
    std::vector<EngineState> changes;

    explicit Rig(const Config& config = micConfig()) {
        engine.onStateChanged = [this](EngineState s) { changes.push_back(s); };
        engine.setConfig(config);
    }

    // Advances the fake clock to `t` in 200 ms ticks.
    void runUntil(double t) {
        while (now + 0.2 <= t + 1e-9) {
            now += 0.2;
            engine.tick();
        }
        now = t;
        engine.tick();
    }

    bool sawState(EngineState s) const { return std::find(changes.begin(), changes.end(), s) != changes.end(); }
};

} // namespace

TEST_CASE("EngineController starts on demand and stops after the grace period", "[engine]") {
    Rig rig;
    REQUIRE(rig.engine.state() == EngineState::Idle);
    REQUIRE(rig.backend.opens == 0);

    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.backend.opens == 1);
    REQUIRE(rig.backend.openTaps == std::vector<bool>{true});
    REQUIRE(rig.changes == std::vector<EngineState>{EngineState::Starting, EngineState::Running});

    rig.runUntil(10.0);   // demand still on: nothing happens
    REQUIRE(rig.backend.closes == 0);

    rig.engine.onDemandChanged(false);   // at t = 10
    rig.runUntil(12.99);
    REQUIRE(rig.backend.closes == 0);
    REQUIRE(rig.engine.state() == EngineState::Running);

    rig.runUntil(13.0);
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.engine.state() == EngineState::Idle);
    REQUIRE_FALSE(rig.engine.ioOpen());

    rig.runUntil(20.0);
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.changes == std::vector<EngineState>{EngineState::Starting, EngineState::Running, EngineState::Idle});
}

TEST_CASE("EngineController grace period: demand back within 3 s keeps the IO", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);
    rig.engine.onDemandChanged(false);   // t = 0
    rig.runUntil(2.0);
    rig.engine.onDemandChanged(true);    // t = 2
    rig.runUntil(10.0);
    REQUIRE(rig.backend.closes == 0);
    REQUIRE(rig.backend.opens == 1);
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.changes == std::vector<EngineState>{EngineState::Starting, EngineState::Running});

    // A second off -> the grace period starts over from the new off.
    rig.engine.onDemandChanged(false);   // t = 10
    rig.runUntil(12.9);
    REQUIRE(rig.backend.closes == 0);
    rig.runUntil(13.1);
    REQUIRE(rig.backend.closes == 1);
}

TEST_CASE("EngineController mic missing goes silent and recovers", "[engine]") {
    Rig rig;
    rig.backend.mic = false;
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::MicMissing);
    REQUIRE(rig.backend.opens == 0);
    REQUIRE(rig.backend.micAsked == "HyperX");

    rig.runUntil(30.0);   // never retries on its own, never picks another mic
    REQUIRE(rig.backend.opens == 0);

    rig.backend.mic = true;
    rig.engine.onMicListChanged();
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.backend.opens == 1);
}

TEST_CASE("EngineController mic unplugged while running closes the IO", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::Running);

    rig.backend.mic = false;
    rig.engine.onMicListChanged();
    REQUIRE(rig.engine.state() == EngineState::MicMissing);
    REQUIRE(rig.backend.closes == 1);
    REQUIRE_FALSE(rig.backend.open);

    rig.engine.onDeviceLost();   // the aggregate's own notification arriving afterwards: harmless
    REQUIRE(rig.engine.state() == EngineState::MicMissing);
    REQUIRE(rig.backend.opens == 1);
}

TEST_CASE("EngineController without a selected mic is MicMissing", "[engine]") {
    Rig rig(Config{});
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::MicMissing);
    REQUIRE(rig.backend.opens == 0);
}

TEST_CASE("EngineController output change rebuilds the tap without stopping", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);
    rig.changes.clear();

    rig.engine.onDefaultOutputChanged();
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.backend.opens == 2);
    REQUIRE(rig.backend.openTaps.back() == true);
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.changes.empty());   // never through Idle (nor Starting)
    REQUIRE(rig.backend.open);
}

TEST_CASE("EngineController output change without a tap or without IO does nothing", "[engine]") {
    Rig idle;
    idle.engine.onDefaultOutputChanged();
    REQUIRE(idle.backend.opens == 0);
    REQUIRE(idle.backend.closes == 0);

    Rig noTap(micConfig(false));
    noTap.engine.onDemandChanged(true);
    noTap.engine.onDefaultOutputChanged();
    REQUIRE(noTap.backend.opens == 1);
    REQUIRE(noTap.backend.closes == 0);
}

TEST_CASE("EngineController with AEC off opens the IO without a tap", "[engine]") {
    Rig rig(micConfig(false));
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.backend.openTaps == std::vector<bool>{false});

    Rig bypassed(micConfig(true, true));
    bypassed.engine.onDemandChanged(true);
    REQUIRE(bypassed.backend.openTaps == std::vector<bool>{false});
}

TEST_CASE("EngineController driver missing", "[engine]") {
    Rig rig;
    rig.backend.driver = false;
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::DriverMissing);
    REQUIRE(rig.backend.opens == 0);

    // Reported without demand too: without the driver there is no fixomic Mic to demand.
    Rig noDemand;
    noDemand.backend.driver = false;
    noDemand.engine.onMicListChanged();
    REQUIRE(noDemand.engine.state() == EngineState::DriverMissing);

    rig.backend.driver = true;
    rig.engine.onMicListChanged();   // the driver's devices appear in the device list
    REQUIRE(rig.engine.state() == EngineState::Running);
}

TEST_CASE("EngineController unsupported sample rate", "[engine]") {
    Rig rig;
    rig.backend.mic48k = false;
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::UnsupportedSampleRate);
    REQUIRE(rig.backend.opens == 0);

    Config other = micConfig();
    other.inputDeviceUid = "USB48";
    rig.backend.mic48k = true;
    rig.engine.setConfig(other);   // the user picks another mic
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.backend.micAsked == "USB48");
}

TEST_CASE("EngineController wake rebuilds the IO when in demand", "[engine]") {
    Rig rig;
    rig.engine.onSystemWake();   // Idle: no calls at all
    REQUIRE(rig.backend.opens == 0);
    REQUIRE(rig.backend.closes == 0);
    REQUIRE(rig.engine.state() == EngineState::Idle);

    rig.engine.onDemandChanged(true);
    rig.engine.onSystemWake();
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.backend.opens == 2);
    REQUIRE(rig.backend.openTaps == std::vector<bool>{true, true});
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.backend.open);
}

TEST_CASE("EngineController onStateChanged fires only on actual changes", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);
    rig.engine.onDemandChanged(true);
    rig.engine.onMicListChanged();
    rig.engine.setConfig(micConfig());
    rig.engine.tick();
    REQUIRE(rig.changes == std::vector<EngineState>{EngineState::Starting, EngineState::Running});
    REQUIRE(rig.backend.opens == 1);

    Rig missing;
    missing.backend.mic = false;
    missing.engine.onMicListChanged();
    missing.engine.onMicListChanged();
    missing.engine.onDemandChanged(true);
    REQUIRE(missing.changes == std::vector<EngineState>{EngineState::MicMissing});
}

TEST_CASE("EngineController openIO failure is an Error retried while in demand", "[engine]") {
    Rig rig;
    rig.backend.openFails = true;
    rig.engine.onDemandChanged(true);
    REQUIRE(rig.engine.state() == EngineState::Error);
    REQUIRE(rig.engine.lastError().contains("aggregate exploded"));
    REQUIRE(rig.backend.opens == 1);
    REQUIRE_FALSE(rig.engine.ioOpen());

    rig.runUntil(EngineController::kRetrySeconds - 0.1);
    REQUIRE(rig.backend.opens == 1);
    rig.runUntil(EngineController::kRetrySeconds + 0.1);
    REQUIRE(rig.backend.opens == 2);

    rig.backend.openFails = false;
    rig.runUntil(2 * EngineController::kRetrySeconds + 0.3);
    REQUIRE(rig.engine.state() == EngineState::Running);
    REQUIRE(rig.engine.lastError().isEmpty());

    Rig gaveUp;
    gaveUp.backend.openFails = true;
    gaveUp.engine.onDemandChanged(true);
    gaveUp.engine.onDemandChanged(false);   // nothing to stop: Idle right away
    REQUIRE(gaveUp.engine.state() == EngineState::Idle);
    gaveUp.runUntil(20.0);
    REQUIRE(gaveUp.backend.opens == 1);
}

TEST_CASE("EngineController device lost reopens the IO while in demand", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);
    rig.engine.onDeviceLost();
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.backend.opens == 2);
    REQUIRE(rig.engine.state() == EngineState::Running);

    // During the grace period nobody needs the IO any more: it just stops.
    rig.engine.onDemandChanged(false);
    rig.engine.onDeviceLost();
    REQUIRE(rig.backend.closes == 2);
    REQUIRE(rig.backend.opens == 2);
    REQUIRE(rig.engine.state() == EngineState::Idle);
}

TEST_CASE("EngineController rebuilds the IO only for IO-shaping config changes", "[engine]") {
    Rig rig;
    rig.engine.onDemandChanged(true);

    Config c = micConfig();
    c.launchAtLogin = true;
    c.chain.push_back({});
    rig.engine.setConfig(c);
    REQUIRE(rig.backend.opens == 1);
    REQUIRE(rig.backend.closes == 0);

    c.bufferSize = 64;
    rig.engine.setConfig(c);
    REQUIRE(rig.backend.closes == 1);
    REQUIRE(rig.backend.opens == 2);
    REQUIRE(rig.engine.state() == EngineState::Running);

    c.aecEnabled = false;   // tap no longer wanted
    rig.engine.setConfig(c);
    REQUIRE(rig.backend.opens == 3);
    REQUIRE(rig.backend.openTaps.back() == false);

    c.inputChannel = 1;
    rig.engine.setConfig(c);
    REQUIRE(rig.backend.opens == 4);
}

TEST_CASE("EngineController destructor closes an open IO", "[engine]") {
    FakeBackend backend;
    {
        EngineController engine(backend, [] { return 0.0; });
        engine.setConfig(micConfig());
        engine.onDemandChanged(true);
        REQUIRE(backend.open);
    }
    REQUIRE_FALSE(backend.open);
    REQUIRE(backend.closes == 1);
}
