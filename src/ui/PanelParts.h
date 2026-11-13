#pragma once

#include "ui/StatusModel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace fixomic::ui {

// The building blocks of MainPanel and its views: level meter and its scale, the route strip, glyph
// buttons, a notice row, a list of choices and a segmented control.

// A peak level bar on the -60..0 dBFS scale, ruled at -40 / -20 / -10 dB, drawn in the ink of the
// state line (the voice's colour). Empty while not live.
class LevelMeter : public juce::Component {
public:
    void setLevel(float peakDb);
    void setInk(StatusKind kind, bool live);
    void paint(juce::Graphics&) override;

    static constexpr float kTicksDb[] = {-40.0f, -20.0f, -10.0f};

private:
    float fraction_ = 0.0f;
    bool clipping_ = false;
    StatusKind kind_ = StatusKind::Waiting;
    bool live_ = false;
};

// The meter scale's numerals, aligned with LevelMeter's rules.
class MeterScale : public juce::Component {
public:
    void paint(juce::Graphics&) override;
};

// The signature: the voice's route Microfono -> Eco -> Catena -> fixomic Mic as a transit line in
// the state's ink; filled stations process, hollow ones are skipped, a barred one is closed.
class RouteStripView : public juce::Component {
public:
    RouteStripView();
    void setRoute(const RouteStrip&);
    void paint(juce::Graphics&) override;

    static constexpr int kHeight = 34;

private:
    RouteStrip route_;
};

// A small borderless button that shows a glyph in the secondary ink, with the flat button's hover
// fill: a chain row's remove (×), the plugin picker's back (‹). Its button text is its accessible
// name (not drawn).
class GlyphButton : public juce::Button {
public:
    enum class Glyph { Remove, Back };

    explicit GlyphButton(Glyph, const juce::String& accessibleName = {});
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;

    static constexpr int kSize = 22;

private:
    Glyph glyph_;
};

// A warning, tinted, with an optional button to the System Settings pane that fixes it ("Apri
// Impostazioni", only when the notice has a pane), or with an action of its own.
class NoticeRow : public juce::Component {
public:
    explicit NoticeRow(const Notice&, std::function<void(mac::PrivacyPane)> openPane);
    NoticeRow(const juce::String& text, const juce::String& buttonText, std::function<void()> onButton);
    int heightForWidth(int width) const;
    juce::Button* button();   // nullptr when it has none
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    TextBlock text_{TextStyle::Caption, TextBlock::Ink::Primary};
    juce::TextButton button_;
    bool hasButton_ = false;
};

// Exclusive choices one under the other (the microphones): a 28 pt row each, a check mark at the
// content edge on the chosen one, the text after it (secondary ink when dimmed), the flat button's
// hover fill. Rows are radio buttons for accessibility. A click is reported through onChoose once
// the click handler has returned (the owner may rebuild the list in response). Its bounds reach
// Theme::kFlatTextInset past the content edges, as ChainList's. Message thread only.
class ChoiceList : public juce::Component {
public:
    struct Item {
        juce::String text;
        bool selected = false;
        bool dimmed = false;
        bool operator==(const Item&) const = default;
    };
    static constexpr int kRowHeight = 28;
    static constexpr int kTextIndent = 20;   // the check's column

    explicit ChoiceList(const juce::String& accessibleTitle);
    ~ChoiceList() override;

    void setItems(std::vector<Item>);
    const std::vector<Item>& items() const;
    int heightForWidth(int width) const;
    juce::Button* row(int index) const;   // for tests

    std::function<void(int index)> onChoose;

    void resized() override;

private:
    class Row;
    void chooseSoon(int index);

    std::vector<Item> items_;
    std::vector<std::unique_ptr<Row>> rows_;
};

// Exclusive choices side by side (the buffer size, the input channel): equal segments on a track,
// at most kPerLine per line (more wrap onto the next), the chosen one raised in the push button's
// fill and edge, caption text. Segments are radio buttons for accessibility. A click is reported
// through onChoose once the click handler has returned. Message thread only.
class SegmentedControl : public juce::Component {
public:
    static constexpr int kSegmentHeight = 24;
    static constexpr int kLineGap = 4;
    static constexpr int kPerLine = 4;

    explicit SegmentedControl(const juce::String& accessibleTitle);
    ~SegmentedControl() override;

    void setOptions(const juce::StringArray& options, int selected);
    const juce::StringArray& options() const;
    int selected() const;
    int heightForWidth(int width) const;
    juce::Button* segment(int index) const;   // for tests

    std::function<void(int index)> onChoose;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Segment;
    void chooseSoon(int index);
    int lines() const;
    juce::Rectangle<int> segmentBounds(int index) const;

    juce::StringArray options_;
    int selected_ = -1;
    std::vector<std::unique_ptr<Segment>> segments_;
};

} // namespace fixomic::ui
