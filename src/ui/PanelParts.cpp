#include "ui/PanelParts.h"

#include <algorithm>
#include <cmath>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

// The x of a dB value on a meter of width `width`.
float meterX(float db, float width) { return meterFraction(db) * width; }

} // namespace

// ---- LevelMeter -------------------------------------------------------------------------------

void LevelMeter::setLevel(float peakDb) {
    const float fraction = live_ ? meterFraction(peakDb) : 0.0f;
    const bool clipping = live_ && peakDb >= -1.0f;
    if (std::abs(fraction - fraction_) < 0.002f && clipping == clipping_) return;
    fraction_ = fraction;
    clipping_ = clipping;
    repaint();
}

void LevelMeter::setInk(StatusKind kind, bool live) {
    if (kind == kind_ && live == live_) return;
    kind_ = kind;
    live_ = live;
    if (!live_) fraction_ = 0.0f;
    repaint();
}

void LevelMeter::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    const float h = 6.0f;
    const auto bar = getLocalBounds().toFloat().withSizeKeepingCentre(static_cast<float>(getWidth()), h);
    g.setColour(p.track);
    g.fillRoundedRectangle(bar, h / 2.0f);
    if (fraction_ > 0.0f) {
        g.setColour(clipping_ ? p.problem : p.ink(kind_));
        g.fillRoundedRectangle(bar.withWidth(std::max(h, bar.getWidth() * fraction_)), h / 2.0f);
    }
    // Rules: notches in the ground colour, so the scale reads on the track and on the fill alike.
    g.setColour(p.background);
    for (const float db : kTicksDb)
        g.fillRect(juce::Rectangle<float>(bar.getX() + meterX(db, bar.getWidth()) - 0.75f, bar.getY(), 1.5f, h));
}

void MeterScale::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    g.setFont(Theme::font(TextStyle::Micro));
    g.setColour(p.textTertiary);
    const float width = static_cast<float>(getWidth());
    const auto label = [&](float db, const juce::String& text, juce::Justification justification) {
        const float x = meterX(db, width);
        const float w = 40.0f;
        const float left = justification == juce::Justification::centredRight ? x - w
                           : justification == juce::Justification::centred    ? x - w / 2.0f
                                                                              : x;
        g.drawText(text, juce::Rectangle<float>(left, 0.0f, w, static_cast<float>(getHeight())), justification, false);
    };
    for (const float db : LevelMeter::kTicksDb)
        label(db, utf8("\u2212") + juce::String(static_cast<int>(-db)), juce::Justification::centred);
    label(0.0f, "0 dB", juce::Justification::centredRight);
}

// ---- RouteStripView ---------------------------------------------------------------------------

RouteStripView::RouteStripView() {
    setInterceptsMouseClicks(false, false);
    setTitle("Percorso della voce");
}

void RouteStripView::setRoute(const RouteStrip& route) {
    if (route == route_) return;
    route_ = route;
    repaint();
}

void RouteStripView::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    const juce::Colour ink = p.ink(route_.line);
    const float inset = 30.0f;
    const float lineY = 7.0f;
    const float radius = 5.5f;
    const float left = inset, right = static_cast<float>(getWidth()) - inset;

    g.setColour(ink);
    g.fillRoundedRectangle(juce::Rectangle<float>(left, lineY - 1.5f, right - left, 3.0f), 1.5f);

    struct Station {
        StationState state;
        const char* name;
    };
    const Station stations[] = {{route_.microphone, "Microfono"},
                                {route_.echo, "Eco"},
                                {route_.chain, "Catena"},
                                {route_.output, "fixomic Mic"}};
    g.setFont(Theme::font(TextStyle::Micro));
    for (int i = 0; i < 4; ++i) {
        const auto& station = stations[i];
        const float x = left + (right - left) * static_cast<float>(i) / 3.0f;
        const auto marker = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre({x, lineY});
        juce::Colour labelColour = p.textSecondary;
        switch (station.state) {
        case StationState::InService:
            g.setColour(p.background);   // a ring of ground keeps the marker off the line
            g.fillEllipse(marker.expanded(2.0f));
            g.setColour(ink);
            g.fillEllipse(marker);
            break;
        case StationState::Skipped:
            g.setColour(p.background);
            g.fillEllipse(marker);
            g.setColour(ink);
            g.drawEllipse(marker.reduced(1.0f), 2.0f);
            labelColour = p.textTertiary;
            break;
        case StationState::Closed:
            g.setColour(p.background);
            g.fillEllipse(marker.expanded(2.0f));
            g.setColour(p.problem);
            g.fillEllipse(marker.expanded(1.0f));
            g.setColour(p.background);   // "no entry" bar
            g.fillRoundedRectangle(juce::Rectangle<float>(radius * 1.3f, 2.2f).withCentre({x, lineY}), 1.1f);
            labelColour = p.problem;
            break;
        }
        g.setColour(labelColour);
        g.drawText(station.name, juce::Rectangle<float>(x - 44.0f, lineY + 9.0f, 88.0f, 14.0f),
                   juce::Justification::centredTop, false);
    }
}

// ---- GlyphButton ------------------------------------------------------------------------------

GlyphButton::GlyphButton(Glyph glyph, const juce::String& accessibleName) : juce::Button(accessibleName), glyph_(glyph) {
    setButtonText(accessibleName);
    setMouseClickGrabsKeyboardFocus(false);
}

void GlyphButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = Theme::palette(*this);
    const auto bounds = getLocalBounds().toFloat();
    if (isEnabled() && (highlighted || down)) {
        g.setColour(down ? p.separator : p.hover);
        g.fillRoundedRectangle(bounds, 6.0f);
    }
    if (hasKeyboardFocus(false)) {
        g.setColour(p.accent.withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 6.0f, 2.0f);
    }

    const auto c = bounds.getCentre();
    juce::Path glyph;
    switch (glyph_) {
    case Glyph::Remove: {
        const float r = 3.75f;
        glyph.startNewSubPath(c.x - r, c.y - r);
        glyph.lineTo(c.x + r, c.y + r);
        glyph.startNewSubPath(c.x + r, c.y - r);
        glyph.lineTo(c.x - r, c.y + r);
        break;
    }
    case Glyph::Back:
        glyph.startNewSubPath(c.x + 2.0f, c.y - 5.0f);
        glyph.lineTo(c.x - 3.0f, c.y);
        glyph.lineTo(c.x + 2.0f, c.y + 5.0f);
        break;
    }
    g.setColour(isEnabled() ? p.textSecondary : p.textTertiary);
    g.strokePath(glyph, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ---- NoticeRow --------------------------------------------------------------------------------

NoticeRow::NoticeRow(const Notice& notice, std::function<void(mac::PrivacyPane)> openPane)
    : NoticeRow(notice.text, notice.settingsPane ? juce::String("Apri Impostazioni") : juce::String(),
                [pane = notice.settingsPane, openPane] {
                    if (pane && openPane) openPane(*pane);
                }) {
    text_.setMaxLines(notice.maxLines);
}

NoticeRow::NoticeRow(const juce::String& text, const juce::String& buttonText, std::function<void()> onButton) {
    text_.setText(text);
    addAndMakeVisible(text_);
    hasButton_ = buttonText.isNotEmpty();
    if (hasButton_) {
        button_.setButtonText(buttonText);
        button_.setMouseClickGrabsKeyboardFocus(false);
        button_.onClick = std::move(onButton);
        addAndMakeVisible(button_);
    }
}

int NoticeRow::heightForWidth(int width) const {
    const int textHeight = text_.heightForWidth(width - 24);
    return 8 + textHeight + (hasButton_ ? 6 + 24 : 0) + 8;
}

juce::Button* NoticeRow::button() { return hasButton_ ? &button_ : nullptr; }

void NoticeRow::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(p.bypass.withAlpha(p.dark ? 0.16f : 0.13f));
    g.fillRoundedRectangle(bounds, 8.0f);
}

void NoticeRow::resized() {
    auto area = getLocalBounds().reduced(12, 8);
    text_.setBounds(area.removeFromTop(text_.heightForWidth(area.getWidth())));
    if (hasButton_) {
        area.removeFromTop(6);
        button_.setBounds(area.removeFromTop(24).removeFromLeft(Theme::pushButtonWidth(button_.getButtonText())));
    }
}

// ---- ChoiceList -------------------------------------------------------------------------------

namespace {

constexpr int kRadioGroup = 0x6669;   // any non-zero id: makes the rows radio buttons

void drawCheck(juce::Graphics& g, juce::Point<float> centre, juce::Colour colour) {
    juce::Path check;
    check.startNewSubPath(centre.x - 4.0f, centre.y + 0.2f);
    check.lineTo(centre.x - 1.2f, centre.y + 3.0f);
    check.lineTo(centre.x + 4.2f, centre.y - 3.4f);
    g.setColour(colour);
    g.strokePath(check, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

} // namespace

class ChoiceList::Row : public juce::Button {
public:
    explicit Row(ChoiceList& owner) : juce::Button({}), owner_(owner) {
        setMouseClickGrabsKeyboardFocus(false);
        setRadioGroupId(kRadioGroup);
        onClick = [this] { owner_.chooseSoon(index_); };
    }

    void set(int index, const Item& item) {
        index_ = index;
        item_ = item;
        setButtonText(item.text);
        setToggleState(item.selected, juce::dontSendNotification);
        repaint();
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
        const auto& p = Theme::palette(*this);
        const auto bounds = getLocalBounds().toFloat().reduced(0.0f, 1.0f);
        if (isEnabled() && (highlighted || down)) {
            g.setColour(down ? p.separator : p.hover);
            g.fillRoundedRectangle(bounds, 6.0f);
        }
        if (hasKeyboardFocus(false)) {
            g.setColour(p.accent.withAlpha(0.6f));
            g.drawRoundedRectangle(bounds.reduced(1.0f), 6.0f, 2.0f);
        }
        const float x = static_cast<float>(Theme::kFlatTextInset);
        if (item_.selected) drawCheck(g, {x + 5.0f, bounds.getCentreY()}, p.text);
        g.setFont(Theme::font(TextStyle::Body));
        g.setColour(item_.dimmed ? p.textSecondary : p.text);
        g.drawText(item_.text,
                   getLocalBounds().withTrimmedLeft(Theme::kFlatTextInset + kTextIndent).withTrimmedRight(Theme::kFlatTextInset),
                   juce::Justification::centredLeft, true);
    }

private:
    ChoiceList& owner_;
    int index_ = -1;
    Item item_;
};

ChoiceList::ChoiceList(const juce::String& accessibleTitle) {
    setOpaque(false);
    setTitle(accessibleTitle);
}

ChoiceList::~ChoiceList() = default;

void ChoiceList::setItems(std::vector<Item> items) {
    if (items == items_) return;
    items_ = std::move(items);
    // Rows are kept and refilled by index: only the surplus is destroyed (never from inside a row's
    // click: choices are reported asynchronously).
    while (rows_.size() > items_.size()) rows_.pop_back();
    while (rows_.size() < items_.size()) {
        rows_.push_back(std::make_unique<Row>(*this));
        addAndMakeVisible(*rows_.back());
    }
    for (size_t i = 0; i < items_.size(); ++i) rows_[i]->set(static_cast<int>(i), items_[i]);
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));
    resized();
}

const std::vector<ChoiceList::Item>& ChoiceList::items() const { return items_; }

int ChoiceList::heightForWidth(int) const { return static_cast<int>(items_.size()) * kRowHeight; }

juce::Button* ChoiceList::row(int index) const {
    return index >= 0 && index < static_cast<int>(rows_.size()) ? rows_[static_cast<size_t>(index)].get() : nullptr;
}

void ChoiceList::resized() {
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->setBounds(0, static_cast<int>(i) * kRowHeight, getWidth(), kRowHeight);
}

void ChoiceList::chooseSoon(int index) {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ChoiceList>(this), index] {
        if (safe != nullptr && index >= 0 && index < static_cast<int>(safe->items_.size()) && safe->onChoose)
            safe->onChoose(index);
    });
}

// ---- SegmentedControl -------------------------------------------------------------------------

class SegmentedControl::Segment : public juce::Button {
public:
    Segment(SegmentedControl& owner, int index) : juce::Button({}), owner_(owner), index_(index) {
        setMouseClickGrabsKeyboardFocus(false);
        setRadioGroupId(kRadioGroup);
        onClick = [this] { owner_.chooseSoon(index_); };
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
        const auto& p = Theme::palette(*this);
        const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        const bool on = getToggleState();
        if (on) {
            g.setColour(p.control);
            g.fillRoundedRectangle(bounds, 5.0f);
            g.setColour(p.controlBorder);
            g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
        } else if (isEnabled() && (highlighted || down)) {
            g.setColour(p.text.withAlpha(down ? 0.10f : 0.05f));
            g.fillRoundedRectangle(bounds, 5.0f);
        }
        if (hasKeyboardFocus(false)) {
            g.setColour(p.accent.withAlpha(0.6f));
            g.drawRoundedRectangle(bounds, 5.0f, 2.0f);
        }
        g.setFont(Theme::font(TextStyle::Caption));
        g.setColour(on ? p.text : p.textSecondary);
        g.drawText(getButtonText(), getLocalBounds().reduced(4, 0), juce::Justification::centred, true);
    }

private:
    SegmentedControl& owner_;
    int index_;
};

SegmentedControl::SegmentedControl(const juce::String& accessibleTitle) {
    setOpaque(false);
    setTitle(accessibleTitle);
}

SegmentedControl::~SegmentedControl() = default;

void SegmentedControl::setOptions(const juce::StringArray& options, int selected) {
    if (options != options_) {
        options_ = options;
        segments_.clear();   // never from inside a segment's click: choices are reported asynchronously
        for (int i = 0; i < options_.size(); ++i) {
            segments_.push_back(std::make_unique<Segment>(*this, i));
            segments_.back()->setButtonText(options_[i]);
            addAndMakeVisible(*segments_.back());
        }
    }
    selected_ = selected;
    for (size_t i = 0; i < segments_.size(); ++i)
        segments_[i]->setToggleState(static_cast<int>(i) == selected_, juce::dontSendNotification);
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));
    resized();
    repaint();
}

const juce::StringArray& SegmentedControl::options() const { return options_; }
int SegmentedControl::selected() const { return selected_; }

int SegmentedControl::lines() const { return (options_.size() + kPerLine - 1) / kPerLine; }

int SegmentedControl::heightForWidth(int) const {
    const int n = lines();
    return n == 0 ? 0 : n * kSegmentHeight + (n - 1) * kLineGap;
}

juce::Button* SegmentedControl::segment(int index) const {
    return index >= 0 && index < static_cast<int>(segments_.size()) ? segments_[static_cast<size_t>(index)].get()
                                                                     : nullptr;
}

juce::Rectangle<int> SegmentedControl::segmentBounds(int index) const {
    const int perLine = std::min(kPerLine, std::max(1, options_.size()));
    const int line = index / perLine, column = index % perLine;
    const int inner = getWidth() - 4;   // 2 pt of track around the segments
    const int left = 2 + inner * column / perLine;
    const int right = 2 + inner * (column + 1) / perLine;
    return {left, line * (kSegmentHeight + kLineGap) + 2, right - left, kSegmentHeight - 4};
}

void SegmentedControl::resized() {
    for (size_t i = 0; i < segments_.size(); ++i) segments_[i]->setBounds(segmentBounds(static_cast<int>(i)));
}

void SegmentedControl::paint(juce::Graphics& g) {
    // The track: one per line, the hover step of the ramp.
    const auto& p = Theme::palette(*this);
    g.setColour(p.hover);
    for (int line = 0; line < lines(); ++line)
        g.fillRoundedRectangle(juce::Rectangle<float>(0.0f, static_cast<float>(line * (kSegmentHeight + kLineGap)),
                                                      static_cast<float>(getWidth()), static_cast<float>(kSegmentHeight)),
                               6.0f);
}

void SegmentedControl::chooseSoon(int index) {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<SegmentedControl>(this), index] {
        if (safe != nullptr && index >= 0 && index < safe->options_.size() && safe->onChoose) safe->onChoose(index);
    });
}

} // namespace fixomic::ui
