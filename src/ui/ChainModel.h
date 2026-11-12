#pragma once

#include "dsp/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <vector>

namespace fixomic::ui {

// The chain editor's pure logic: the plugin picker's search and messages, the drag-to-reorder
// index maths and the chain rows' texts. No GUI, no AppController: everything comes in as values.

// ---- plugin picker ------------------------------------------------------------------------------

// `text` folded for matching: lower case, without accents or width variants ("Équaliseur" ->
// "equaliseur", also for decomposed accents).
juce::String foldForSearch(const juce::String& text);

// Every whitespace-separated word of `query` appears in the effect's name or manufacturer,
// ignoring case and accents. An empty (or blank) query matches every effect.
bool matchesSearch(const juce::PluginDescription& effect, const juce::String& query);

// The indices of the effects matching `query`, in their order.
std::vector<int> filterEffects(const std::vector<juce::PluginDescription>& effects, const juce::String& query);

// A picker row's secondary text: "iZotope · VST3", "Apple · AU" (AudioUnit shortened); the format
// alone without a manufacturer.
juce::String effectDetail(const juce::PluginDescription&);

// What the picker says about its list. `list` takes the list's place when it shows nothing ("" when
// there are matches); `status` is the line under it ("" when nothing is going on).
struct PickerMessages {
    juce::String list;
    juce::String status;
    bool operator==(const PickerMessages&) const = default;
};

// `effectCount`: effects in the catalog; `matchCount`: those matching the search.
PickerMessages pickerMessages(int effectCount, int matchCount, bool scanning);

// ---- drag to reorder ----------------------------------------------------------------------------

// The gap between rows that a drag at `y` points at, for `count` rows of `rowHeight` starting at
// y = 0: 0 is above the first row, `count` below the last. The nearest row boundary, clamped.
int insertionGap(float y, float rowHeight, int count);

// The `to` of PluginChain::move(from, to) that puts row `from` into `gap` (0..count, clamped). A row
// dragged down leaves its old place, so `to` is one less than the gap. nullopt when the row would
// stay where it is (the gaps just above and below it) or `from` is out of range.
std::optional<int> moveTarget(int from, int gap, int count);

// ---- chain rows ---------------------------------------------------------------------------------

inline constexpr double kChainSampleRate = 48000.0;   // the engine's only rate

// The chain's latency: the sum over the slots that process (neither missing nor bypassed), as
// PluginChain::totalLatencySamples() counts it.
int chainLatencySamples(const std::vector<PluginChain::SlotInfo>&);

// A plugin's latency, "1,0 ms": "" when it reports none (0 or less); "< 0,1 ms" when it would
// round to zero.
juce::String formatSlotLatency(int latencySamples, double sampleRate = kChainSampleRate);

// A chain row's secondary text: "Plugin non trovato" for a missing slot, "In bypass" for a
// bypassed one (it adds no latency), else its latency (formatSlotLatency).
juce::String slotDetail(const PluginChain::SlotInfo&, double sampleRate = kChainSampleRate);

} // namespace fixomic::ui
