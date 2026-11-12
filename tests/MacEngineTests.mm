#include <catch2/catch_test_macros.hpp>

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "engine/EngineController.h"
#include "platform/mac/AggregateIO.h"
#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/CoreAudioUtil.h"
#include "platform/mac/DemandMonitor.h"
#include "platform/mac/FixomicDevices.h"
#include "platform/mac/MacEngineBackend.h"
#include "platform/mac/WakeObserver.h"
#include "support/TestPlugin.h"

#import <AppKit/AppKit.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace mac = fixomic::mac;

namespace {

template <typename Predicate>
void pumpMainLoop(double seconds, Predicate done)
{
    CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3600, 3600, 0, 0, ^(CFRunLoopTimerRef) {
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end && !done())
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CFRunLoopRemoveTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);
    CFRelease(keepAlive);
}

juce::String testMicUid()
{
    if (const char* env = std::getenv("FIXOMIC_TEST_MIC_UID"))
        return env;
    return "BuiltInMicrophoneDevice";
}

int privateAggregateCount()
{
    int count = 0;
    for (AudioObjectID device : mac::util::getArray<AudioObjectID>(kAudioObjectSystemObject,
                                                                    kAudioHardwarePropertyDevices))
        if (mac::util::getString(device, kAudioDevicePropertyDeviceUID).startsWith("fixomic_aggregate_"))
            ++count;
    return count;
}

// The process's device list catches up with a created / destroyed aggregate asynchronously.
int settledAggregateCount(int expected)
{
    pumpMainLoop(2.0, [&] { return privateAggregateCount() == expected; });
    return privateAggregateCount();
}

bool fixomicMicRunningSomewhere()
{
    const auto mic = mac::findDeviceByUid(fixomic::devices::kMicUid);
    return mic
           && mac::util::getScalar<UInt32>(*mic, kAudioDevicePropertyDeviceIsRunningSomewhere).value_or(0) != 0;
}

// The AEC stage, chain and graph a backend drives.
struct Engine {
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    mac::MacEngineBackend backend{graph, aec, chain, [this] { return config; }};

    Engine()
    {
        config.inputDeviceUid = testMicUid();
        config.bufferSize = 128;
    }
};

} // namespace

TEST_CASE("WakeObserver calls back on NSWorkspaceDidWakeNotification", "[wake]")
{
    int wakes = 0;
    {
        mac::WakeObserver observer([&] { ++wakes; });
        [NSWorkspace.sharedWorkspace.notificationCenter postNotificationName:NSWorkspaceDidWakeNotification
                                                                      object:nil];
        pumpMainLoop(1.0, [&] { return wakes > 0; });
        REQUIRE(wakes == 1);
    }
    [NSWorkspace.sharedWorkspace.notificationCenter postNotificationName:NSWorkspaceDidWakeNotification object:nil];
    pumpMainLoop(0.2, [] { return false; });
    REQUIRE(wakes == 1);   // destroyed: no more callbacks
}

TEST_CASE("DemandMonitor reports a reader of fixomic Mic within one poll", "[demand][hardware]")
{
    const auto micId = mac::findDeviceByUid(fixomic::devices::kMicUid);
    REQUIRE(micId.has_value());
    if (fixomicMicRunningSomewhere())
        SKIP("fixomic Mic is already in use by another process");

    std::vector<bool> reports;
    std::vector<double> at;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    mac::DemandMonitor monitor([&](bool inUse) {
        reports.push_back(inUse);
        at.push_back(elapsed());
    });
    monitor.start();
    pumpMainLoop(0.5, [] { return false; });
    REQUIRE(reports.empty());   // nobody reads: nothing to report
    REQUIRE_FALSE(monitor.inUse());

    // This process becomes a reader of fixomic Mic.
    AudioDeviceIOProcID proc = nullptr;
    REQUIRE(AudioDeviceCreateIOProcIDWithBlock(&proc, *micId, nullptr,
                                               ^(const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                                                 AudioBufferList*, const AudioTimeStamp*) {
                                               })
            == noErr);
    REQUIRE(AudioDeviceStart(*micId, proc) == noErr);
    const double started = elapsed();
    pumpMainLoop(2.0, [&] { return !reports.empty(); });
    const double seen = reports.empty() ? -1.0 : at.back() - started;

    AudioDeviceStop(*micId, proc);
    const double stopped = elapsed();
    pumpMainLoop(2.0, [&] { return reports.size() >= 2; });
    const double gone = reports.size() >= 2 ? at.back() - stopped : -1.0;
    AudioDeviceDestroyIOProcID(*micId, proc);

    monitor.stop();
    pumpMainLoop(0.5, [] { return false; });   // no callback after stop()

    INFO("in use seen after " << seen << " s, not in use after " << gone << " s");
    REQUIRE(reports == std::vector<bool>{true, false});
    REQUIRE(seen >= 0.0);
    REQUIRE(seen < 0.45);   // one 200 ms poll plus HAL and run-loop slack
    REQUIRE(gone >= 0.0);
    REQUIRE(gone < 0.45);
}

TEST_CASE("MacEngineBackend device checks", "[backend][hardware]")
{
    Engine e;
    REQUIRE(e.backend.driverInstalled());
    REQUIRE(e.backend.micAvailable(testMicUid()));
    REQUIRE(e.backend.micSupports48k(testMicUid()));
    REQUIRE_FALSE(e.backend.micAvailable(fixomic::devices::kMicUid));     // never a fixomic device
    REQUIRE_FALSE(e.backend.micAvailable(fixomic::devices::kBridgeUid));
    REQUIRE_FALSE(e.backend.micAvailable("no-such-mic-uid"));
    REQUIRE_FALSE(e.backend.micAvailable(""));
    REQUIRE_FALSE(e.backend.micSupports48k("no-such-mic-uid"));
}

TEST_CASE("MacEngineBackend opens and closes the IO and keeps the plugins", "[backend][hardware]")
{
    Engine e;
    e.config.aecEnabled = false;
    e.chain.prepare(48000.0, 128);
    e.chain.add(std::make_unique<fixomic::test::TestPlugin>("keep me", 1.0f, 32));
    const auto* plugin = e.chain.pluginAt(0);
    const int aggregatesBefore = settledAggregateCount(0);

    REQUIRE_FALSE(e.backend.latency().has_value());
    juce::String error;
    REQUIRE(e.backend.openIO(false, error));
    INFO(error.toStdString());
    REQUIRE(e.backend.ioOpen());
    REQUIRE_FALSE(e.backend.tapActive());
    REQUIRE(settledAggregateCount(aggregatesBefore + 1) == aggregatesBefore + 1);

    pumpMainLoop(1.0, [&] { return e.backend.io()->stats().callbacks > 50; });
    REQUIRE(e.backend.io()->stats().callbacks > 50);
    const auto latency = e.backend.latency();
    REQUIRE(latency.has_value());
    REQUIRE(latency->sampleRate == 48000.0);
    REQUIRE(latency->bufferFrames == 256);
    REQUIRE(latency->aecFrames == 0);
    REQUIRE(latency->pluginFrames == 32);

    e.backend.closeIO();
    REQUIRE_FALSE(e.backend.ioOpen());
    REQUIRE_FALSE(e.backend.latency().has_value());
    REQUIRE(settledAggregateCount(aggregatesBefore) == aggregatesBefore);
    e.backend.closeIO();   // idempotent

    // Stopping never unloads plugins.
    REQUIRE(e.chain.slots().size() == 1);
    REQUIRE(e.chain.pluginAt(0) == plugin);
    REQUIRE(fixomic::test::TestPlugin::liveInstances.load() >= 1);
}

TEST_CASE("MacEngineBackend prepares the plugins again only when the buffer size changes", "[backend][hardware]")
{
    using fixomic::test::TestPlugin;
    Engine e;
    e.config.aecEnabled = false;
    e.chain.prepare(48000.0, 128);
    e.chain.add(std::make_unique<TestPlugin>("counted", 1.0f));
    juce::String error;

    const int before = TestPlugin::prepareCalls.load();
    REQUIRE(e.backend.openIO(false, error));   // first open: prepares
    const int afterFirst = TestPlugin::prepareCalls.load();
    REQUIRE(afterFirst > before);
    e.backend.closeIO();
    REQUIRE(e.backend.openIO(false, error));   // same buffer: no prepareToPlay
    e.backend.closeIO();
    REQUIRE(TestPlugin::prepareCalls.load() == afterFirst);

    e.config.bufferSize = 256;                 // new buffer: prepared again
    REQUIRE(e.backend.openIO(false, error));
    INFO(error.toStdString());
    REQUIRE(TestPlugin::prepareCalls.load() > afterFirst);
    pumpMainLoop(0.5, [&] { return e.backend.io()->stats().callbacks > 20; });
    REQUIRE(e.backend.io()->stats().callbacks > 20);
    e.backend.closeIO();
}

TEST_CASE("MacEngineBackend applyFlags switches bypass on the open IO", "[backend][hardware]")
{
    Engine e;
    juce::String error;
    REQUIRE(e.backend.openIO(true, error));
    INFO(error.toStdString());
    REQUIRE(e.backend.tapActive());
    pumpMainLoop(1.0, [&] { return e.graph.aecActive(); });
    REQUIRE(e.graph.aecActive());
    const auto* io = e.backend.io();

    e.backend.applyFlags(true, true);   // bypass: AEC skipped, same IO and tap
    pumpMainLoop(1.0, [&] { return !e.graph.aecActive(); });
    REQUIRE_FALSE(e.graph.aecActive());
    REQUIRE(e.backend.io() == io);
    REQUIRE(e.backend.tapActive());

    e.backend.applyFlags(true, false);
    pumpMainLoop(1.0, [&] { return e.graph.aecActive(); });
    REQUIRE(e.graph.aecActive());
    e.backend.closeIO();
}

TEST_CASE("MacEngineBackend with a tap runs the AEC", "[backend][hardware]")
{
    Engine e;
    std::vector<juce::String> warnings;
    e.backend.onWarning = [&](juce::String w) { warnings.push_back(w); };
    juce::String error;
    REQUIRE(e.backend.openIO(true, error));
    INFO(error.toStdString());
    if (!warnings.empty())
        FAIL("tap warning: " << warnings.front().toStdString());
    REQUIRE(e.backend.tapActive());
    pumpMainLoop(1.0, [&] { return e.backend.io()->stats().callbacks > 50; });
    REQUIRE(e.graph.aecActive());
    const auto latency = e.backend.latency();
    REQUIRE(latency.has_value());
    REQUIRE(latency->aecFrames == e.aec.latencySamples());
    REQUIRE(latency->aecFrames > 0);
    const int aggregatesOpen = settledAggregateCount(1);
    e.backend.closeIO();
    REQUIRE(settledAggregateCount(aggregatesOpen - 1) == aggregatesOpen - 1);
}

TEST_CASE("MacEngineBackend openIO failure leaves nothing open", "[backend][hardware]")
{
    Engine e;
    e.config.inputDeviceUid = "no-such-mic-uid";
    const int aggregatesBefore = settledAggregateCount(0);
    juce::String error;
    REQUIRE_FALSE(e.backend.openIO(true, error));
    REQUIRE(error.contains("not found"));
    REQUIRE_FALSE(e.backend.ioOpen());
    REQUIRE_FALSE(e.backend.tapActive());
    REQUIRE(settledAggregateCount(aggregatesBefore) == aggregatesBefore);
}

TEST_CASE("EngineController drives the Mac backend", "[backend][hardware]")
{
    Engine e;
    e.config.aecEnabled = false;
    double now = 0.0;
    fixomic::EngineController engine(e.backend, [&] { return now; });
    e.backend.onDeviceLost = [&] { engine.onDeviceLost(); };
    engine.setConfig(e.config);
    REQUIRE(engine.state() == fixomic::EngineState::Idle);

    engine.onDemandChanged(true);
    REQUIRE(engine.state() == fixomic::EngineState::Running);
    REQUIRE(e.backend.ioOpen());
    pumpMainLoop(0.5, [] { return false; });

    engine.onDemandChanged(false);
    now = fixomic::EngineController::kStopGraceSeconds;
    engine.tick();
    REQUIRE(engine.state() == fixomic::EngineState::Idle);
    REQUIRE_FALSE(e.backend.ioOpen());
}

TEST_CASE("MacEngineBackend asks for the microphone before touching the HAL", "[backend][permissions]")
{
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    config.inputDeviceUid = "no-such-mic-uid";   // would fail with "not found" if the HAL were reached

    mac::Permission status = mac::Permission::NotDetermined;
    int requests = 0;
    std::function<void(bool)> pendingCompletion;
    mac::MacEngineBackend::MicrophoneAccess access{
        [&] { return status; },
        [&](std::function<void(bool)> done) {
            ++requests;
            pendingCompletion = std::move(done);
        }};
    mac::MacEngineBackend backend{graph, aec, chain, [&] { return config; }, access};
    std::vector<bool> answers;
    backend.onMicrophonePermission = [&](bool granted) { answers.push_back(granted); };

    juce::String error;
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error == "In attesa del permesso per il microfono");
    REQUIRE(requests == 1);
    REQUIRE_FALSE(backend.openIO(false, error));   // retries do not ask again
    REQUIRE(requests == 1);
    REQUIRE_FALSE(backend.ioOpen());

    REQUIRE(pendingCompletion);
    pendingCompletion(true);
    REQUIRE(answers == std::vector<bool>{true});

    status = mac::Permission::Denied;
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error.startsWith("Permesso microfono negato"));
    REQUIRE(requests == 1);

    status = mac::Permission::Granted;   // now the HAL is reached
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error.contains("not found"));
}
