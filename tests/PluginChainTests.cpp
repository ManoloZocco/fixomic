#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <thread>

#include "dsp/PluginChain.h"
#include "support/AllocCounter.h"
#include "support/TestPlugin.h"

using fixomic::PluginChain;
using fixomic::PluginSlot;
using fixomic::test::TestLayouts;
using fixomic::test::TestPlugin;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

std::unique_ptr<TestPlugin> plugin(const juce::String& name, float gain = 1.0f, int latency = 0,
                                   TestLayouts layouts = TestLayouts::stereoOrMono) {
    return std::make_unique<TestPlugin>(name, gain, latency, layouts);
}

// Processes one block whose left/right channels hold constant values; returns the buffer.
juce::AudioBuffer<float> run(PluginChain& chain, float left, float right, int numSamples = kBlock) {
    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int i = 0; i < numSamples; ++i) {
        buffer.setSample(0, i, left);
        buffer.setSample(1, i, right);
    }
    juce::MidiBuffer midi;
    chain.process(buffer, midi);
    return buffer;
}

bool allEqual(const juce::AudioBuffer<float>& buffer, int channel, float value) {
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        if (buffer.getSample(channel, i) != value) return false;
    return true;
}

TestPlugin& testPluginAt(const PluginChain& chain, int index) {
    auto* p = dynamic_cast<TestPlugin*>(chain.pluginAt(index));
    REQUIRE(p != nullptr);
    return *p;
}

juce::String descriptionXml(const juce::String& name) {
    juce::PluginDescription d;
    d.name = name;
    d.pluginFormatName = "VST3";
    d.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/" + name + ".vst3";
    d.uniqueId = name.hashCode();
    return d.createXml()->toString();
}

} // namespace

TEST_CASE("PluginChain processes plugins in order", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("x2", 2.0f));
    chain.add(plugin("x3", 3.0f));

    const auto out = run(chain, 1.0f, 1.0f);
    REQUIRE(allEqual(out, 0, 6.0f));
    REQUIRE(allEqual(out, 1, 6.0f));
}

TEST_CASE("PluginChain passes audio through before prepare", "[chain]") {
    PluginChain chain;
    chain.add(plugin("x2", 2.0f));
    REQUIRE(allEqual(run(chain, 1.0f, 0.5f), 0, 1.0f));
}

TEST_CASE("PluginChain bypass skips plugin and its latency", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("x2", 2.0f, 10));
    chain.add(plugin("x3", 3.0f, 20));
    REQUIRE(chain.totalLatencySamples() == 30);

    chain.setBypassed(1, true);
    REQUIRE(chain.slots()[1].bypassed);
    REQUIRE(chain.slots()[1].latencySamples == 20);
    REQUIRE(chain.totalLatencySamples() == 10);
    REQUIRE(allEqual(run(chain, 1.0f, 1.0f), 0, 2.0f));

    chain.setBypassed(1, false);
    REQUIRE(chain.totalLatencySamples() == 30);
    REQUIRE(allEqual(run(chain, 1.0f, 1.0f), 0, 6.0f));
}

TEST_CASE("PluginChain move reorders", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("first"));
    chain.add(plugin("second"));

    chain.move(1, 0);
    REQUIRE(chain.slots().size() == 2);
    REQUIRE(chain.slots()[0].name == "second");
    REQUIRE(chain.slots()[1].name == "first");
    REQUIRE(chain.pluginAt(0)->getName() == "second");
}

TEST_CASE("PluginChain add inserts at the requested index and remove deletes", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    REQUIRE(chain.add(plugin("a")) == 0);
    REQUIRE(chain.add(plugin("b")) == 1);
    REQUIRE(chain.add(plugin("c"), 0) == 0);
    REQUIRE(chain.add(plugin("d"), 99) == 3);

    chain.remove(1); // "a"
    chain.remove(7); // out of range: ignored
    const auto s = chain.slots();
    REQUIRE(s.size() == 3);
    REQUIRE(s[0].name == "c");
    REQUIRE(s[1].name == "b");
    REQUIRE(s[2].name == "d");
    REQUIRE(chain.pluginAt(3) == nullptr);
}

TEST_CASE("PluginChain mono plugin output copied to both channels", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("mono x2", 2.0f, 0, TestLayouts::monoOnly));
    REQUIRE(chain.pluginAt(0)->getTotalNumInputChannels() == 1);

    const auto out = run(chain, 1.0f, 0.5f);
    REQUIRE(allEqual(out, 0, 2.0f));
    REQUIRE(allEqual(out, 1, 2.0f));
}

TEST_CASE("PluginChain configures stereo plugins as stereo", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("stereo x2", 2.0f));
    REQUIRE(chain.pluginAt(0)->getTotalNumInputChannels() == 2);
    REQUIRE(chain.pluginAt(0)->getTotalNumOutputChannels() == 2);

    const auto out = run(chain, 1.0f, 0.5f);
    REQUIRE(allEqual(out, 0, 2.0f));
    REQUIRE(allEqual(out, 1, 1.0f));
}

TEST_CASE("PluginChain add rejects plugins without a stereo or mono layout", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    REQUIRE_THROWS_WITH(chain.add(plugin("Some Synth", 1.0f, 0, TestLayouts::none)),
                        Catch::Matchers::ContainsSubstring("Some Synth"));
    REQUIRE(chain.slots().empty());
}

TEST_CASE("PluginChain edits notify onChanged", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    int changes = 0;
    chain.onChanged = [&] { ++changes; };

    chain.add(plugin("a"));
    chain.add(plugin("b"));
    chain.setBypassed(0, true);
    chain.move(0, 1);
    chain.remove(0);
    REQUIRE(changes == 5);
}

TEST_CASE("PluginChain process does not allocate", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("x2", 2.0f));
    chain.add(plugin("mono x3", 3.0f, 0, TestLayouts::monoOnly));
    chain.add(plugin("bypassed", 5.0f));
    chain.setBypassed(2, true);
    chain.restore(chain.saveState(), [](const juce::PluginDescription& d, double, int, juce::String& error)
                                         -> std::unique_ptr<juce::AudioPluginInstance> {
        if (d.name == "x2") return plugin("x2", 2.0f);
        if (d.name == "mono x3") return plugin("mono x3", 3.0f, 0, TestLayouts::monoOnly);
        error = "not installed";
        return nullptr; // "bypassed" becomes a missing slot
    });
    REQUIRE(chain.slots()[2].missing);

    juce::AudioBuffer<float> normal(2, kBlock), longer(2, 3 * kBlock + 17); // longer: chunked
    juce::MidiBuffer midi;
    normal.clear();
    longer.clear();

    fixomic::test::startCountingAllocations();
    chain.process(normal, midi);
    chain.process(longer, midi);
    REQUIRE(fixomic::test::stopCountingAllocations() == 0);
}

TEST_CASE("PluginChain processes blocks longer than maxBlockSize in chunks", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("x2", 2.0f));
    const auto out = run(chain, 1.0f, 1.0f, 3 * kBlock + 17);
    REQUIRE(allEqual(out, 0, 2.0f));
    REQUIRE(allEqual(out, 1, 2.0f));
}

TEST_CASE("PluginChain remove while audio thread running", "[chain][stress]") {
    const int liveBefore = TestPlugin::liveInstances.load();
    const int offThreadBefore = TestPlugin::destroyedOnAnotherThread.load();
    {
        PluginChain chain;
        chain.prepare(kSampleRate, kBlock);
        chain.add(plugin("seed"));

        std::atomic<bool> stop{false};
        std::atomic<long> blocks{0};
        std::atomic<bool> nonFinite{false};
        std::thread audio([&] {
            juce::AudioBuffer<float> buffer(2, kBlock);
            juce::MidiBuffer midi;
            while (!stop.load()) {
                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill(buffer.getWritePointer(ch), 0.25f, kBlock);
                chain.process(buffer, midi);
                if (!std::isfinite(buffer.getSample(0, 0)) || !std::isfinite(buffer.getSample(1, kBlock - 1)))
                    nonFinite = true;
                blocks.fetch_add(1);
            }
        });

        const auto start = std::chrono::steady_clock::now();
        std::mt19937 rng(20261111);
        auto pick = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
        for (int op = 0; op < 200; ++op) {
            const int size = static_cast<int>(chain.slots().size());
            const int kind = size == 0 ? 0 : pick(10);
            if (kind < 4) {
                const auto layouts = pick(2) == 0 ? TestLayouts::stereoOrMono : TestLayouts::monoOnly;
                chain.add(plugin("p" + juce::String(op), 0.5f + 0.1f * static_cast<float>(pick(10)), pick(64), layouts),
                          pick(size + 1));
            } else if (kind < 7) {
                chain.remove(pick(size));
            } else if (kind < 9) {
                chain.move(pick(size), pick(size));
            } else {
                chain.setBypassed(pick(size), pick(2) == 0);
            }
            chain.collectGarbage();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop = true;
        audio.join();

        REQUIRE(blocks.load() > 0);
        REQUIRE_FALSE(nonFinite.load());

        // Once the audio thread is idle, every retired plugin is freed.
        chain.collectGarbage();
        REQUIRE(TestPlugin::liveInstances.load() - liveBefore == static_cast<int>(chain.slots().size()));
    }
    REQUIRE(TestPlugin::liveInstances.load() == liveBefore);
    REQUIRE(TestPlugin::destroyedOnAnotherThread.load() == offThreadBefore);
}

TEST_CASE("PluginChain keeps a removed plugin alive until collectGarbage", "[chain]") {
    const int liveBefore = TestPlugin::liveInstances.load();
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("a"));
    chain.remove(0);
    REQUIRE(TestPlugin::liveInstances.load() == liveBefore + 1);
    chain.collectGarbage();
    REQUIRE(TestPlugin::liveInstances.load() == liveBefore);
}

TEST_CASE("PluginChain latency change notifies", "[chain]") {
    // The test's main thread becomes the JUCE message thread.
    juce::ScopedJuceInitialiser_GUI juce;

    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("a", 1.0f, 0));
    chain.add(plugin("b", 1.0f, 16));
    int changes = 0;
    chain.onChanged = [&] { ++changes; };

    SECTION("reported on the message thread: notified synchronously") {
        testPluginAt(chain, 0).changeLatency(64);
        REQUIRE(changes == 1);
        REQUIRE(chain.totalLatencySamples() == 80);
        REQUIRE(chain.slots()[0].latencySamples == 64);
    }

    SECTION("reported on another thread: delivered once by collectGarbage") {
        std::thread([&] { testPluginAt(chain, 0).changeLatency(128); }).join();
        REQUIRE(changes == 0);
        REQUIRE(chain.totalLatencySamples() == 144);
        chain.collectGarbage();
        REQUIRE(changes == 1);
        chain.collectGarbage();
        REQUIRE(changes == 1);
    }
}

TEST_CASE("PluginChain restore keeps missing plugins", "[chain]") {
    PluginChain chain;
    chain.prepare(kSampleRate, kBlock);
    chain.add(plugin("replaced"));

    juce::MemoryBlock gainState;
    {
        TestPlugin source("Gain", 4.0f);
        source.getStateInformation(gainState);
    }
    const PluginSlot gain{descriptionXml("Gain"), gainState, false};
    const PluginSlot ghost{descriptionXml("Ghost"), juce::MemoryBlock("\x01\x02\x03", 3), true};
    const PluginSlot garbage{"not a plugin description", juce::MemoryBlock("\x09", 1), false};

    double factoryRate = 0.0;
    int factoryBlock = 0;
    const auto errors = chain.restore(
        {gain, ghost, garbage},
        [&](const juce::PluginDescription& d, double rate, int block, juce::String& error)
            -> std::unique_ptr<juce::AudioPluginInstance> {
            factoryRate = rate;
            factoryBlock = block;
            if (d.name == "Gain") return plugin("Gain", 1.0f);
            error = "not installed";
            return nullptr;
        });

    REQUIRE(factoryRate == kSampleRate);
    REQUIRE(factoryBlock == kBlock);
    REQUIRE(errors.size() == 2);
    REQUIRE(errors[0].contains("Ghost"));
    REQUIRE(errors[0].contains("not installed"));

    const auto s = chain.slots();
    REQUIRE(s.size() == 3);
    REQUIRE_FALSE(s[0].missing);
    REQUIRE(s[0].name == "Gain");
    REQUIRE(testPluginAt(chain, 0).gain() == 4.0f); // state applied
    REQUIRE(s[1].missing);
    REQUIRE(s[1].name == "Ghost");
    REQUIRE(s[1].bypassed);
    REQUIRE(s[1].latencySamples == 0);
    REQUIRE(s[2].missing);
    REQUIRE(chain.pluginAt(1) == nullptr);
    REQUIRE(chain.pluginAt(2) == nullptr);

    // Missing slots pass audio through.
    REQUIRE(allEqual(run(chain, 1.0f, 1.0f), 0, 4.0f));

    const auto saved = chain.saveState();
    REQUIRE(saved.size() == 3);
    REQUIRE(saved[1].descriptionXml == ghost.descriptionXml);
    REQUIRE(saved[1].state == ghost.state);
    REQUIRE(saved[1].bypassed);
    REQUIRE(saved[2].descriptionXml == garbage.descriptionXml);
    REQUIRE(saved[2].state == garbage.state);

    juce::PluginDescription savedGain;
    const auto xml = juce::parseXML(saved[0].descriptionXml);
    REQUIRE(xml != nullptr);
    REQUIRE(savedGain.loadFromXml(*xml));
    REQUIRE(savedGain.name == "Gain");
    REQUIRE(saved[0].state == gainState);
}
