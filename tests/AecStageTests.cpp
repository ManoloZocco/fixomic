#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

#include "dsp/AecStage.h"

using fixomic::AecStage;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrame = 480;

// Runs mic/ref through the stage using the given repeating block-size pattern.
std::vector<float> runStage(AecStage& stage, const std::vector<float>& mic, const std::vector<float>& ref,
                            const std::vector<int>& blockPattern) {
    std::vector<float> out(mic.size());
    size_t pos = 0;
    size_t patternIndex = 0;
    while (pos < mic.size()) {
        const int block = std::min<int>(blockPattern[patternIndex++ % blockPattern.size()],
                                        static_cast<int>(mic.size() - pos));
        stage.process(mic.data() + pos, ref.data() + pos, out.data() + pos, block);
        pos += static_cast<size_t>(block);
    }
    return out;
}

double energy(const std::vector<float>& x, size_t begin, size_t end) {
    double e = 0.0;
    for (size_t i = begin; i < end; ++i) e += static_cast<double>(x[i]) * x[i];
    return e;
}

double toDb(double ratio) { return 10.0 * std::log10(ratio); }

// Energy of x[begin, end) inside [lowHz, highHz], via a 1 Hz-spaced bank of DFT bins.
double bandEnergy(const std::vector<float>& x, size_t begin, size_t end, double lowHz, double highHz) {
    const double n = static_cast<double>(end - begin);
    double total = 0.0;
    for (double f = lowHz; f <= highHz; f += 1.0) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * std::numbers::pi * f / kSampleRate;
        for (size_t i = begin; i < end; ++i) {
            const double phase = w * static_cast<double>(i - begin);
            re += x[i] * std::cos(phase);
            im -= x[i] * std::sin(phase);
        }
        total += (re * re + im * im) / n;
    }
    return total;
}

// Fixed-seed white noise (uniform in [-amplitude, amplitude]) as the far-end reference, and a mic
// signal containing a 0.5x copy delayed by 48 samples (a crude model of headphone-jack crosstalk).
struct LeakSignals {
    std::vector<float> ref;
    std::vector<float> mic;
};

LeakSignals makeLeak(size_t numSamples) {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-0.3f, 0.3f);
    LeakSignals s;
    s.ref.resize(numSamples);
    s.mic.assign(numSamples, 0.0f);
    for (auto& v : s.ref) v = dist(rng);
    constexpr size_t delay = 48;
    for (size_t i = delay; i < numSamples; ++i) s.mic[i] = 0.5f * s.ref[i - delay];
    return s;
}

} // namespace

TEST_CASE("AecStage latency is one frame", "[aec]") {
    AecStage stage;
    stage.prepare(kSampleRate, 128);
    REQUIRE(stage.latencySamples() == kFrame);
    // The APM adds its own constant delay on top of the FIFO frame.
    const int total = stage.latencySamples() + stage.apmDelaySamples();
    REQUIRE(stage.apmDelaySamples() > 0);
    REQUIRE(stage.apmDelaySamples() < kFrame);

    std::vector<float> mic(4800, 0.0f), ref(4800, 0.0f);
    mic[0] = 1.0f;
    const auto out = runStage(stage, mic, ref, {128});

    size_t peak = 0;
    for (size_t i = 0; i < out.size(); ++i)
        if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
    INFO("peak at " << peak << " value " << out[peak] << ", APM delay " << stage.apmDelaySamples());
    REQUIRE(peak >= static_cast<size_t>(total - 8));
    REQUIRE(peak <= static_cast<size_t>(total + 8));
    REQUIRE(std::abs(out[peak]) >= 0.1f);
    // Nothing comes out before the FIFO delay line has filled.
    REQUIRE(energy(out, 0, kFrame) < 1e-9);
}

TEST_CASE("AecStage cancels linear leak", "[aec]") {
    AecStage stage;
    stage.prepare(kSampleRate, 128);

    const size_t total = static_cast<size_t>(5 * kSampleRate);
    const auto s = makeLeak(total);
    const auto out = runStage(stage, s.mic, s.ref, {128});

    const size_t begin = static_cast<size_t>(3 * kSampleRate);
    const double micEnergy = energy(s.mic, begin, total);
    const double outEnergy = energy(out, begin, total);
    const double attenuationDb = toDb(outEnergy / micEnergy);
    INFO("output vs mic energy: " << attenuationDb << " dB, ERLE " << stage.erleDb() << " dB");
    REQUIRE(attenuationDb <= -20.0);
}

TEST_CASE("AecStage preserves near-end speech during double-talk", "[aec]") {
    AecStage stage;
    stage.prepare(kSampleRate, 128);

    const size_t converge = static_cast<size_t>(3 * kSampleRate);
    const size_t window = static_cast<size_t>(2 * kSampleRate);
    const size_t total = converge + window + kFrame;
    auto s = makeLeak(total);

    // 440 Hz "voice" added to the mic after convergence.
    std::vector<float> tone(total, 0.0f);
    for (size_t i = converge; i < total; ++i) {
        const double t = static_cast<double>(i - converge) / kSampleRate;
        tone[i] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
        s.mic[i] += tone[i];
    }
    const auto out = runStage(stage, s.mic, s.ref, {128});

    // The output is delayed by one frame: compare the window shifted by the latency.
    const double toneBand = bandEnergy(tone, converge, converge + window, 400.0, 480.0);
    const double outBand = bandEnergy(out, converge + kFrame, converge + kFrame + window, 400.0, 480.0);
    const double ratioDb = toDb(outBand / toneBand);
    INFO("440 Hz band, output vs original: " << ratioDb << " dB");
    REQUIRE(ratioDb >= -6.0);
}

TEST_CASE("AecStage handles arbitrary block sizes", "[aec]") {
    AecStage stage;
    stage.prepare(kSampleRate, 256);

    std::vector<float> mic(9600, 0.0f), ref(9600, 0.0f);
    mic[0] = 1.0f;
    const auto out = runStage(stage, mic, ref, {64, 100, 256});

    size_t peak = 0;
    for (size_t i = 0; i < out.size(); ++i)
        if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
    const int total = stage.latencySamples() + stage.apmDelaySamples();
    REQUIRE(stage.latencySamples() == kFrame);
    REQUIRE(peak >= static_cast<size_t>(total - 8));
    REQUIRE(peak <= static_cast<size_t>(total + 8));
    REQUIRE(std::abs(out[peak]) >= 0.1f);
}

TEST_CASE("AecStage process may run in place", "[aec]") {
    AecStage stage;
    stage.prepare(kSampleRate, 128);

    std::vector<float> buffer(4800, 0.0f), ref(4800, 0.0f);
    buffer[0] = 1.0f;
    size_t peak = 0;
    for (size_t pos = 0; pos < buffer.size(); pos += 128) {
        const int n = static_cast<int>(std::min<size_t>(128, buffer.size() - pos));
        stage.process(buffer.data() + pos, ref.data() + pos, buffer.data() + pos, n);
    }
    for (size_t i = 0; i < buffer.size(); ++i)
        if (std::abs(buffer[i]) > std::abs(buffer[peak])) peak = i;
    const int total = stage.latencySamples() + stage.apmDelaySamples();
    REQUIRE(peak >= static_cast<size_t>(total - 8));
    REQUIRE(peak <= static_cast<size_t>(total + 8));
}
