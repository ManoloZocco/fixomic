#include "ui/Theme.h"

namespace fixomic::ui {

namespace {

juce::Colour rgb(juce::uint32 value) { return juce::Colour(0xff000000 | value); }

// One cool neutral ramp per appearance, lightest to darkest; every grey token is a step of it.
constexpr juce::uint32 kLightRamp[] = {0xffffff, 0xf6f7f9, 0xeceef1, 0xe1e4e8, 0xc9ced5,
                                       0x9ba2ad, 0x6a727e, 0x545b66, 0x353a42, 0x1b1e23};
constexpr juce::uint32 kDarkRamp[] = {0x121418, 0x1b1e23, 0x24282e, 0x2e3239, 0x3b4048,
                                      0x5a606a, 0x7f8692, 0xa4aab4, 0xcdd1d7, 0xeef0f3};

Palette makeLight() {
    const auto n = [](int step) { return rgb(kLightRamp[step]); };
    Palette p;
    p.dark = false;
    p.background = n(1);
    p.outline = juce::Colours::black.withAlpha(0.12f);
    p.text = n(9);
    p.textSecondary = n(7);
    p.textTertiary = n(6);
    p.separator = n(3);
    p.track = n(3);
    p.switchOff = n(4);
    p.control = n(0);
    p.controlBorder = n(4);
    p.hover = n(2);
    p.accent = rgb(0x2d6bdf);
    p.onAccent = n(0);
    p.active = rgb(0x1e9a52);
    p.waiting = n(6);
    p.bypass = rgb(0xb87900);
    p.problem = rgb(0xd2342b);
    return p;
}

Palette makeDark() {
    const auto n = [](int step) { return rgb(kDarkRamp[step]); };
    Palette p;
    p.dark = true;
    p.background = n(1);
    p.outline = juce::Colours::white.withAlpha(0.14f);
    p.text = n(9);
    p.textSecondary = n(7);
    p.textTertiary = n(6);
    p.separator = n(3);
    p.track = n(3);
    p.switchOff = n(4);
    p.control = n(3);
    p.controlBorder = n(4);
    p.hover = n(2);
    p.accent = rgb(0x4a8cff);
    p.onAccent = rgb(0xffffff);
    p.active = rgb(0x3cc572);
    p.waiting = n(6);
    p.bypass = rgb(0xf0b43c);
    p.problem = rgb(0xff6259);
    return p;
}

bool isFlat(const juce::Component& c) {
    return c.getProperties()[Theme::kStyleProperty].toString() == Theme::kFlatStyle;
}

} // namespace

juce::Colour Palette::ink(StatusKind kind) const {
    switch (kind) {
    case StatusKind::Active: return active;
    case StatusKind::Waiting: return waiting;
    case StatusKind::Bypass: return bypass;
    case StatusKind::Problem: return problem;
    }
    return waiting;
}

// ---- Theme ------------------------------------------------------------------------------------

Theme::Theme(Appearance appearance) : appearance_(appearance) {
    setDefaultSansSerifTypefaceName(juce::Font::getSystemUIFontName());
    if (appearance_ == Appearance::System) juce::Desktop::getInstance().addDarkModeSettingListener(this);
    apply();
}

Theme::~Theme() {
    if (appearance_ == Appearance::System) juce::Desktop::getInstance().removeDarkModeSettingListener(this);
}

Appearance Theme::appearance() const { return appearance_; }
bool Theme::isDark() const { return dark_; }
const Palette& Theme::palette() const { return dark_ ? darkPalette() : lightPalette(); }

const Palette& Theme::lightPalette() {
    static const Palette palette = makeLight();
    return palette;
}

const Palette& Theme::darkPalette() {
    static const Palette palette = makeDark();
    return palette;
}

const Palette& Theme::palette(const juce::Component& component) {
    if (const auto* theme = dynamic_cast<const Theme*>(&component.getLookAndFeel())) return theme->palette();
    return lightPalette();
}

juce::Font Theme::font(TextStyle style) {
    const auto system = [](const char* weight, float points) {
        return juce::Font(juce::FontOptions{}
                              .withName(juce::Font::getSystemUIFontName())
                              .withStyle(weight)
                              .withPointHeight(points));
    };
    switch (style) {
    case TextStyle::Title: return system("Semibold", 15.0f);
    case TextStyle::Body: return system("Regular", 13.0f);
    case TextStyle::BodyStrong: return system("Medium", 13.0f);
    case TextStyle::Caption: return system("Regular", 11.5f);
    case TextStyle::Micro: return system("Medium", 10.0f);
    }
    return system("Regular", 13.0f);
}

void Theme::darkModeSettingChanged() {
    apply();
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* component = desktop.getComponent(i)) component->sendLookAndFeelChange();
}

void Theme::apply() {
    switch (appearance_) {
    case Appearance::System: dark_ = juce::Desktop::getInstance().isDarkModeActive(); break;
    case Appearance::Light: dark_ = false; break;
    case Appearance::Dark: dark_ = true; break;
    }
    const auto& p = palette();
    setColourScheme(dark_ ? getDarkColourScheme() : getLightColourScheme());
    setColour(juce::ResizableWindow::backgroundColourId, p.background);
    setColour(juce::Label::textColourId, p.text);
    setColour(juce::TextButton::buttonColourId, p.control);
    setColour(juce::TextButton::textColourOffId, p.text);
    setColour(juce::TextButton::textColourOnId, p.text);
    setColour(juce::ToggleButton::textColourId, p.text);
    setColour(juce::ToggleButton::tickColourId, p.accent);
    setColour(juce::PopupMenu::backgroundColourId, p.background);
    setColour(juce::PopupMenu::textColourId, p.text);
    setColour(juce::PopupMenu::headerTextColourId, p.textSecondary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.accent);
    setColour(juce::PopupMenu::highlightedTextColourId, p.onAccent);
    setColour(juce::TextEditor::backgroundColourId, p.control);
    setColour(juce::TextEditor::textColourId, p.text);
    setColour(juce::TextEditor::outlineColourId, p.controlBorder);
    setColour(juce::TextEditor::focusedOutlineColourId, p.accent.withAlpha(0.6f));
    setColour(juce::TextEditor::highlightColourId, p.accent.withAlpha(0.3f));   // text selection: focus
    setColour(juce::TextEditor::highlightedTextColourId, p.text);
    setColour(juce::CaretComponent::caretColourId, p.text);
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
}

// A switch (macOS style), right-aligned in the button; the button text is its accessible name.
void Theme::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool, bool) {
    const auto& p = palette(button);
    const auto bounds = button.getLocalBounds().toFloat();
    const float h = 20.0f, w = 34.0f;
    const juce::Rectangle<float> track(bounds.getRight() - w - 2.0f, bounds.getCentreY() - h / 2.0f, w, h);
    const bool on = button.getToggleState();
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;

    if (button.hasKeyboardFocus(false)) {
        g.setColour(p.accent.withAlpha(0.45f * alpha));
        g.drawRoundedRectangle(track.expanded(2.5f), (h + 5.0f) / 2.0f, 3.0f);
    }
    g.setColour((on ? p.accent : p.switchOff).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(track, h / 2.0f);

    const float knob = h - 4.0f;
    const float knobX = on ? track.getRight() - 2.0f - knob : track.getX() + 2.0f;
    const juce::Rectangle<float> knobArea(knobX, track.getY() + 2.0f, knob, knob);
    g.setColour(juce::Colours::black.withAlpha(0.18f * alpha));
    g.fillEllipse(knobArea.translated(0.0f, 0.5f).expanded(0.5f));
    g.setColour(p.onAccent.withMultipliedAlpha(alpha));
    g.fillEllipse(knobArea);
}

void Theme::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted,
                                 bool down) {
    const auto& p = palette(button);
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;
    const float radius = 6.0f;

    if (isFlat(button)) {
        if (button.isEnabled() && (highlighted || down)) {
            g.setColour(down ? p.separator : p.hover);
            g.fillRoundedRectangle(bounds, radius);
        }
    } else {
        auto fill = p.control;
        if (button.isEnabled() && down)
            fill = fill.overlaidWith(p.text.withAlpha(0.10f));
        else if (button.isEnabled() && highlighted)
            fill = fill.overlaidWith(p.text.withAlpha(0.05f));
        g.setColour(fill.withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(p.controlBorder.withMultipliedAlpha(alpha));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
    }
    if (button.hasKeyboardFocus(false)) {
        g.setColour(p.accent.withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 2.0f);
    }
}

void Theme::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) {
    const auto& p = palette(button);
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(button.isEnabled() ? p.text : p.textTertiary);
    const int inset = isFlat(button) ? kFlatTextInset : 10;
    g.drawText(button.getButtonText(), button.getLocalBounds().reduced(inset, 0),
               isFlat(button) ? juce::Justification::centredLeft : juce::Justification::centred, true);
}

juce::Font Theme::getTextButtonFont(juce::TextButton& button, int) {
    return font(isFlat(button) ? TextStyle::Body : TextStyle::Caption);
}

juce::Font Theme::getPopupMenuFont() { return font(TextStyle::Body); }

void Theme::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) {
    const auto& p = palette(editor);
    g.setColour(p.control.withMultipliedAlpha(editor.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(0.5f),
                           6.0f);
}

void Theme::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) {
    const auto& p = palette(editor);
    const auto bounds = juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    g.setColour(p.controlBorder.withMultipliedAlpha(editor.isEnabled() ? 1.0f : 0.4f));
    g.drawRoundedRectangle(bounds, 6.0f, 1.0f);
    if (editor.isEnabled() && editor.hasKeyboardFocus(true) && !editor.isReadOnly()) {
        g.setColour(p.accent.withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 2.0f);
    }
}

// ---- TextBlock --------------------------------------------------------------------------------

TextBlock::TextBlock(TextStyle style, Ink ink, juce::Justification justification)
    : style_(style), ink_(ink), justification_(justification) {
    setInterceptsMouseClicks(false, false);
}

void TextBlock::setText(const juce::String& text) {
    if (text == text_) return;
    text_ = text;
    setTitle(text);
    if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
    repaint();
}

const juce::String& TextBlock::text() const { return text_; }

void TextBlock::setInk(Ink ink) {
    if (ink == ink_) return;
    ink_ = ink;
    repaint();
}

void TextBlock::setMaxLines(int lines) {
    if (lines == maxLines_) return;
    maxLines_ = lines;
    repaint();
}

juce::TextLayout TextBlock::layoutOf(const juce::String& text, float width, juce::Colour colour) const {
    juce::AttributedString string;
    string.setWordWrap(juce::AttributedString::byWord);
    string.setJustification(justification_);
    string.append(text, Theme::font(style_), colour);
    juce::TextLayout result;
    result.createLayout(string, std::max(1.0f, width));
    return result;
}

juce::TextLayout TextBlock::layout(float width, juce::Colour colour) const {
    auto full = layoutOf(text_, width, colour);
    if (maxLines_ <= 0 || full.getNumLines() <= maxLines_) return full;
    // The longest prefix that still fits with the ellipsis (binary search: a handful of layouts,
    // only for capped text that overflows).
    const auto elided = [this](int length) {
        return text_.substring(0, length).trimEnd() + juce::String::fromUTF8("\u2026");
    };
    int fits = 0, tooLong = text_.length();
    while (tooLong - fits > 1) {
        const int middle = (fits + tooLong) / 2;
        if (layoutOf(elided(middle), width, colour).getNumLines() <= maxLines_)
            fits = middle;
        else
            tooLong = middle;
    }
    return layoutOf(elided(fits), width, colour);
}

int TextBlock::heightForWidth(int width) const {
    if (text_.isEmpty()) return 0;
    return static_cast<int>(std::ceil(layout(static_cast<float>(width), juce::Colours::black).getHeight()));
}

void TextBlock::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    juce::Colour colour = p.text;
    switch (ink_) {
    case Ink::Primary: colour = p.text; break;
    case Ink::Secondary: colour = p.textSecondary; break;
    case Ink::Tertiary: colour = p.textTertiary; break;
    case Ink::Problem: colour = p.problem; break;
    }
    layout(static_cast<float>(getWidth()), colour).draw(g, getLocalBounds().toFloat());
}

std::unique_ptr<juce::AccessibilityHandler> TextBlock::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::staticText);
}

} // namespace fixomic::ui
