#pragma once

#include "dsp/PluginChain.h"
#include "ui/PanelParts.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace fixomic::ui {

// The chain under "Catena": one row per slot in signal order, then the "+ Aggiungi plugin" button.
// A row: a drag handle, the plugin's name (a click opens its editor), a secondary text (its latency
// when above 0, "In bypass", "Plugin non trovato"), its on / off switch (off = bypassed) and a remove
// button (no confirmation). A missing slot is dimmed, has no editor and no switch, and can still be
// removed. Without slots: an invitation line, then the button.
//
// Data in through setSlots(), intents out through the callbacks: the list never edits the chain.
// After it asked for an edit (move, bypass, remove) it ignores input until the next setSlots(), so a
// quick second click cannot act on an index the edit made stale; its owner re-reads the chain
// right after the edit (MainPanel does it asynchronously, outside the row's own click handler).
//
// Drag to reorder: press on a row (outside its buttons) and drag. The row recedes, a line in the
// text ink with a ring at the handle marks the gap it will drop into (no line where it would stay),
// and an enclosing Viewport scrolls when the pointer nears its edges.
//
// Its bounds reach Theme::kFlatTextInset past the content edges on both sides, so the flat button's
// text lines up with the panel's content; setSlots() sets its own height (heightForWidth()), and the
// owner follows through childBoundsChanged(). Message thread only.
class ChainList : public juce::Component {
public:
    static constexpr int kRowHeight = 32;

    ChainList();
    ~ChainList() override;

    void setSlots(std::vector<PluginChain::SlotInfo>);
    const std::vector<PluginChain::SlotInfo>& slots() const;
    int heightForWidth(int width) const;

    std::function<void(int from, int to)> onMove;          // PluginChain::move semantics
    std::function<void(int index, bool bypassed)> onBypassChanged;
    std::function<void(int index)> onOpenEditor;
    std::function<void(int index)> onRemove;
    std::function<void()> onAdd;                           // "+ Aggiungi plugin"

    // The drag in progress: the dragged row and the gap (0..size) it points at; nullopt when none.
    std::optional<int> draggedRow() const;
    std::optional<int> dropGap() const;
    juce::Rectangle<int> rowBounds(int index) const;      // in this component
    juce::Component* row(int index) const;                 // for tests
    juce::Button& addButton();

    void resized() override;
    void paintOverChildren(juce::Graphics&) override;

private:
    class Row;

    // From the rows (positions in this component).
    bool beginDrag(int index);
    void dragMoved(juce::Point<float>);
    void endDrag(juce::Point<float>);
    void cancelDrag();
    void bypassClicked(int index, bool bypassed);
    void removeClicked(int index);
    void openEditorClicked(int index);

    void fitHeight();
    juce::Rectangle<int> content() const;   // the bounds minus the flat button's inset

    std::vector<PluginChain::SlotInfo> slots_;
    std::vector<std::unique_ptr<Row>> rows_;
    TextBlock emptyText_{TextStyle::Caption, TextBlock::Ink::Secondary};
    juce::TextButton addButton_;
    bool awaitingSlots_ = false;   // an edit was asked for: wait for setSlots()
    int dragFrom_ = -1;
    int dragGap_ = -1;
};

} // namespace fixomic::ui
