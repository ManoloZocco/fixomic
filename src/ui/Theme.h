#pragma once

#include "ui/StatusModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace fixomic::ui {

enum class Appearance { System, Light, Dark };

// Named colour tokens of one appearance. Every grey is a step of the appearance's single neutral
// ramp; the four state inks are used only for state; the accent only for switches that are on and
// keyboard focus.
struct Palette {
    bool dark = false;
    juce::Colour background;      // panel ground
    juce::Colour outline;         // panel edge
    juce::Colour text;            // primary text
    juce::Colour textSecondary;   // labels, values, secondary lines
    juce::Colour textTertiary;    // scale numerals, skipped stations (non-essential text)
    juce::Colour separator;       // hairlines
    juce::Colour track;           // meter track
    juce::Colour switchOff;       // switch track when off
    juce::Colour control;         // push button fill
    juce::Colour controlBorder;   // push button edge
    juce::Colour hover;           // flat button hover / pressed fill
    juce::Colour accent;          // switch on, focus ring
    juce::Colour onAccent;        // switch knob
    juce::Colour active;          // Running
    juce::Colour waiting;         // Idle, Starting, waiting for a permission
    juce::Colour bypass;          // global bypass; also warnings (degraded)
    juce::Colour problem;         // MicMissing, DriverMissing, UnsupportedSampleRate, Error

    juce::Colour ink(StatusKind) const;
};

enum class TextStyle {
    Title,       // the state sentence: 15 pt semibold
    Body,        // row labels: 13 pt
    BodyStrong,  // values, section names: 13 pt medium
    Caption,     // secondary lines, notices: 11.5 pt
    Micro,       // station names, meter scale: 10 pt medium
};

// fixomic's LookAndFeel: light and dark palettes, the system font (SF), native-looking switches
// and buttons. Appearance::System follows the macOS setting (and re-themes every on-screen
// component when it changes); Light / Dark force one (used by the snapshot command).
//
// Components read colours with Theme::palette(component), which resolves through the component's
// LookAndFeel, so a forced Theme set on a component re-themes it and its children.
//
// TextButton styles: property "fixomic.style" = "flat" (borderless, footer) or default (push).
// TextEditor (the plugin picker's search field): the push button's fill, edge and radius; focus ring
// in the accent.
class Theme : public juce::LookAndFeel_V4, private juce::DarkModeSettingListener {
public:
    explicit Theme(Appearance = Appearance::System);
    ~Theme() override;
    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;

    Appearance appearance() const;
    bool isDark() const;
    const Palette& palette() const;

    static const Palette& lightPalette();
    static const Palette& darkPalette();
    // The palette of the Theme the component uses (light when it uses another LookAndFeel).
    static const Palette& palette(const juce::Component&);

    static juce::Font font(TextStyle);
    static constexpr const char* kStyleProperty = "fixomic.style";
    static constexpr const char* kFlatStyle = "flat";
    // A flat button's text starts this far inside it: place the button this far outside the content
    // edge and its text lines up with it.
    static constexpr int kFlatTextInset = 8;

    // ---- LookAndFeel ------------------------------------------------------------------------
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted,
                              bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

private:
    void darkModeSettingChanged() override;
    void apply();   // palette from the appearance, JUCE colour ids from the palette

    Appearance appearance_;
    bool dark_ = false;
};

// A run of word-wrapped text in one TextStyle and colour, sized by its owner (heightForWidth).
// Exposed to accessibility as static text.
class TextBlock : public juce::Component {
public:
    enum class Ink { Primary, Secondary, Tertiary, Problem };

    TextBlock(TextStyle = TextStyle::Body, Ink = Ink::Primary,
              juce::Justification = juce::Justification::topLeft);

    void setText(const juce::String&);
    const juce::String& text() const;
    void setInk(Ink);
    // At most this many lines, the last one ending in "…" when the text is longer; 0 = all. The
    // accessible title keeps the whole text.
    void setMaxLines(int);
    int heightForWidth(int width) const;

    void paint(juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::TextLayout layout(float width, juce::Colour) const;
    juce::TextLayout layoutOf(const juce::String&, float width, juce::Colour) const;

    TextStyle style_;
    Ink ink_;
    juce::Justification justification_;
    juce::String text_;
    int maxLines_ = 0;
};

} // namespace fixomic::ui
