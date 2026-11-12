#pragma once

#include "ui/StatusModel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace fixomic::ui {

// The building blocks of MainPanel: level meter and its scale, the route strip, a notice row.

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

// A warning, tinted, with an optional button to the System Settings pane that fixes it.
class NoticeRow : public juce::Component {
public:
    explicit NoticeRow(const Notice&, std::function<void(mac::PrivacyPane)> openPane);
    int heightForWidth(int width) const;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    Notice notice_;
    TextBlock text_{TextStyle::Caption, TextBlock::Ink::Primary};
    juce::TextButton openButton_;
};

} // namespace fixomic::ui
