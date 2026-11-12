#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/Config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fixomic {

// Creates a plugin instance for a description, already initialised for the given rate and block
// size. Returns nullptr and fills `error` on failure.
using PluginFactory = std::function<std::unique_ptr<juce::AudioPluginInstance>(
    const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error)>;

// Ordered chain of hosted plugins that processes a stereo buffer on the audio thread while the
// message thread edits it.
//
// The structure lives in immutable snapshots. Every edit builds a new snapshot and publishes it
// with one atomic exchange; the audio thread announces the snapshot it is reading through a
// hazard pointer (`inUse`), so collectGarbage() frees only retired snapshots that no block can
// still be reading. Plugin instances are therefore destroyed only in collectGarbage() (or the
// destructor), on the message thread.
//
// Bus layout: each plugin added is configured as {stereo in, stereo out}, else {mono, mono}; a
// mono plugin processes channel 0, which is then copied to channel 1. Slots whose plugin could not
// be restored ("missing") pass audio through and keep their saved description and state.
//
// Threading: every method except process() is message-thread only. prepare() must not run
// concurrently with process(); the others may.
class PluginChain : private juce::AudioProcessorListener {
public:
    struct SlotInfo {
        juce::String name;
        bool bypassed;
        bool missing;
        int latencySamples;
        bool operator==(const SlotInfo&) const = default;
    };

    PluginChain();
    ~PluginChain() override;   // process() must have stopped
    PluginChain(const PluginChain&) = delete;
    PluginChain& operator=(const PluginChain&) = delete;

    // ---- message thread ----------------------------------------------------------------------

    // (Re)prepares every plugin. Until the first call, process() passes audio through.
    void prepare(double sampleRate, int maxBlockSize);

    // Inserts at `index` (-1 or out of range = append) and returns the actual index. Throws
    // std::runtime_error naming the plugin if it supports neither stereo nor mono in/out.
    int add(std::unique_ptr<juce::AudioPluginInstance>, int index = -1);
    void remove(int index);              // out of range: no-op
    void move(int from, int to);         // `to` is clamped; `from` out of range: no-op
    void setBypassed(int index, bool) noexcept;

    juce::AudioPluginInstance* pluginAt(int index) const;   // nullptr if missing or out of range
    std::vector<SlotInfo> slots() const;
    int totalLatencySamples() const;                         // sum over non-bypassed slots

    // Missing slots return their original descriptionXml and state verbatim.
    std::vector<PluginSlot> saveState() const;

    // A hosted VST3 hands parameter changes to its processor only inside a process call, so an edit
    // made in its editor while nothing processes is missing from its state. This gives every VST3
    // plugin a zero-length block (the VST3 "parameter flush") so saveState() sees the edit. Only
    // while process() is not running (engine IO closed); no-op before prepare().
    void flushPluginParameters();

    // The same flush while the audio thread runs: the slots it skips (bypassed ones, or every slot
    // when the graph is globally bypassed and calls processSkipped() instead of process()) never
    // see their queued edits. Asks the audio thread to give each skipped VST3 a zero-length block
    // at the end of its next process() / processSkipped() and waits for it at most `timeoutMs`
    // (sleeping 1 ms steps). True if the audio thread acknowledged in time.
    bool flushPluginParametersOnAudioThread(int timeoutMs);

    // Replaces the whole chain. Slots that cannot be recreated become missing slots; returns one
    // readable error per such slot.
    std::vector<juce::String> restore(const std::vector<PluginSlot>&, const PluginFactory&);

    // Structure, bypass or latency changed. Always invoked on the message thread: synchronously
    // for edits and for latency changes reported on the message thread; latency changes reported
    // on any other thread are flagged lock-free and delivered by the next collectGarbage().
    std::function<void()> onChanged;

    // A plugin reported a parameter, program or other state change (the user editing it, or a
    // preset load). Plugins report these on any thread, from inside their own calls, so it is
    // always deferred: flagged lock-free and delivered once (however many changes) by the next
    // collectGarbage(), on the message thread. Not fired for chain edits (see onChanged).
    std::function<void()> onPluginStateChanged;

    // Frees retired snapshots (and the plugins only they reference) and delivers pending
    // latency and plugin-state notifications. Call it periodically from a message-thread timer.
    void collectGarbage();

    // ---- audio thread ------------------------------------------------------------------------

    // Never allocates, locks, logs or throws. Buffers with fewer than two channels, or calls
    // before prepare(), pass through; blocks longer than maxBlockSize are processed in chunks.
    void process(juce::AudioBuffer<float>& stereo, juce::MidiBuffer&) noexcept;

    // For blocks in which the caller skips the chain (global bypass): serves a pending
    // flushPluginParametersOnAudioThread() request for every slot, touches no audio.
    void processSkipped() noexcept;

private:
    struct Slot;
    struct Snapshot;

    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override;

    const Snapshot& snapshot() const noexcept;
    const Snapshot* acquire() noexcept;   // audio thread: sets the hazard pointer
    void serviceFlushRequest(const Snapshot&, bool everySlot) noexcept;
    void publish(std::unique_ptr<Snapshot>);
    void preparePlugin(juce::AudioPluginInstance&) const;
    std::shared_ptr<Slot> makeSlot(std::unique_ptr<juce::AudioPluginInstance>);
    void notifyChanged() const;

    double sampleRate_ = 0.0;
    std::atomic<int> maxBlockSize_{0};      // 0 = not prepared
    std::atomic<Snapshot*> current_;        // owned
    std::atomic<Snapshot*> inUse_{nullptr}; // hazard pointer set by process()
    std::vector<std::unique_ptr<Snapshot>> retired_;
    std::atomic<bool> latencyChangedPending_{false};
    std::atomic<bool> stateChangedPending_{false};
    juce::MidiBuffer emptyMidi_;            // MIDI for every chunk after the first
    std::atomic<std::uint32_t> flushRequested_{0};   // audio-thread flush requests, counted
    std::atomic<std::uint32_t> flushServed_{0};      // last request the audio thread served
    float flushScratch_[2] = {};
};

} // namespace fixomic
