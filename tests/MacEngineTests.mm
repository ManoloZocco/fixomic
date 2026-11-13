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
#include <thread>
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
    // System audio reported as granted: these tests exercise the tap itself (from a terminal the
    // TCC preflight answers for the terminal, "not determined", and the tap works anyway).
    mac::MacEngineBackend backend{graph, aec, chain, [this] { return config; }, {},
                                  {[] { return mac::Permission::Granted; }, nullptr, nullptr}};

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

TEST_CASE("PropertyWatcher can be destroyed while Core Audio tears its object down", "[devices]")
{
    // A dying object's last notifications are delivered by the HAL with dispatch_sync on the
    // listener's queue while it holds the object's lock, and removing a listener waits for that
    // lock. With the main queue as listener queue, destroying a watcher on the main thread then
    // deadlocks (seen after `killall coreaudiod`: closing the IO froze the app).
    NSDictionary* description = @{
        @kAudioAggregateDeviceNameKey : @"fixomic test teardown",
        @kAudioAggregateDeviceUIDKey : [@"fixomic_test_teardown_" stringByAppendingString:NSUUID.UUID.UUIDString],
        @kAudioAggregateDeviceIsPrivateKey : @1,
    };
    AudioObjectID aggregate = kAudioObjectUnknown;
    REQUIRE(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &aggregate) == noErr);
    int calls = 0;
    auto watcher = std::make_unique<mac::PropertyWatcher>(
        aggregate,
        AudioObjectPropertyAddress{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain},
        [&] { ++calls; });
    REQUIRE(watcher->isListening());

    AudioHardwareDestroyAggregateDevice(aggregate);
    // The main queue is not serviced meanwhile: the teardown notification is in flight.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    watcher.reset();   // must return
    pumpMainLoop(0.3, [] { return false; });
    REQUIRE(calls == 0);   // destroyed before the main queue ran: never called
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
    e.backend.onWarning = [&](mac::WarningKind, juce::String w) { warnings.push_back(w); };
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
    REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::None);
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error == "In attesa del permesso per il microfono");
    REQUIRE(error == mac::MacEngineBackend::kWaitingForMicrophone);
    REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::Microphone);
    REQUIRE(requests == 1);
    REQUIRE_FALSE(backend.openIO(false, error));   // retries do not ask again
    REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::Microphone);
    REQUIRE(requests == 1);
    REQUIRE_FALSE(backend.ioOpen());

    REQUIRE(pendingCompletion);
    pendingCompletion(true);
    REQUIRE(answers == std::vector<bool>{true});

    status = mac::Permission::Denied;
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error.startsWith("Permesso microfono negato"));
    REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::None);   // denied is an error
    REQUIRE(requests == 1);

    status = mac::Permission::Granted;   // now the HAL is reached
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error.contains("not found"));
}

TEST_CASE("MacEngineBackend asks for the system audio permission off the main thread", "[backend][permissions]")
{
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    config.inputDeviceUid = "no-such-mic-uid";   // fails with "not found" once the aggregate is attempted

    mac::Permission status = mac::Permission::NotDetermined;
    int requests = 0;
    std::function<void(bool, juce::String)> pendingCompletion;
    mac::SystemAudioAccess audio{
        [&] { return status; },
        [&](std::function<void(bool, juce::String)> done) {
            ++requests;
            pendingCompletion = std::move(done);
        },
        [] { return false; }};
    mac::MacEngineBackend::MicrophoneAccess mic{[] { return mac::Permission::Granted; }, nullptr};
    mac::MacEngineBackend backend{graph, aec, chain, [&] { return config; }, mic, audio};
    int answers = 0;
    std::vector<juce::String> warnings;
    std::vector<mac::WarningKind> kinds;
    backend.onSystemAudioPermission = [&] { ++answers; };
    backend.onWarning = [&](mac::WarningKind kind, juce::String w) {
        kinds.push_back(kind);
        warnings.push_back(w);
    };

    juce::String error;
    SECTION("not determined: one prompt, then the tap is created as usual")
    {
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error == "In attesa del permesso per l'audio di sistema");
        REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::SystemAudio);
        REQUIRE(requests == 1);
        REQUIRE_FALSE(backend.openIO(true, error));   // retries while pending do not ask again
        REQUIRE(requests == 1);

        // Without a tap there is nothing to ask; the open fails for another reason.
        REQUIRE_FALSE(backend.openIO(false, error));
        REQUIRE(error.contains("not found"));
        REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::None);

        REQUIRE(pendingCompletion);
        pendingCompletion(true, {});
        REQUIRE(answers == 1);

        // Still "not determined" (the preflight is stale after the prompt): proceeds anyway.
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error.contains("not found"));
        REQUIRE(requests == 1);
        REQUIRE(warnings.empty());
    }
    SECTION("the throwaway IO fails: opens without the tap, warns, asks again on a later open")
    {
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error == "In attesa del permesso per l'audio di sistema");
        REQUIRE(requests == 1);

        // The prompt may not have been shown (e.g. Core Audio restarting): a tap IOProc created
        // now, on the main thread, could raise it there. Re-evaluated at once, without the tap.
        REQUIRE(pendingCompletion);
        pendingCompletion(false, "AudioHardwareCreateProcessTap failed: '!obj'");
        REQUIRE(answers == 1);
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error.contains("not found"));   // the aggregate was attempted
        REQUIRE(warnings.size() == 1);
        REQUIRE(warnings[0].startsWith("Audio di sistema non disponibile"));
        REQUIRE(warnings[0].contains("AudioHardwareCreateProcessTap failed"));
        REQUIRE(kinds == std::vector<mac::WarningKind>{mac::WarningKind::SystemAudioUnavailable});
        REQUIRE(requests == 1);

        // The next open asks again (in the background) instead of creating the tap itself.
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error == "In attesa del permesso per l'audio di sistema");
        REQUIRE(requests == 2);
        pendingCompletion(true, {});
        REQUIRE(answers == 2);
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error.contains("not found"));
        REQUIRE(requests == 2);
        REQUIRE(warnings.size() == 1);
    }
    SECTION("denied: opens without the tap and warns")
    {
        status = mac::Permission::Denied;
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error.contains("not found"));
        REQUIRE(requests == 0);
        REQUIRE(warnings.size() == 1);
        REQUIRE(warnings[0].startsWith("Permesso audio di sistema negato"));
        REQUIRE(warnings[0].contains("Registrazione schermo e audio di sistema"));
        REQUIRE(kinds == std::vector<mac::WarningKind>{mac::WarningKind::SystemAudioDenied});
    }
    SECTION("granted: no prompt, no warning")
    {
        status = mac::Permission::Granted;
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(error.contains("not found"));
        REQUIRE(requests == 0);
        REQUIRE(warnings.empty());
    }
}

// The first-run guide asks through the backend, so it and openIO() share one prompt: whoever asks
// first raises it, the other waits for the same answer.
TEST_CASE("MacEngineBackend raises the system audio prompt once for the guide and the engine", "[backend][permissions]")
{
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    config.inputDeviceUid = "no-such-mic-uid";   // fails with "not found" once the aggregate is attempted

    mac::Permission status = mac::Permission::NotDetermined;
    int requests = 0;
    std::function<void(bool, juce::String)> pendingCompletion;
    mac::SystemAudioAccess audio{
        [&] { return status; },
        [&](std::function<void(bool, juce::String)> done) {
            ++requests;
            pendingCompletion = std::move(done);
        },
        [] { return false; }};
    mac::MacEngineBackend::MicrophoneAccess mic{[] { return mac::Permission::Granted; }, nullptr};
    mac::MacEngineBackend backend{graph, aec, chain, [&] { return config; }, mic, audio};
    int engineAnswers = 0;
    backend.onSystemAudioPermission = [&] { ++engineAnswers; };
    std::vector<bool> guideAnswers;
    const auto guide = [&](bool ok) { guideAnswers.push_back(ok); };
    juce::String error;

    SECTION("the guide first, then the engine opens: no second prompt")
    {
        REQUIRE_FALSE(backend.systemAudioRequestPending());
        backend.requestSystemAudio(guide);
        REQUIRE(requests == 1);
        REQUIRE(backend.systemAudioRequestPending());
        REQUIRE_FALSE(backend.systemAudioAnswered());
        REQUIRE(guideAnswers.empty());

        REQUIRE_FALSE(backend.openIO(true, error));   // waits for the guide's prompt
        REQUIRE(error == mac::MacEngineBackend::kWaitingForSystemAudio);
        REQUIRE(backend.pendingPermission() == fixomic::PendingPermission::SystemAudio);
        backend.requestSystemAudio(guide);           // asked again while it is up: waits too
        REQUIRE(requests == 1);

        pendingCompletion(true, {});
        REQUIRE(guideAnswers == std::vector<bool>{true, true});
        REQUIRE(engineAnswers == 1);                 // the engine re-evaluates
        REQUIRE(backend.systemAudioAnswered());
        REQUIRE_FALSE(backend.systemAudioRequestPending());
        REQUIRE_FALSE(backend.openIO(true, error));  // with the tap, as usual
        REQUIRE(error.contains("not found"));
        REQUIRE(requests == 1);

        backend.requestSystemAudio(guide);           // answered: at once, no prompt
        REQUIRE(guideAnswers.size() == 3);
        REQUIRE(guideAnswers.back());
        REQUIRE(requests == 1);
    }
    SECTION("the engine first, then the guide: it waits for the engine's prompt")
    {
        REQUIRE_FALSE(backend.openIO(true, error));
        REQUIRE(requests == 1);
        REQUIRE(backend.systemAudioRequestPending());
        backend.requestSystemAudio(guide);
        REQUIRE(requests == 1);
        pendingCompletion(true, {});
        REQUIRE(guideAnswers == std::vector<bool>{true});
        REQUIRE(engineAnswers == 1);
    }
    SECTION("a failed throwaway IO: the guide hears it, and asking again raises a new prompt")
    {
        backend.requestSystemAudio(guide);
        pendingCompletion(false, "AudioHardwareCreateAggregateDevice failed");
        REQUIRE(guideAnswers == std::vector<bool>{false});
        REQUIRE_FALSE(backend.systemAudioAnswered());
        backend.requestSystemAudio(guide);
        REQUIRE(requests == 2);
        pendingCompletion(true, {});
        REQUIRE(guideAnswers == std::vector<bool>{false, true});
    }
    SECTION("known to the preflight: nothing to ask")
    {
        for (const auto known : {mac::Permission::Granted, mac::Permission::Denied}) {
            status = known;
            backend.requestSystemAudio(guide);
        }
        REQUIRE(guideAnswers == std::vector<bool>{true, true});
        REQUIRE(requests == 0);
    }
    SECTION("destroyed before the answer: no callback")
    {
        {
            mac::MacEngineBackend shortLived{graph, aec, chain, [&] { return config; }, mic, audio};
            shortLived.requestSystemAudio(guide);
        }
        pendingCompletion(true, {});
        REQUIRE(guideAnswers.empty());
    }
}

TEST_CASE("MacEngineBackend raises the microphone prompt once for the guide and the engine", "[backend][permissions]")
{
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    config.inputDeviceUid = "no-such-mic-uid";

    mac::Permission status = mac::Permission::NotDetermined;
    int requests = 0;
    std::function<void(bool)> pendingCompletion;
    mac::MacEngineBackend::MicrophoneAccess access{[&] { return status; },
                                                   [&](std::function<void(bool)> done) {
                                                       ++requests;
                                                       pendingCompletion = std::move(done);
                                                   }};
    mac::MacEngineBackend backend{graph, aec, chain, [&] { return config; }, access};
    std::vector<bool> engineAnswers, guideAnswers;
    backend.onMicrophonePermission = [&](bool granted) { engineAnswers.push_back(granted); };
    const auto guide = [&](bool granted) { guideAnswers.push_back(granted); };
    juce::String error;

    backend.requestMicrophone(guide);
    REQUIRE(requests == 1);
    REQUIRE(backend.microphoneRequestPending());
    REQUIRE_FALSE(backend.openIO(false, error));   // the engine waits for the same prompt
    REQUIRE(error == mac::MacEngineBackend::kWaitingForMicrophone);
    REQUIRE(requests == 1);

    status = mac::Permission::Granted;
    pendingCompletion(true);
    REQUIRE(guideAnswers == std::vector<bool>{true});
    REQUIRE(engineAnswers == std::vector<bool>{true});
    REQUIRE_FALSE(backend.microphoneRequestPending());

    backend.requestMicrophone(guide);   // answered: at once
    REQUIRE(guideAnswers == std::vector<bool>{true, true});
    REQUIRE(requests == 1);
}

// A new buffer size reaches the AEC and the plugins at the next open, while the IO is closed: the
// prepares run before the HAL is reached, so a failing open shows them too.
TEST_CASE("MacEngineBackend prepares the AEC and the plugins for a new buffer size at the next open",
          "[backend][buffer]")
{
    using fixomic::test::TestPlugin;
    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph{aec, chain};
    fixomic::Config config;
    config.inputDeviceUid = "no-such-mic-uid";
    config.aecEnabled = false;
    config.bufferSize = 128;
    mac::MacEngineBackend::MicrophoneAccess mic{[] { return mac::Permission::Granted; }, nullptr};
    mac::MacEngineBackend backend{graph, aec, chain, [&] { return config; }, mic};
    chain.prepare(48000.0, 128);
    chain.add(std::make_unique<TestPlugin>("counted", 1.0f));
    auto* plugin = chain.pluginAt(0);
    REQUIRE(plugin != nullptr);
    juce::String error;

    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(error.contains("not found"));
    const int afterFirst = TestPlugin::prepareCalls.load();
    REQUIRE(plugin->getBlockSize() == 128);
    REQUIRE_FALSE(backend.openIO(false, error));   // same size: not prepared again
    REQUIRE(TestPlugin::prepareCalls.load() == afterFirst);

    config.bufferSize = 256;
    REQUIRE_FALSE(backend.openIO(false, error));
    REQUIRE(TestPlugin::prepareCalls.load() == afterFirst + 1);
    REQUIRE(plugin->getBlockSize() == 256);
    REQUIRE(chain.pluginAt(0) == plugin);   // the same instance, state kept
}
