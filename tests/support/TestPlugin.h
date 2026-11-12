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
        addHostedParameter(std::make_unique<Parameter>());
        liveInstances.fetch_add(1);
    }

    ~TestPlugin() override {
        liveInstances.fetch_sub(1);
        if (std::this_thread::get_id() != creatorThread_) destroyedOnAnotherThread.fetch_add(1);
    }

    // Instances alive right now, and instances destroyed on a thread other than their creator's.
    inline static std::atomic<int> liveInstances{0};
    inline static std::atomic<int> prepareCalls{0};   // prepareToPlay over all instances
    inline static std::atomic<int> destroyedOnAnotherThread{0};

    float gain() const noexcept { return gain_.load(); }

    // setLatencySamples() notifies listeners with
    // updateHostDisplay(ChangeDetails().withLatencyChanged(true)), as a real plugin wrapper does.
    void changeLatency(int samples) { setLatencySamples(samples); }

    // Makes the plugin look like a hosted VST3 (getVST3Client() != nullptr).
    void pretendVst3() { vst3_ = std::make_unique<FakeVst3Client>(); }
    juce::AudioPluginExtensions::VST3Client* getVST3Client() override { return vst3_.get(); }
    const juce::AudioPluginExtensions::VST3Client* getVST3Client() const override { return vst3_.get(); }

    // processBlock calls with an empty buffer, over all instances.
    inline static std::atomic<int> zeroLengthBlocks{0};

    // As if the user moved the plugin's one parameter in its editor (notifies listeners with
    // audioProcessorParameterChanged on the calling thread).
    void touchParameter(float value) { getParameters()[0]->setValueNotifyingHost(value); }

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
    void prepareToPlay(double, int) override { prepareCalls.fetch_add(1); }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        if (buffer.getNumSamples() == 0) zeroLengthBlocks.fetch_add(1);
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
    struct FakeVst3Client final : juce::AudioPluginExtensions::VST3Client {
        Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override { return nullptr; }
        juce::MemoryBlock getPreset() const override { return {}; }
        bool setPreset(const juce::MemoryBlock&) override { return false; }
    };

    // A parameter with no effect on the audio; only there to exercise parameter notifications.
    struct Parameter final : HostedParameter {
        float getValue() const override { return value.load(); }
        void setValue(float v) override { value.store(v); }
        float getDefaultValue() const override { return 0.0f; }
        juce::String getName(int) const override { return "Amount"; }
        juce::String getLabel() const override { return {}; }
        float getValueForText(const juce::String& text) const override { return text.getFloatValue(); }
        juce::String getParameterID() const override { return "amount"; }
        std::atomic<float> value{0.0f};
    };

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
    std::unique_ptr<FakeVst3Client> vst3_;
    std::atomic<float> gain_;
    const std::thread::id creatorThread_ = std::this_thread::get_id();
};

} // namespace fixomic::test
