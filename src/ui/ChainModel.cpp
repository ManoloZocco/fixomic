#include "ui/ChainModel.h"

#include "ui/StatusModel.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

juce::StringArray searchWords(const juce::String& query) {
    juce::StringArray words;
    words.addTokens(foldForSearch(query), " \t\r\n", "");
    words.removeEmptyStrings();
    return words;
}

bool containsEvery(const juce::String& haystack, const juce::StringArray& words) {
    return std::all_of(words.begin(), words.end(), [&](const juce::String& word) { return haystack.contains(word); });
}

juce::String searchableText(const juce::PluginDescription& effect) {
    return foldForSearch(effect.name) + "\n" + foldForSearch(effect.manufacturerName);
}

} // namespace

// ---- plugin picker ------------------------------------------------------------------------------

juce::String foldForSearch(const juce::String& text) {
    if (text.isEmpty()) return {};
    // CFStringFold is what NSString's folding uses: case, diacritics (precomposed or combining) and
    // width; no locale, so the result does not depend on the user's language.
    const CFStringRef source = text.toCFString();
    CFMutableStringRef folded = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
    CFRelease(source);
    if (folded == nullptr) return text.toLowerCase();
    CFStringFold(folded, kCFCompareCaseInsensitive | kCFCompareDiacriticInsensitive | kCFCompareWidthInsensitive,
                 nullptr);
    const auto result = juce::String::fromCFString(folded);
    CFRelease(folded);
    return result;
}

bool matchesSearch(const juce::PluginDescription& effect, const juce::String& query) {
    return containsEvery(searchableText(effect), searchWords(query));
}

std::vector<int> filterEffects(const std::vector<juce::PluginDescription>& effects, const juce::String& query) {
    const auto words = searchWords(query);   // folded once, not per effect
    std::vector<int> matches;
    for (size_t i = 0; i < effects.size(); ++i)
        if (containsEvery(searchableText(effects[i]), words)) matches.push_back(static_cast<int>(i));
    return matches;
}

juce::String effectDetail(const juce::PluginDescription& effect) {
    const juce::String format = effect.pluginFormatName == "AudioUnit" ? juce::String("AU") : effect.pluginFormatName;
    if (effect.manufacturerName.isEmpty()) return format;
    return effect.manufacturerName + utf8(" · ") + format;
}

PickerMessages pickerMessages(int effectCount, int matchCount, bool scanning) {
    const auto scanText = utf8("Scansione in corso…");
    PickerMessages messages;
    if (effectCount <= 0) {
        // An empty catalog: the list area says it all, the scan once.
        messages.list = scanning ? scanText
                                 : utf8("Nessun plugin nell’elenco: usa «Cerca nuovi plugin» per "
                                        "trovare quelli installati.");
        return messages;
    }
    if (matchCount <= 0) messages.list = "Nessun plugin trovato";
    if (scanning) messages.status = scanText;
    return messages;
}

// ---- drag to reorder ----------------------------------------------------------------------------

int insertionGap(float y, float rowHeight, int count) {
    if (count <= 0 || !(rowHeight > 0.0f)) return 0;
    const int gap = static_cast<int>(std::floor(y / rowHeight + 0.5f));
    return std::clamp(gap, 0, count);
}

std::optional<int> moveTarget(int from, int gap, int count) {
    if (from < 0 || from >= count) return std::nullopt;
    gap = std::clamp(gap, 0, count);
    const int to = gap > from ? gap - 1 : gap;
    if (to == from) return std::nullopt;
    return to;
}

// ---- chain rows ---------------------------------------------------------------------------------

juce::String formatSlotLatency(int latencySamples, double sampleRate) {
    if (latencySamples <= 0 || !(sampleRate > 0.0)) return {};
    const double ms = static_cast<double>(latencySamples) / sampleRate * 1000.0;
    if (ms < 0.05) return utf8("< ") + formatMs(0.1);   // above zero, never "0,0 ms"
    return formatMs(ms);
}

juce::String slotDetail(const PluginChain::SlotInfo& slot, double sampleRate) {
    if (slot.missing) return "Plugin non trovato";
    if (slot.bypassed) return "In bypass";
    return formatSlotLatency(slot.latencySamples, sampleRate);
}

} // namespace fixomic::ui
