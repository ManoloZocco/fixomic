#include <catch2/catch_test_macros.hpp>

#include "ui/ChainModel.h"

#include <optional>
#include <vector>

using fixomic::PluginChain;
using namespace fixomic::ui;

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

juce::PluginDescription effect(const char* name, const char* manufacturer, const char* format = "VST3") {
    juce::PluginDescription d;
    d.name = utf8(name);
    d.manufacturerName = utf8(manufacturer);
    d.pluginFormatName = format;
    return d;
}

// PluginChain::move(from, to): take the slot out, insert it at `to`.
std::vector<char> applyMove(std::vector<char> slots, int from, int to) {
    const char moved = slots[static_cast<size_t>(from)];
    slots.erase(slots.begin() + from);
    slots.insert(slots.begin() + to, moved);
    return slots;
}

// The order after dropping row `from` into `gap` (unchanged when there is nothing to move).
std::vector<char> drop(const std::vector<char>& slots, int from, int gap) {
    const auto to = moveTarget(from, gap, static_cast<int>(slots.size()));
    return to ? applyMove(slots, from, *to) : slots;
}

} // namespace

// ---- picker search ----------------------------------------------------------------------------

TEST_CASE("foldForSearch ignores case and accents", "[ui][chainmodel]") {
    REQUIRE(foldForSearch("NECTAR 4 Gate") == "nectar 4 gate");
    REQUIRE(foldForSearch(utf8("Équaliseur à l’ancienne")) == utf8("equaliseur a l’ancienne"));
    REQUIRE(foldForSearch(utf8("Riù Réverb Über")) == "riu reverb uber");
    REQUIRE(foldForSearch(utf8("é")) == "e");   // decomposed accent
    REQUIRE(foldForSearch({}).isEmpty());
}

TEST_CASE("matchesSearch looks for every word in the name or the manufacturer", "[ui][chainmodel]") {
    const auto gate = effect("Nectar 4 Gate", "iZotope");

    REQUIRE(matchesSearch(gate, {}));
    REQUIRE(matchesSearch(gate, "   "));
    REQUIRE(matchesSearch(gate, "nectar"));
    REQUIRE(matchesSearch(gate, "GATE"));
    REQUIRE(matchesSearch(gate, "ect"));                       // anywhere in a word
    REQUIRE(matchesSearch(gate, "izotope"));                   // the manufacturer
    REQUIRE(matchesSearch(gate, utf8("  nèct  Gâte ")));   // accents, spaces, word order
    REQUIRE(matchesSearch(gate, "izotope gate"));              // one word from each
    REQUIRE_FALSE(matchesSearch(gate, "reverb"));
    REQUIRE_FALSE(matchesSearch(gate, "gate reverb"));         // every word must match
    REQUIRE_FALSE(matchesSearch(gate, "vst3"));                // not the format

    REQUIRE(matchesSearch(effect("Égaliseur", "Studio À"), "egaliseur studio a"));
}

TEST_CASE("filterEffects keeps the matching effects in their order", "[ui][chainmodel]") {
    const std::vector<juce::PluginDescription> effects{effect("AUGraphicEQ", "Apple", "AudioUnit"),
                                                       effect("Nectar 4 Gate", "iZotope"),
                                                       effect("AUNBandEQ", "Apple", "AudioUnit"),
                                                       effect("Pro-Q 3", "FabFilter")};
    REQUIRE(filterEffects(effects, {}) == std::vector<int>{0, 1, 2, 3});
    REQUIRE(filterEffects(effects, "eq") == std::vector<int>{0, 2});
    REQUIRE(filterEffects(effects, "apple band") == std::vector<int>{2});
    REQUIRE(filterEffects(effects, "fabfilter") == std::vector<int>{3});
    REQUIRE(filterEffects(effects, "chorus").empty());
    REQUIRE(filterEffects({}, "eq").empty());
}

TEST_CASE("effectDetail names the manufacturer and a short format", "[ui][chainmodel]") {
    REQUIRE(effectDetail(effect("Nectar 4 Gate", "iZotope")) == utf8("iZotope · VST3"));
    REQUIRE(effectDetail(effect("AUNBandEQ", "Apple", "AudioUnit")) == utf8("Apple · AU"));
    REQUIRE(effectDetail(effect("Mystery", "", "AudioUnit")) == "AU");
    REQUIRE(effectDetail(effect("Odd", "Someone", "LV2")) == utf8("Someone · LV2"));
}

TEST_CASE("pickerMessages: no results, no effects at all, a scan in progress", "[ui][chainmodel]") {
    const auto scanning = utf8("Scansione in corso…");

    REQUIRE(pickerMessages(12, 3, false) == PickerMessages{});
    REQUIRE(pickerMessages(12, 0, false) == PickerMessages{"Nessun plugin trovato", {}});
    REQUIRE(pickerMessages(12, 3, true) == PickerMessages{{}, scanning});
    REQUIRE(pickerMessages(12, 0, true) == PickerMessages{"Nessun plugin trovato", scanning});

    // An empty catalog invites a scan; while one runs, it says so once.
    const auto empty = pickerMessages(0, 0, false);
    REQUIRE(empty.list.contains(utf8("«Cerca nuovi plugin»")));
    REQUIRE(empty.status.isEmpty());
    REQUIRE(pickerMessages(0, 0, true) == PickerMessages{scanning, {}});
}

// ---- reorder ----------------------------------------------------------------------------------

TEST_CASE("insertionGap is the row boundary nearest to the pointer", "[ui][chainmodel]") {
    // Three rows of 32 pt: boundaries at 0, 32, 64, 96.
    REQUIRE(insertionGap(-40.0f, 32.0f, 3) == 0);
    REQUIRE(insertionGap(0.0f, 32.0f, 3) == 0);
    REQUIRE(insertionGap(15.9f, 32.0f, 3) == 0);
    REQUIRE(insertionGap(16.0f, 32.0f, 3) == 1);
    REQUIRE(insertionGap(47.0f, 32.0f, 3) == 1);
    REQUIRE(insertionGap(49.0f, 32.0f, 3) == 2);
    REQUIRE(insertionGap(90.0f, 32.0f, 3) == 3);
    REQUIRE(insertionGap(500.0f, 32.0f, 3) == 3);
    REQUIRE(insertionGap(10.0f, 32.0f, 0) == 0);
    REQUIRE(insertionGap(10.0f, 0.0f, 3) == 0);   // degenerate rows
}

TEST_CASE("moveTarget turns a drop gap into movePlugin(from, to)", "[ui][chainmodel]") {
    SECTION("the gaps around the dragged row move nothing") {
        REQUIRE_FALSE(moveTarget(0, 0, 3).has_value());
        REQUIRE_FALSE(moveTarget(0, 1, 3).has_value());
        REQUIRE_FALSE(moveTarget(1, 1, 3).has_value());
        REQUIRE_FALSE(moveTarget(1, 2, 3).has_value());
        REQUIRE_FALSE(moveTarget(2, 3, 3).has_value());
    }
    SECTION("dragging up: the row takes the gap's index") {
        REQUIRE(moveTarget(2, 0, 3) == 0);
        REQUIRE(moveTarget(2, 1, 3) == 1);
        REQUIRE(moveTarget(1, 0, 3) == 0);
    }
    SECTION("dragging down past itself: one less, the row left its old place") {
        REQUIRE(moveTarget(0, 2, 3) == 1);
        REQUIRE(moveTarget(0, 3, 3) == 2);
        REQUIRE(moveTarget(1, 3, 3) == 2);
    }
    SECTION("out of range") {
        REQUIRE_FALSE(moveTarget(-1, 0, 3).has_value());
        REQUIRE_FALSE(moveTarget(3, 0, 3).has_value());
        REQUIRE_FALSE(moveTarget(0, 0, 0).has_value());
        REQUIRE(moveTarget(0, 99, 3) == 2);    // past the end: to the end
        REQUIRE(moveTarget(2, -5, 3) == 0);    // before the start: to the start
    }
    SECTION("the dragged row lands in the gap it was dropped into") {
        const std::vector<char> abc{'a', 'b', 'c'};
        REQUIRE(drop(abc, 0, 2) == std::vector<char>{'b', 'a', 'c'});
        REQUIRE(drop(abc, 0, 3) == std::vector<char>{'b', 'c', 'a'});
        REQUIRE(drop(abc, 2, 0) == std::vector<char>{'c', 'a', 'b'});
        REQUIRE(drop(abc, 2, 1) == std::vector<char>{'a', 'c', 'b'});
        REQUIRE(drop(abc, 1, 0) == std::vector<char>{'b', 'a', 'c'});
        REQUIRE(drop(abc, 1, 3) == std::vector<char>{'a', 'c', 'b'});
        for (int from = 0; from < 3; ++from)
            for (int gap = 0; gap <= 3; ++gap) {
                // Wherever it lands, the dragged row is just below the rows that were above the gap.
                const auto after = drop(abc, from, gap);
                const int above = gap - (from < gap ? 1 : 0);
                REQUIRE(after[static_cast<size_t>(above)] == abc[static_cast<size_t>(from)]);
            }
    }
}

// ---- rows -------------------------------------------------------------------------------------

TEST_CASE("formatSlotLatency shows a plugin's latency in ms at 48 kHz, only above 0", "[ui][chainmodel]") {
    REQUIRE(kChainSampleRate == 48000.0);
    REQUIRE(formatSlotLatency(0).isEmpty());
    REQUIRE(formatSlotLatency(-12).isEmpty());
    REQUIRE(formatSlotLatency(48) == "1,0 ms");
    REQUIRE(formatSlotLatency(1024) == "21,3 ms");
    REQUIRE(formatSlotLatency(5) == "0,1 ms");
    REQUIRE(formatSlotLatency(1) == "< 0,1 ms");   // above 0, never "0,0 ms"
    REQUIRE(formatSlotLatency(441, 44100.0) == "10,0 ms");
}

TEST_CASE("slotDetail: missing, then bypassed, then latency", "[ui][chainmodel]") {
    REQUIRE(slotDetail(PluginChain::SlotInfo{"Nectar 4 Gate", false, false, 48}) == "1,0 ms");
    REQUIRE(slotDetail(PluginChain::SlotInfo{"Nectar 4 Gate", false, false, 0}).isEmpty());
    // Bypassed: it adds no latency, so its latency is not shown.
    REQUIRE(slotDetail(PluginChain::SlotInfo{"Nectar 4 Gate", true, false, 48}) == "In bypass");
    REQUIRE(slotDetail(PluginChain::SlotInfo{"Old EQ", false, true, 0}) == "Plugin non trovato");
    REQUIRE(slotDetail(PluginChain::SlotInfo{"Old EQ", true, true, 0}) == "Plugin non trovato");
}

TEST_CASE("chainLatencySamples sums the slots that process", "[ui][chainmodel]") {
    REQUIRE(chainLatencySamples({}) == 0);
    REQUIRE(chainLatencySamples({{"Gate", false, false, 48}, {"EQ", false, false, 64}}) == 112);
    // Bypassed and missing slots add nothing, whatever they report.
    REQUIRE(chainLatencySamples({{"Gate", false, false, 48}, {"EQ", true, false, 64}, {"Old", false, true, 32}}) == 48);
}
