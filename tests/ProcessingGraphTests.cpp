#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "dsp/AecStage.h"
#include "dsp/Meter.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "support/AllocCounter.h"
#include "support/TestPlugin.h"

using fixomic::AecStage;
using fixomic::Meter;
using fixomic::PluginChain;
using fixomic::ProcessingGraph;
using fixomic::test::TestPlugin;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

// Owns a prepared AEC stage + chain (one gain-2 plugin) + graph.
struct Rig {
    AecStage aec;
    PluginChain chain;
    ProcessingGraph graph{aec, chain};

    explicit Rig(bool withGainPlugin = true) {
        aec.prepare(kSampleRate, kBlock);
        chain.prepare(kSampleRate, kBlock);
        if (withGainPlugin) chain.add(std::make_unique<TestPlugin>("x2", 2.0f));
        graph.prepare(kSampleRate, kBlock);
    }

    // Feeds mic/ref through the graph in blocks of `block`; returns the two output channels.
    void run(const std::vector<float>& mic, const std::vector<float>* ref, std::vector<float>& outL,
             std::vector<float>& outR, int block = kBlock) {
        outL.assign(mic.size(), 0.0f);
        outR.assign(mic.size(), 0.0f);
        for (size_t pos = 0; pos < mic.size(); pos += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), mic.size() - pos));
            graph.process(mic.data() + pos, ref != nullptr ? ref->data() + pos : nullptr, outL.data() + pos,
                          outR.data() + pos, n);
        }
    }
};

std::vector<float> ramp(size_t n) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = 0.001f * static_cast<float>(i % 400) - 0.2f;
    return v;
}

} // namespace

TEST_CASE("Meter starts at the floor and tracks the block peak", "[meter]") {
    Meter m;
    m.prepare(kSampleRate);
    REQUIRE(m.peakDb() <= -100.0f);

    std::vector<float> block(kBlock, 0.0f);
    block[10] = -0.5f;
    m.push(block.data(), kBlock);
    REQUIRE(m.peakDb() == Catch::Approx(-6.02f).margin(0.05f));

    block[10] = 1.0f;
    m.push(block.data(), kBlock);
    REQUIRE(m.peakDb() == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("Meter decays at 20 dB per second and never goes below -100 dB", "[meter]") {
    Meter m;
    m.prepare(kSampleRate);
    std::vector<float> loud(kBlock, 1.0f);
    m.push(loud.data(), kBlock);

    const std::vector<float> silence(kBlock, 0.0f);
    const int blocksPerHalfSecond = static_cast<int>(0.5 * kSampleRate / kBlock);
    for (int i = 0; i < blocksPerHalfSecond; ++i) m.push(silence.data(), kBlock);
    // 0 dB peak, then 0.5 s of silence (the pushing block itself decays too, hence the margin).
    REQUIRE(m.peakDb() == Catch::Approx(-10.0f).margin(0.3f));

    for (int i = 0; i < 100 * 375; ++i) m.push(silence.data(), kBlock);
    REQUIRE(m.peakDb() == -100.0f);
}

TEST_CASE("Meter holds a peak louder than the decayed level", "[meter]") {
    Meter m;
    m.prepare(kSampleRate);
    std::vector<float> loud(kBlock, 0.8f), quiet(kBlock, 0.1f);
    m.push(loud.data(), kBlock);
    m.push(quiet.data(), kBlock);   // decay over one block is ~0.05 dB: 0.8 stays on top
    REQUIRE(m.peakDb() > -2.0f);
}

TEST_CASE("ProcessingGraph bypass copies mic to both channels and skips the chain", "[graph]") {
    Rig rig;
    rig.graph.setBypass(true);
    const auto mic = ramp(4 * kBlock);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r);
    REQUIRE(l == mic);
    REQUIRE(r == mic);
    REQUIRE_FALSE(rig.graph.aecActive());
}

TEST_CASE("ProcessingGraph bypass skips the AEC even with a reference", "[graph]") {
    Rig rig;
    rig.graph.setAecEnabled(true);
    rig.graph.setBypass(true);
    const auto mic = ramp(4 * kBlock);
    const std::vector<float> ref(mic.size(), 0.1f);
    std::vector<float> l, r;
    rig.run(mic, &ref, l, r);
    REQUIRE(l == mic);
    REQUIRE(r == mic);
    REQUIRE_FALSE(rig.graph.aecActive());
}

TEST_CASE("ProcessingGraph outputs silence when the mic is absent", "[graph]") {
    Rig rig;
    rig.graph.setMicPresent(false);
    std::vector<float> l, r;
    // The mic pointer must not even be read: pass nullptr.
    l.assign(kBlock, 1.0f);
    r.assign(kBlock, 1.0f);
    rig.graph.process(nullptr, nullptr, l.data(), r.data(), kBlock);
    REQUIRE(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(r.begin(), r.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(rig.graph.outputMeter.peakDb() <= -100.0f);
    REQUIRE_FALSE(rig.graph.aecActive());
}

TEST_CASE("ProcessingGraph without a reference tap leaves the AEC inactive and adds no delay", "[graph]") {
    Rig rig;
    rig.graph.setAecEnabled(true);
    const auto mic = ramp(5 * kBlock);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r);
    REQUIRE_FALSE(rig.graph.aecActive());
    for (size_t i = 0; i < mic.size(); ++i) {
        REQUIRE(l[i] == 2.0f * mic[i]);
        REQUIRE(r[i] == 2.0f * mic[i]);
    }
}

TEST_CASE("ProcessingGraph with AEC disabled ignores the reference", "[graph]") {
    Rig rig;
    const auto mic = ramp(3 * kBlock);
    const std::vector<float> ref(mic.size(), 0.1f);
    std::vector<float> l, r;
    rig.run(mic, &ref, l, r);
    REQUIRE_FALSE(rig.graph.aecActive());
    for (size_t i = 0; i < mic.size(); ++i) REQUIRE(l[i] == 2.0f * mic[i]);
}

TEST_CASE("ProcessingGraph with AEC and a reference delays the mic by the AEC latency", "[graph]") {
    Rig rig(false);   // empty chain: pure AEC delay
    rig.graph.setAecEnabled(true);
    const int total = rig.aec.latencySamples();
    REQUIRE(total > 0);

    std::vector<float> mic(4800, 0.0f);
    const std::vector<float> ref(4800, 0.0f);
    mic[0] = 1.0f;
    std::vector<float> l, r;
    rig.run(mic, &ref, l, r);
    REQUIRE(rig.graph.aecActive());

    size_t peak = 0;
    for (size_t i = 0; i < l.size(); ++i)
        if (std::abs(l[i]) > std::abs(l[peak])) peak = i;
    INFO("peak at " << peak << ", AEC latency " << total);
    REQUIRE(peak >= static_cast<size_t>(total - 8));
    REQUIRE(peak <= static_cast<size_t>(total + 8));
    REQUIRE(std::abs(l[peak]) >= 0.1f);
    REQUIRE(l == r);
}

TEST_CASE("ProcessingGraph runs the chain after the AEC", "[graph]") {
    Rig rig;   // chain: gain 2
    rig.graph.setAecEnabled(true);
    const int total = rig.aec.latencySamples();

    std::vector<float> mic(4800, 0.0f);
    const std::vector<float> ref(4800, 0.0f);
    mic[0] = 1.0f;
    std::vector<float> l, r;
    rig.run(mic, &ref, l, r);

    size_t peak = 0;
    for (size_t i = 0; i < l.size(); ++i)
        if (std::abs(l[i]) > std::abs(l[peak])) peak = i;
    REQUIRE(peak >= static_cast<size_t>(total - 8));
    REQUIRE(peak <= static_cast<size_t>(total + 8));
    // The AEC output of the unit impulse is attenuated by the APM; the chain must double whatever
    // the AEC alone produces.
    Rig aecOnly(false);
    aecOnly.graph.setAecEnabled(true);
    std::vector<float> l2, r2;
    aecOnly.run(mic, &ref, l2, r2);
    for (size_t i = 0; i < l.size(); ++i) REQUIRE(l[i] == Catch::Approx(2.0f * l2[i]).margin(1e-6));
}

TEST_CASE("ProcessingGraph meters follow input and output", "[graph]") {
    Rig rig;
    std::vector<float> mic(kBlock, 0.25f), l(kBlock), r(kBlock);
    rig.graph.process(mic.data(), nullptr, l.data(), r.data(), kBlock);
    REQUIRE(rig.graph.inputMeter.peakDb() == Catch::Approx(-12.04f).margin(0.05f));
    REQUIRE(rig.graph.outputMeter.peakDb() == Catch::Approx(-6.02f).margin(0.05f));
}

TEST_CASE("ProcessingGraph handles blocks longer than maxBlockSize and n == 0", "[graph]") {
    Rig rig;
    const auto mic = ramp(3 * kBlock + 17);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r, static_cast<int>(mic.size()));
    for (size_t i = 0; i < mic.size(); ++i) {
        REQUIRE(l[i] == 2.0f * mic[i]);
        REQUIRE(r[i] == 2.0f * mic[i]);
    }
    rig.graph.process(mic.data(), nullptr, l.data(), r.data(), 0);   // must not crash
}

TEST_CASE("ProcessingGraph process may run in place on the left output", "[graph]") {
    Rig rig;
    const auto mic = ramp(kBlock);
    std::vector<float> l = mic, r(kBlock);
    rig.graph.process(l.data(), nullptr, l.data(), r.data(), kBlock);
    for (size_t i = 0; i < mic.size(); ++i) {
        REQUIRE(l[i] == 2.0f * mic[i]);
        REQUIRE(r[i] == 2.0f * mic[i]);
    }
}

TEST_CASE("ProcessingGraph process never allocates", "[graph][alloc]") {
    Rig rig;
    rig.graph.setAecEnabled(true);
    std::vector<float> mic(kBlock, 0.1f), ref(kBlock, 0.2f), l(kBlock), r(kBlock);
    std::vector<float> longMic(3 * kBlock + 17, 0.1f), longRef(3 * kBlock + 17, 0.2f), ll(longMic.size()),
        lr(longMic.size());
    // One warm-up block of each kind, then count.
    rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);

    fixomic::test::startCountingAllocations();
    for (int i = 0; i < 20; ++i) rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);
    rig.graph.process(longMic.data(), longRef.data(), ll.data(), lr.data(), static_cast<int>(longMic.size()));
    rig.graph.process(mic.data(), nullptr, l.data(), r.data(), kBlock);   // AEC toggles inactive
    rig.graph.setBypass(true);
    rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);
    rig.graph.setBypass(false);
    rig.graph.setMicPresent(false);
    rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);
    REQUIRE(fixomic::test::stopCountingAllocations() == 0);
}

TEST_CASE("ProcessingGraph before prepare passes the mic through", "[graph]") {
    AecStage aec;
    PluginChain chain;
    ProcessingGraph graph(aec, chain);
    const auto mic = ramp(kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    graph.process(mic.data(), nullptr, l.data(), r.data(), kBlock);
    REQUIRE(l == mic);
    REQUIRE(r == mic);
}

namespace {

constexpr int kMute = 14400;   // 300 ms at 48 kHz
constexpr int kFade = 960;     // 20 ms

// Startup-muted output of a constant mic through an empty chain (unity), fed in `block`s.
std::vector<float> startupMuted(int block, float level = 0.5f) {
    Rig rig(false);
    rig.graph.armStartupMute(kMute, kFade);
    const std::vector<float> mic(static_cast<size_t>(kMute + kFade + 2000), level);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r, block);
    REQUIRE(l == r);
    return l;
}

void requireStartupShape(const std::vector<float>& out, float level) {
    for (int i = 0; i < kMute; ++i) {
        INFO("sample " << i);
        REQUIRE(out[static_cast<size_t>(i)] == 0.0f);
    }
    for (int k = 0; k < kFade; ++k) {
        INFO("fade sample " << k);
        REQUIRE(out[static_cast<size_t>(kMute + k)]
                == Catch::Approx(level * static_cast<float>(k) / kFade).margin(1e-6));
    }
    for (size_t i = kMute + kFade; i < out.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(out[i] == level);
    }
}

} // namespace

TEST_CASE("ProcessingGraph startup mute: silence, then a linear fade-in, then unity", "[graph]") {
    requireStartupShape(startupMuted(kBlock), 0.5f);
}

TEST_CASE("ProcessingGraph startup mute works across odd and oversized block boundaries", "[graph]") {
    requireStartupShape(startupMuted(77), 0.5f);
    requireStartupShape(startupMuted(4096), 0.5f);   // longer than maxBlockSize: chunked
    requireStartupShape(startupMuted(1), 0.5f);
}

TEST_CASE("ProcessingGraph startup mute also covers bypass and is re-armable", "[graph]") {
    Rig rig(false);
    rig.graph.setBypass(true);
    rig.graph.armStartupMute(kMute, kFade);
    const std::vector<float> mic(static_cast<size_t>(kMute + kFade + 500), -0.25f);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r);
    requireStartupShape(l, -0.25f);
    requireStartupShape(r, -0.25f);

    // Armed again (next openIO): starts over.
    rig.graph.armStartupMute(kMute, kFade);
    rig.run(mic, nullptr, l, r);
    requireStartupShape(l, -0.25f);

    // Not armed: no mute at all.
    Rig plain(false);
    plain.run(mic, nullptr, l, r);
    REQUIRE(l == mic);
}

TEST_CASE("ProcessingGraph startup mute keeps the output meter silent while muted", "[graph]") {
    Rig rig(false);
    rig.graph.armStartupMute(kMute, kFade);
    const std::vector<float> mic(static_cast<size_t>(kMute / 2), 0.9f);
    std::vector<float> l, r;
    rig.run(mic, nullptr, l, r);
    REQUIRE(rig.graph.outputMeter.peakDb() <= -100.0f);
    REQUIRE(rig.graph.inputMeter.peakDb() > -1.0f);
}

TEST_CASE("ProcessingGraph startup mute never allocates", "[graph][alloc]") {
    Rig rig;
    rig.graph.setAecEnabled(true);
    std::vector<float> mic(kBlock, 0.1f), ref(kBlock, 0.2f), l(kBlock), r(kBlock);
    rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);   // warm-up

    fixomic::test::startCountingAllocations();
    rig.graph.armStartupMute(kMute, kFade);
    for (int i = 0; i < (kMute + kFade) / kBlock + 20; ++i)
        rig.graph.process(mic.data(), ref.data(), l.data(), r.data(), kBlock);
    REQUIRE(fixomic::test::stopCountingAllocations() == 0);
    REQUIRE(l[0] != 0.0f);   // past the fade by now
}
