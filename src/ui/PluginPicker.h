#pragma once

#include "ui/PanelParts.h"
#include "ui/Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace fixomic::ui {

// "+ Aggiungi plugin": an in-place view that takes the chain's place in the panel (never a window).
// A header with back (‹) and "Aggiungi plugin" on the left and "Cerca nuovi plugin" (a rescan) on the
// right; a search field (name and manufacturer, ignoring case and accents); the installed effects
// that match, each with its manufacturer and format; under them a status line: "Scansione in
// corso…" while the catalog is scanned, or why the last choice could not be added. The list's place
// says "Nessun plugin trovato" when the search matches nothing, and invites a scan when the catalog
// is empty.
//
// Data in through setEffects() / setScanning(), intents out through the callbacks. A click on an
// entry (or Return in the search field, for the selected one) chooses it, once the click handler
// has returned; onChoose answers "" (added: the owner closes the picker) or the error to show.
// Up / Down in the search field move the selection; Escape and back call onClose.
//
// Its bounds reach Theme::kFlatTextInset past the content edges, as ChainList's; its height changes
// only with the status line (heightForWidth()), and it sets it itself. Message thread only.
class PluginPicker : public juce::Component, private juce::ListBoxModel, private juce::KeyListener {
public:
    static constexpr int kRowHeight = 28;
    static constexpr float kVisibleRows = 6.5f;   // a cut row says the list scrolls

    PluginPicker();
    ~PluginPicker() override;

    // The catalog's effects. The search, and the selection while its entry is still listed, stay.
    void setEffects(std::vector<juce::PluginDescription>);
    void setScanning(bool);
    void setSearchText(const juce::String&);   // as if typed
    // Back to the start: no search, no error, the first entry selected, scrolled to the top.
    void reset();
    void focusSearch();                        // when on screen
    int heightForWidth(int width) const;

    std::function<juce::String(const juce::PluginDescription&)> onChoose;
    std::function<void()> onRescan;            // "Cerca nuovi plugin"
    std::function<void()> onClose;             // back, Escape

    // Chooses the `index`-th listed entry now (what a click does, once its handler has returned).
    void choose(int index);

    // State, for tests and the snapshot.
    std::vector<juce::PluginDescription> listedEffects() const;
    int selectedIndex() const;
    juce::String listMessage() const;          // shown in the list's place; "" while it lists something
    juce::String statusText() const;           // the line under the list: scan or error
    bool showsError() const;

    void resized() override;
    void paintOverChildren(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void lookAndFeelChanged() override;

private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int lastRowSelected) override;
    juce::String getNameForRow(int row) override;
    // KeyListener (the search field: Up / Down / Return / Escape before the editor sees them)
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;

    void chooseSoon(int index);
    void chooseEffect(const juce::PluginDescription&);
    void searchChanged();
    void refilter(const juce::String& selectIdentifier);   // then selects it, else the first entry
    void updateMessages();
    void fitHeight();
    void applyColours();
    int listHeight() const;

    std::vector<juce::PluginDescription> effects_;
    std::vector<int> listed_;          // indices into effects_
    bool scanning_ = false;
    bool choosing_ = false;            // a click's choice is on its way
    juce::String error_;

    GlyphButton back_{GlyphButton::Glyph::Back, "Torna alla catena"};
    TextBlock title_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    juce::TextButton rescan_;
    juce::TextEditor search_;
    juce::ListBox list_;
    TextBlock message_{TextStyle::Caption, TextBlock::Ink::Secondary, juce::Justification::centredTop};
    TextBlock status_{TextStyle::Caption, TextBlock::Ink::Secondary};
};

} // namespace fixomic::ui
