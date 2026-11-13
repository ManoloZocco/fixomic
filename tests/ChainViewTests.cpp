#include <catch2/catch_test_macros.hpp>

#include "ui/ChainList.h"
#include "ui/PluginPicker.h"

#include <optional>
#include <utility>
#include <vector>

using fixomic::PluginChain;
using namespace fixomic::ui;

// The chain section's views, driven like the panel drives them: data in, intents out. Mouse events
// are synthesised and delivered to the row directly (no window, no event loop).

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int kListWidth = 344;   // MainPanel's content width plus the flat buttons' inset each side

juce::MouseEvent mouseAt(juce::Component& target, juce::Point<float> position, juce::Point<float> downAt,
                         bool dragged) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
                            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &target, &target, now, downAt, now, 1, dragged);
}

// Press on row `index` at `x`, drag to `toY` (in the list), release.
void dragRow(ChainList& list, int index, float x, float toY) {
    auto& row = *list.row(index);
    const juce::Point<float> down{x, ChainList::kRowHeight / 2.0f};
    const juce::Point<float> to{x, toY - static_cast<float>(row.getY())};
    row.mouseDown(mouseAt(row, down, down, false));
    row.mouseDrag(mouseAt(row, {x, down.y + 6.0f}, down, true));
    row.mouseDrag(mouseAt(row, to, down, true));
    row.mouseUp(mouseAt(row, to, down, true));
}

// Press on row `index`, drag to `to` (in the list), release there.
void dragRowTo(ChainList& list, int index, juce::Point<float> to) {
    auto& row = *list.row(index);
    const juce::Point<float> down{30.0f, ChainList::kRowHeight / 2.0f};
    const auto inRow = to - row.getPosition().toFloat();
    row.mouseDown(mouseAt(row, down, down, false));
    row.mouseDrag(mouseAt(row, {down.x, down.y + 6.0f}, down, true));
    row.mouseDrag(mouseAt(row, inRow, down, true));
    row.mouseUp(mouseAt(row, inRow, down, true));
}

void clickRow(ChainList& list, int index, float x) {
    auto& row = *list.row(index);
    const juce::Point<float> at{x, ChainList::kRowHeight / 2.0f};
    row.mouseDown(mouseAt(row, at, at, false));
    row.mouseUp(mouseAt(row, at, at, false));
}

template <typename T>
T* childOf(juce::Component& parent, const juce::String& textPrefix) {
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<T*>(child); button != nullptr && button->getButtonText().startsWith(textPrefix))
            return button;
    return nullptr;
}

struct Recorder {
    std::vector<std::pair<int, int>> moves;
    std::vector<std::pair<int, bool>> bypasses;
    std::vector<int> editors, removals;
    int adds = 0;

    void attach(ChainList& list) {
        list.onMove = [this](int from, int to) { moves.emplace_back(from, to); };
        list.onBypassChanged = [this](int index, bool bypassed) { bypasses.emplace_back(index, bypassed); };
        list.onOpenEditor = [this](int index) { editors.push_back(index); };
        list.onRemove = [this](int index) { removals.push_back(index); };
        list.onAdd = [this] { ++adds; };
    }
};

std::vector<PluginChain::SlotInfo> threeSlots() {
    return {{"Nectar 4 Gate", false, false, 48}, {"AUNBandEQ", true, false, 0}, {"Old EQ", false, true, 0}};
}

juce::PluginDescription effect(const char* name, const char* manufacturer, const char* format = "VST3") {
    juce::PluginDescription d;
    d.name = utf8(name);
    d.manufacturerName = utf8(manufacturer);
    d.pluginFormatName = format;
    d.fileOrIdentifier = juce::String("/Library/Audio/Plug-Ins/") + name;
    return d;
}

} // namespace

// ---- ChainList --------------------------------------------------------------------------------

TEST_CASE("ChainList shows one row per slot, then the add button; an invitation when empty", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    ChainList list;
    list.setSize(kListWidth, 10);
    Recorder recorder;
    recorder.attach(list);

    list.setSlots(threeSlots());
    REQUIRE(list.getHeight() == list.heightForWidth(kListWidth));
    REQUIRE(list.heightForWidth(kListWidth) == 3 * ChainList::kRowHeight + 2 + 28);
    for (int i = 0; i < 3; ++i) REQUIRE(list.row(i)->getBounds() == list.rowBounds(i));
    REQUIRE(list.row(3) == nullptr);
    REQUIRE(list.addButton().getY() >= list.rowBounds(2).getBottom());

    // The missing slot keeps its remove button and has no switch.
    REQUIRE(childOf<juce::ToggleButton>(*list.row(0), "Attiva")->isVisible());
    REQUIRE_FALSE(childOf<juce::ToggleButton>(*list.row(2), "Attiva")->isVisible());
    REQUIRE(childOf<GlyphButton>(*list.row(2), "Rimuovi Old EQ")->isVisible());

    list.setSlots({});
    REQUIRE(list.row(0) == nullptr);
    REQUIRE(list.heightForWidth(kListWidth) > 28);   // the invitation line, then the button
    list.addButton().onClick();
    REQUIRE(recorder.adds == 1);
}

TEST_CASE("ChainList turns a drop into movePlugin(from, to)", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    ChainList list;
    list.setSize(kListWidth, 10);
    Recorder recorder;
    recorder.attach(list);
    list.setSlots(threeSlots());

    SECTION("down past itself, to the end") {
        dragRow(list, 0, 30.0f, 3.0f * ChainList::kRowHeight - 4.0f);
        REQUIRE(recorder.moves == std::vector<std::pair<int, int>>{{0, 2}});
        REQUIRE_FALSE(list.draggedRow().has_value());
        REQUIRE(recorder.editors.empty());   // a drag that ends on the name is not a click
    }
    SECTION("up, between the first two rows") {
        dragRow(list, 2, 6.0f, ChainList::kRowHeight + 3.0f);
        REQUIRE(recorder.moves == std::vector<std::pair<int, int>>{{2, 1}});
    }
    SECTION("dropped next to itself: nothing moves") {
        dragRow(list, 1, 30.0f, ChainList::kRowHeight * 2.0f + 4.0f);   // the gap under itself
        REQUIRE(recorder.moves.empty());
    }
    SECTION("the drag in progress: the dragged row and the gap it points at") {
        auto& row = *list.row(0);
        const juce::Point<float> down{30.0f, 16.0f};
        row.mouseDown(mouseAt(row, down, down, false));
        row.mouseDrag(mouseAt(row, {30.0f, 18.0f}, down, true));   // under the threshold
        REQUIRE_FALSE(list.draggedRow().has_value());
        row.mouseDrag(mouseAt(row, {30.0f, 70.0f}, down, true));
        REQUIRE(list.draggedRow() == 0);
        REQUIRE(list.dropGap() == 2);
        REQUIRE(row.getAlpha() < 1.0f);
        // A chain change mid-drag cancels it, for the rest of this press: the row may now show
        // another slot, so moving on does not start a new drag.
        auto changed = threeSlots();
        std::swap(changed[0], changed[1]);
        list.setSlots(changed);
        REQUIRE_FALSE(list.draggedRow().has_value());
        REQUIRE(row.getAlpha() == 1.0f);
        row.mouseDrag(mouseAt(row, {30.0f, 80.0f}, down, true));
        REQUIRE_FALSE(list.draggedRow().has_value());
        row.mouseUp(mouseAt(row, {30.0f, 90.0f}, down, true));
        REQUIRE(recorder.moves.empty());
        REQUIRE(recorder.editors.empty());

        // The next press drags again.
        dragRow(list, 0, 30.0f, 3.0f * ChainList::kRowHeight - 4.0f);
        REQUIRE(recorder.moves == std::vector<std::pair<int, int>>{{0, 2}});
    }
    SECTION("a chain change before the drag began cancels the press too") {
        // Pressed on the name, the chain changes under the threshold: the row may now show another
        // slot, so neither a drag nor a click may follow from this press.
        auto& row = *list.row(0);
        const juce::Point<float> down{30.0f, 16.0f};
        row.mouseDown(mouseAt(row, down, down, false));
        auto changed = threeSlots();
        std::swap(changed[0], changed[1]);
        list.setSlots(changed);
        row.mouseDrag(mouseAt(row, {30.0f, 70.0f}, down, true));   // past the threshold
        REQUIRE_FALSE(list.draggedRow().has_value());
        row.mouseUp(mouseAt(row, {30.0f, 70.0f}, down, true));
        REQUIRE(recorder.moves.empty());
        REQUIRE(recorder.editors.empty());

        // A press released without moving, on the name, after a change: no editor either.
        row.mouseDown(mouseAt(row, down, down, false));
        list.setSlots(threeSlots());
        row.mouseUp(mouseAt(row, down, down, false));
        REQUIRE(recorder.editors.empty());

        // The next press works.
        dragRow(list, 0, 30.0f, 3.0f * ChainList::kRowHeight - 4.0f);
        REQUIRE(recorder.moves == std::vector<std::pair<int, int>>{{0, 2}});
    }
    SECTION("released away from the list: cancelled, nothing moves") {
        const float rows = 3.0f * ChainList::kRowHeight;
        const float listHeight = static_cast<float>(list.getHeight());
        dragRowTo(list, 2, {100.0f, -ChainList::kRowHeight - 2.0f});   // more than a row above
        dragRowTo(list, 0, {100.0f, listHeight + ChainList::kRowHeight + 2.0f});   // more than a row below
        dragRowTo(list, 0, {-12.0f, rows - 4.0f});                       // past the panel's left edge
        dragRowTo(list, 0, {kListWidth + 12.0f, rows - 4.0f});           // past its right edge
        REQUIRE(recorder.moves.empty());
    }
    SECTION("away from the list the drop line goes; near it, the drop is clamped") {
        auto& row = *list.row(2);
        const juce::Point<float> down{30.0f, 16.0f};
        row.mouseDown(mouseAt(row, down, down, false));
        row.mouseDrag(mouseAt(row, {30.0f, 8.0f}, down, true));
        const auto above = juce::Point<float>{100.0f, -ChainList::kRowHeight - 2.0f} - row.getPosition().toFloat();
        row.mouseDrag(mouseAt(row, above, down, true));
        REQUIRE(list.draggedRow() == 2);
        REQUIRE_FALSE(list.dropGap().has_value());
        const auto near = juce::Point<float>{100.0f, -10.0f} - row.getPosition().toFloat();   // within a row
        row.mouseDrag(mouseAt(row, near, down, true));
        REQUIRE(list.dropGap() == 0);
        row.mouseUp(mouseAt(row, near, down, true));
        REQUIRE(recorder.moves == std::vector<std::pair<int, int>>{{2, 0}});
    }
    SECTION("cancelDrag() stops a drag in progress and says whether there was one") {
        REQUIRE_FALSE(list.cancelDrag());
        auto& row = *list.row(0);
        const juce::Point<float> down{30.0f, 16.0f};
        row.mouseDown(mouseAt(row, down, down, false));
        row.mouseDrag(mouseAt(row, {30.0f, 70.0f}, down, true));
        REQUIRE(list.draggedRow() == 0);
        REQUIRE(list.cancelDrag());
        REQUIRE_FALSE(list.draggedRow().has_value());
        row.mouseDrag(mouseAt(row, {30.0f, 90.0f}, down, true));
        row.mouseUp(mouseAt(row, {30.0f, 90.0f}, down, true));
        REQUIRE(recorder.moves.empty());
    }
}

TEST_CASE("ChainList rows: editor on the name, bypass switch, remove", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    ChainList list;
    list.setSize(kListWidth, 10);
    Recorder recorder;
    recorder.attach(list);
    list.setSlots(threeSlots());

    clickRow(list, 0, 30.0f);    // on the name
    clickRow(list, 0, 200.0f);   // past it: nothing
    clickRow(list, 2, 30.0f);    // a missing slot has no editor
    REQUIRE(recorder.editors == std::vector<int>{0});

    // The switch is "on" while the plugin processes; turning it off bypasses.
    auto* on = childOf<juce::ToggleButton>(*list.row(0), "Attiva Nectar 4 Gate");
    REQUIRE(on->getToggleState());
    REQUIRE_FALSE(childOf<juce::ToggleButton>(*list.row(1), "Attiva")->getToggleState());
    on->setToggleState(false, juce::dontSendNotification);   // what a click does, then onClick
    on->onClick();
    REQUIRE(recorder.bypasses == std::vector<std::pair<int, bool>>{{0, true}});

    SECTION("after an edit, input waits for the re-read chain") {
        childOf<GlyphButton>(*list.row(2), "Rimuovi")->onClick();
        dragRow(list, 1, 30.0f, 0.0f);
        REQUIRE(recorder.removals.empty());
        REQUIRE(recorder.moves.empty());
        REQUIRE(on->getToggleState() == false);

        list.setSlots(threeSlots());   // the owner re-read the chain (here: unchanged)
        childOf<GlyphButton>(*list.row(2), "Rimuovi")->onClick();
        REQUIRE(recorder.removals == std::vector<int>{2});
    }
    SECTION("an ignored switch click goes back to the slot's state") {
        auto* eq = childOf<juce::ToggleButton>(*list.row(1), "Attiva");
        eq->setToggleState(true, juce::dontSendNotification);
        eq->onClick();   // ignored: the bypass above is still being applied
        REQUIRE(recorder.bypasses.size() == 1);
        REQUIRE_FALSE(eq->getToggleState());
    }
}

// ---- PluginPicker -----------------------------------------------------------------------------

TEST_CASE("PluginPicker filters as the user types and chooses the listed entry", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    PluginPicker picker;
    picker.setSize(kListWidth, 300);
    picker.setEffects({effect("AUNBandEQ", "Apple", "AudioUnit"), effect("Nectar 4 Gate", "iZotope"),
                       effect(utf8("Égaliseur").toRawUTF8(), "Studio")});
    REQUIRE(picker.listedEffects().size() == 3);
    REQUIRE(picker.selectedIndex() == 0);
    REQUIRE(picker.listMessage().isEmpty());

    picker.setSearchText(utf8("NÈCT"));
    REQUIRE(picker.listedEffects().size() == 1);
    REQUIRE(picker.listedEffects()[0].name == "Nectar 4 Gate");
    REQUIRE(picker.selectedIndex() == 0);

    picker.setSearchText("egal");
    REQUIRE(picker.listedEffects().size() == 1);

    picker.setSearchText("reverb");
    REQUIRE(picker.listedEffects().empty());
    REQUIRE(picker.selectedIndex() == -1);
    REQUIRE(picker.listMessage() == "Nessun plugin trovato");

    picker.setSearchText("izotope");
    std::optional<juce::String> chosen;
    picker.onChoose = [&](const juce::PluginDescription& d) {
        chosen = d.name;
        return juce::String();
    };
    picker.choose(0);
    REQUIRE(chosen == "Nectar 4 Gate");
    REQUIRE_FALSE(picker.showsError());
}

TEST_CASE("PluginPicker shows an add error inline until the search changes", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    PluginPicker picker;
    picker.setSize(kListWidth, 300);
    picker.setEffects({effect("Nectar 4 Gate", "iZotope")});
    const int height = picker.getHeight();
    const auto error = juce::String("Impossibile caricare il plugin \"Nectar 4 Gate\": the plugin crashed while "
                                    "loading and could not be instantiated, try rescanning it");
    picker.onChoose = [&](const juce::PluginDescription&) { return error; };

    picker.choose(0);
    REQUIRE(picker.showsError());
    REQUIRE(picker.statusText() == error);
    REQUIRE(picker.getHeight() >= height);   // the status line grows with it

    picker.choose(5);   // out of range: nothing happens
    REQUIRE(picker.statusText() == error);

    picker.setSearchText("nec");
    REQUIRE_FALSE(picker.showsError());
    REQUIRE(picker.statusText().isEmpty());
    REQUIRE(picker.getHeight() == height);
}

TEST_CASE("PluginPicker: an empty catalog invites a scan; a scan in progress says so", "[ui][chainview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    PluginPicker picker;
    picker.setSize(kListWidth, 300);
    int rescans = 0, closes = 0;
    picker.onRescan = [&] { ++rescans; };
    picker.onClose = [&] { ++closes; };

    REQUIRE(picker.listMessage().contains(utf8("«Cerca nuovi plugin»")));
    picker.setScanning(true);
    REQUIRE(picker.listMessage() == utf8("Scansione in corso…"));
    REQUIRE(picker.statusText().isEmpty());

    picker.setEffects({effect("Nectar 4 Gate", "iZotope")});
    REQUIRE(picker.listMessage().isEmpty());
    REQUIRE(picker.statusText() == utf8("Scansione in corso…"));
    picker.setScanning(false);
    REQUIRE(picker.statusText().isEmpty());

    REQUIRE(picker.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    REQUIRE(closes == 1);
    REQUIRE(rescans == 0);

    picker.setSearchText("zzz");
    picker.reset();
    REQUIRE(picker.listedEffects().size() == 1);
    REQUIRE(picker.selectedIndex() == 0);
}
