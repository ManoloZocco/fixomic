#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstring>
#include <thread>

namespace fixomic::test {

// Bus layouts a TestPlugin accepts.
enum class TestLayouts {
    stereoOrMono, // main in == main out, mono or stereo
    monoOnly,     // mono in, mono out
    none,         // instrument-like: no input bus, so it cannot sit in an effect chain
};

// Minimal hosted-plugin stand-in: multiplies every channel by a constant gain and reports a
// configurable latency. The gain is its whole state (get/setStateInformation).
class TestPlugin final : public juce::AudioPluginInstance {
public:
    explicit TestPlugin(juce::String name, float gain = 1.0f, int latencySamples = 0,
                        TestLayouts layouts = TestLayouts::stereoOrMono)
        : juce::AudioPluginInstance(makeBuses(layouts)), name_(std::move(name)), layouts_(layouts), gain_(gain) {
        setLatencySamples(latencySamples);
        liveInstances.fetch_add(1);
    }

    ~TestPlugin() override {
        liveInstances.fetch_sub(1);
        if (std::this_thread::get_id() != creatorThread_) destroyedOnAnotherThread.fetch_add(1);
    }

    // Instances alive right now, and instances destroyed on a thread other than their creator's.
    inline static std::atomic<int> liveInstances{0};
    inline static std::atomic<int> destroyedOnAnotherThread{0};

    float gain() const noexcept { return gain_.load(); }

    // setLatencySamples() notifies listeners with
    // updateHostDisplay(ChangeDetails().withLatencyChanged(true)), as a real plugin wrapper does.
    void changeLatency(int samples) { setLatencySamples(samples); }

    void fillInPluginDescription(juce::PluginDescription& d) const override {
        d.name = d.descriptiveName = name_;
        d.pluginFormatName = "Test";
        d.category = layouts_ == TestLayouts::none ? "Synth" : "Effect";
        d.manufacturerName = "fixomic";
        d.version = "1.0";
        d.fileOrIdentifier = "test:" + name_;
        d.uniqueId = d.deprecatedUid = name_.hashCode();
        d.isInstrument = layouts_ == TestLayouts::none;
        d.numInputChannels = getTotalNumInputChannels();
        d.numOutputChannels = getTotalNumOutputChannels();
    }

    const juce::String getName() const override { return name_; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        buffer.applyGain(gain_.load());
    }
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override {
        const float g = gain_.load();
        dest.replaceAll(&g, sizeof(g));
    }
    void setStateInformation(const void* data, int size) override {
        if (size == static_cast<int>(sizeof(float))) {
            float g;
            std::memcpy(&g, data, sizeof(g));
            gain_.store(g);
        }
    }

protected:
    bool isBusesLayoutSupported(const BusesLayout& layout) const override {
        const auto in = layout.getMainInputChannelSet();
        const auto out = layout.getMainOutputChannelSet();
        switch (layouts_) {
            case TestLayouts::stereoOrMono:
                return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
            case TestLayouts::monoOnly:
                return in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::mono();
            case TestLayouts::none:
                return layout.inputBuses.isEmpty() && out == juce::AudioChannelSet::stereo();
        }
        return false;
    }

private:
    static BusesProperties makeBuses(TestLayouts layouts) {
        switch (layouts) {
            case TestLayouts::stereoOrMono:
                return BusesProperties()
                    .withInput("Input", juce::AudioChannelSet::stereo())
                    .withOutput("Output", juce::AudioChannelSet::stereo());
            case TestLayouts::monoOnly:
                return BusesProperties()
                    .withInput("Input", juce::AudioChannelSet::mono())
                    .withOutput("Output", juce::AudioChannelSet::mono());
            case TestLayouts::none:
                return BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo());
        }
        return {};
    }

    const juce::String name_;
    const TestLayouts layouts_;
    std::atomic<float> gain_;
    const std::thread::id creatorThread_ = std::this_thread::get_id();
};

} // namespace fixomic::test
