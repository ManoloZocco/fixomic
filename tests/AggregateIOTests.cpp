#include <catch2/catch_test_macros.hpp>

#include "platform/mac/AggregateIO.h"
#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/FixomicDevices.h"
#include "platform/mac/SystemAudioTap.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>

using fixomic::mac::AggregateIO;
using fixomic::mac::SystemAudioTap;

namespace {

// Runs the main run loop (and so the main dispatch queue) for `seconds` or until `done`.
template <typename Predicate>
void pumpMainLoop(double seconds, Predicate done)
{
    CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3600, 3600, 0, 0, ^(CFRunLoopTimerRef) {
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end && !done())
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CFRunLoopRemoveTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);
    CFRelease(keepAlive);
}

// FIXOMIC_TEST_MIC_UID, else the built-in microphone, else the first selectable input offering
// 48 kHz (never something like a Continuity iPhone mic when a built-in one exists).
juce::String testMicUid()
{
    if (const char* env = std::getenv("FIXOMIC_TEST_MIC_UID"))
        return env;
    const auto inputs = fixomic::mac::listInputDevices();
    juce::String first;
    for (const auto& d : inputs) {
        if (std::find(d.sampleRates.begin(), d.sampleRates.end(), 48000.0) == d.sampleRates.end())
            continue;
        if (d.uid == "BuiltInMicrophoneDevice")
            return d.uid;
        if (first.isEmpty())
            first = d.uid;
    }
    return first;
}

} // namespace

TEST_CASE("AggregateIO refuses a fixomic device as microphone", "[aggregate]")
{
    juce::String error;
    const auto io = AggregateIO::create({fixomic::devices::kMicUid, 0, nullptr, 128}, {}, error);
    REQUIRE(io == nullptr);
    // Italian, like every error that reaches the panel; the technical detail last, in parentheses.
    REQUIRE(error == juce::String::fromUTF8("Il microfono non pu\u00f2 essere un dispositivo fixomic (")
                         + fixomic::devices::kMicUid + ")");
}

TEST_CASE("AggregateIO reports an unknown microphone in Italian, before any HAL device is opened", "[aggregate]")
{
    juce::String error;
    REQUIRE(AggregateIO::create({"no-such-mic-uid", 0, nullptr, 128}, {}, error) == nullptr);
    REQUIRE(error == "Microfono non trovato (no-such-mic-uid)");
}

TEST_CASE("A device-less AggregateIO owns nothing and never runs", "[aggregate]")
{
    auto io = AggregateIO::createDetached();
    REQUIRE(io != nullptr);
    REQUIRE(io->deviceId() == kAudioObjectUnknown);
    REQUIRE(io->stats().callbacks == 0);
    REQUIRE(io->bufferFrames() == 0);
    REQUIRE_FALSE(io->start());
    REQUIRE(io->lastError().isNotEmpty());
    io->stop();
    io.reset();   // destroys nothing in the HAL
}

TEST_CASE("AggregateIO fails cleanly for an unknown microphone", "[aggregate][hardware]")
{
    juce::String error;
    const auto io = AggregateIO::create({"no-such-mic-uid", 0, nullptr, 128}, {}, error);
    REQUIRE(io == nullptr);
    REQUIRE(error.contains("non trovato"));
}

TEST_CASE("AggregateIO rejects a channel the microphone does not have", "[aggregate][hardware]")
{
    const juce::String mic = testMicUid();
    REQUIRE(mic.isNotEmpty());
    juce::String error;
    REQUIRE(AggregateIO::create({mic, 64, nullptr, 128}, {}, error) == nullptr);
    REQUIRE(error.contains("canale 65"));
}

TEST_CASE("SystemAudioTap is a mono tap with a UID", "[tap][hardware]")
{
    juce::String error;
    const auto tap = SystemAudioTap::create(error);
    INFO(error.toStdString());
    REQUIRE(tap != nullptr);
    REQUIRE(tap->id() != kAudioObjectUnknown);
    REQUIRE(tap->uid().isNotEmpty());
    REQUIRE(tap->channels() == 1);
    REQUIRE(tap->sampleRate() > 0.0);
}

TEST_CASE("AggregateIO runs mic + Bridge without a tap", "[aggregate][hardware]")
{
    const juce::String mic = testMicUid();
    REQUIRE(mic.isNotEmpty());
    std::atomic<int> calls{0};
    std::atomic<bool> refSilent{true};
    juce::String error;
    auto io = AggregateIO::create(
        {mic, 0, nullptr, 128},
        [&](const float*, const float* ref, float* outL, float* outR, int n) {
            for (int i = 0; i < n; ++i) {
                if (ref[i] != 0.0f)
                    refSilent = false;
                outL[i] = outR[i] = 0.0f;
            }
            ++calls;
        },
        error);
    INFO(error.toStdString());
    REQUIRE(io != nullptr);
    REQUIRE(io->nominalSampleRate() == 48000.0);
    REQUIRE(io->bufferFrames() == 128);

    const auto latency = io->latency();
    REQUIRE(latency.sampleRate == 48000.0);
    REQUIRE(latency.bufferFrames == 256);
    REQUIRE(latency.inputDeviceFrames >= 0);
    REQUIRE(latency.aecFrames == 0);
    REQUIRE(latency.pluginFrames == 0);

    REQUIRE(io->start());
    pumpMainLoop(1.0, [&] { return calls.load() > 100; });
    io->stop();
    REQUIRE(calls.load() > 100);
    REQUIRE(refSilent.load());
    REQUIRE(io->stats().sampleTimeGaps == 0);
}

TEST_CASE("AggregateIO reports a mic sample rate change as device lost", "[aggregate][hardware]")
{
    const juce::String mic = testMicUid();
    const auto micId = fixomic::mac::findDeviceByUid(mic);
    REQUIRE(micId.has_value());
    const auto inputs = fixomic::mac::listInputDevices();
    const auto info = std::find_if(inputs.begin(), inputs.end(), [&](const auto& d) { return d.uid == mic; });
    REQUIRE(info != inputs.end());
    if (std::find(info->sampleRates.begin(), info->sampleRates.end(), 44100.0) == info->sampleRates.end())
        SKIP("microphone has no 44.1 kHz");

    juce::String error;
    auto io = AggregateIO::create({mic, 0, nullptr, 128}, [](const float*, const float*, float*, float*, int) {},
                                  error);
    REQUIRE(io != nullptr);
    int lost = 0;
    io->onDeviceLost = [&] {
        ++lost;
        io.reset();  // the owner may destroy the AggregateIO inside the handler
    };
    REQUIRE(io->start());
    pumpMainLoop(0.3, [] { return false; });

    const AudioObjectPropertyAddress rate{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    Float64 value = 44100.0;
    AudioObjectSetPropertyData(*micId, &rate, 0, nullptr, sizeof(value), &value);
    pumpMainLoop(2.0, [&] { return lost > 0; });
    io.reset();
    value = 48000.0;  // leave the mic as fixomic wants it
    AudioObjectSetPropertyData(*micId, &rate, 0, nullptr, sizeof(value), &value);
    pumpMainLoop(0.3, [] { return false; });
    REQUIRE(lost == 1);
}

TEST_CASE("AggregateIO keeps a device-lost event that arrives before a handler is set", "[aggregate][hardware]")
{
    const juce::String mic = testMicUid();
    const auto micId = fixomic::mac::findDeviceByUid(mic);
    REQUIRE(micId.has_value());
    const auto inputs = fixomic::mac::listInputDevices();
    const auto info = std::find_if(inputs.begin(), inputs.end(), [&](const auto& d) { return d.uid == mic; });
    REQUIRE(info != inputs.end());
    if (std::find(info->sampleRates.begin(), info->sampleRates.end(), 44100.0) == info->sampleRates.end())
        SKIP("microphone has no 44.1 kHz");

    juce::String error;
    auto io = AggregateIO::create({mic, 0, nullptr, 128}, [](const float*, const float*, float*, float*, int) {},
                                  error);
    REQUIRE(io != nullptr);

    // No handler yet: the event must not be swallowed.
    const AudioObjectPropertyAddress rate{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    Float64 value = 44100.0;
    AudioObjectSetPropertyData(*micId, &rate, 0, nullptr, sizeof(value), &value);
    pumpMainLoop(1.0, [] { return false; });

    // The stale aggregate refuses to start...
    REQUIRE_FALSE(io->start());
    REQUIRE(io->lastError().contains("perso"));

    // ...and a handler assigned later still hears about the next event.
    int lost = 0;
    io->onDeviceLost = [&] { ++lost; };
    value = 48000.0;
    AudioObjectSetPropertyData(*micId, &rate, 0, nullptr, sizeof(value), &value);
    pumpMainLoop(2.0, [&] { return lost > 0; });
    io.reset();
    pumpMainLoop(0.3, [] { return false; });
    REQUIRE(lost == 1);
}
