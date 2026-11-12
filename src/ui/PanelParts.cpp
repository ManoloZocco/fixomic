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

// ---- NoticeRow --------------------------------------------------------------------------------

NoticeRow::NoticeRow(const Notice& notice, std::function<void(mac::PrivacyPane)> openPane) : notice_(notice) {
    text_.setText(notice.text);
    text_.setMaxLines(notice.maxLines);
    addAndMakeVisible(text_);
    if (notice.settingsPane) {
        openButton_.setButtonText("Apri Impostazioni");
        openButton_.setMouseClickGrabsKeyboardFocus(false);
        openButton_.onClick = [pane = *notice.settingsPane, openPane] {
            if (openPane) openPane(pane);
        };
        addAndMakeVisible(openButton_);
    }
}

int NoticeRow::heightForWidth(int width) const {
    const int textHeight = text_.heightForWidth(width - 24);
    return 8 + textHeight + (notice_.settingsPane ? 6 + 24 : 0) + 8;
}

void NoticeRow::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(p.bypass.withAlpha(p.dark ? 0.16f : 0.13f));
    g.fillRoundedRectangle(bounds, 8.0f);
}

void NoticeRow::resized() {
    auto area = getLocalBounds().reduced(12, 8);
    text_.setBounds(area.removeFromTop(text_.heightForWidth(area.getWidth())));
    if (notice_.settingsPane) {
        area.removeFromTop(6);
        const int width = openButton_.getBestWidthForHeight(24) + 8;
        openButton_.setBounds(area.removeFromTop(24).removeFromLeft(width));
    }
}

} // namespace fixomic::ui
