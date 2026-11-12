#include "ui/MainPanel.h"

#include "app/AppController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int kPad = 16;          // panel edge to content
constexpr int kGroupGap = 12;     // group to hairline, hairline to group
constexpr int kSwitchWidth = 40;
constexpr int kSwitchHeight = 24;
constexpr float kCornerRadius = 14.0f;

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

void dim(juce::Component& component, bool dimmed) { component.setAlpha(dimmed ? 0.45f : 1.0f); }

juce::String emptyChainText() {
    return utf8("Nessun plugin: la voce esce con la sola cancellazione dell\u2019eco.");
}

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

// ---- MainPanel --------------------------------------------------------------------------------

struct MainPanel::Layout {
    juce::Rectangle<int> statusText, bypassLabel, bypassSwitch, route;
    std::vector<juce::Rectangle<int>> notices;
    juce::Rectangle<int> inputLabel, inputMeter, outputLabel, outputMeter, meterScale;
    juce::Rectangle<int> aecLabel, aecSwitch, erleLabel, erleValue;
    juce::Rectangle<int> chainTitle, chainBody;
    juce::Rectangle<int> latencyLabel, latencyValue, latencyDetail;
    juce::Rectangle<int> settingsButton, quitButton;
    std::vector<int> separators;
    int height = 0;
};

MainPanel::MainPanel(AppController& app) : app_(app) {
    setOpaque(false);
    setWantsKeyboardFocus(false);

    bypassLabel_.setText("Bypass");
    bypassSwitch_.setButtonText("Bypass globale");
    bypassSwitch_.setMouseClickGrabsKeyboardFocus(false);
    bypassSwitch_.onClick = [this] {
        const bool on = bypassSwitch_.getToggleState();
        app_.updateConfig([on](Config& c) { c.globalBypass = on; });
    };
    inputLabel_.setText("Ingresso");
    outputLabel_.setText("Uscita");
    inputMeter_.setTitle("Livello ingresso");
    outputMeter_.setTitle("Livello uscita");

    aecLabel_.setText("Cancellazione eco");
    aecSwitch_.setButtonText("Cancellazione eco");
    aecSwitch_.setMouseClickGrabsKeyboardFocus(false);
    aecSwitch_.onClick = [this] {
        const bool on = aecSwitch_.getToggleState();
        app_.updateConfig([on](Config& c) { c.aecEnabled = on; });
    };
    erleLabel_.setText("Eco rimossa");

    chainTitle_.setText("Catena");

    latencyLabel_.setText("Latenza");

    settingsButton_.setButtonText(utf8("Impostazioni\u2026"));
    quitButton_.setButtonText("Esci");
    for (auto* button : {&settingsButton_, &quitButton_}) {
        button->getProperties().set(Theme::kStyleProperty, Theme::kFlatStyle);
        button->setMouseClickGrabsKeyboardFocus(false);
    }
    settingsButton_.onClick = [this] {
        if (onOpenSettings) onOpenSettings();
    };
    quitButton_.onClick = [this] {
        if (onQuit) onQuit();
    };

    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &statusText_, &bypassLabel_, &bypassSwitch_, &route_, &inputLabel_, &outputLabel_, &inputMeter_,
             &outputMeter_, &meterScale_, &aecLabel_, &aecSwitch_, &erleLabel_, &erleValue_, &chainTitle_,
             &chainPlaceholder_, &latencyLabel_, &latencyValue_, &latencyDetail_, &settingsButton_, &quitButton_})
        addAndMakeVisible(c);

    refresh(status_);
    setSize(kWidth, preferredHeight(kWidth));
}

MainPanel::~MainPanel() { stopTimer(); }

void MainPanel::refresh(const StatusPresentation& status) {
    status_ = status;
    const auto& config = app_.config();
    const bool running = app_.engineState() == EngineState::Running;
    const bool bypassed = config.globalBypass;

    statusText_.setText(status.text);
    bypassSwitch_.setToggleState(bypassed, juce::dontSendNotification);
    aecSwitch_.setToggleState(config.aecEnabled, juce::dontSendNotification);

    const auto slots = app_.slots();
    const bool chainProcessing = std::any_of(slots.begin(), slots.end(), [](const PluginChain::SlotInfo& slot) {
        return !slot.missing && !slot.bypassed;
    });
    std::vector<ChainEntry> chain;
    for (const auto& slot : slots) chain.push_back({slot.name, slot.bypassed, slot.missing});
    const auto summary = chainSummary(chain);
    chainPlaceholder_.setText(summary.isNotEmpty() ? summary : emptyChainText());
    chainPlaceholder_.setInk(summary.isNotEmpty() ? TextBlock::Ink::Primary : TextBlock::Ink::Secondary);
    const auto warningPane = app_.lastWarningPane();
    route_.setRoute(routeFor(status, {config.aecEnabled, app_.lastWarning().isNotEmpty(), chainProcessing}));

    inputMeter_.setInk(status.kind, running);
    outputMeter_.setInk(status.kind, running);

    // What global bypass skips recedes; it stays operable.
    for (juce::Component* c : std::initializer_list<juce::Component*>{&aecLabel_, &aecSwitch_, &erleLabel_,
                                                                      &erleValue_, &chainTitle_, &chainPlaceholder_})
        dim(*c, bypassed);
    if (chainView_ != nullptr) dim(*chainView_, bypassed);

    const auto latency = app_.latency();
    latencyLabel_.setText(running ? "Latenza" : "Latenza stimata");
    latencyValue_.setText(formatLatencyTotal(latency));
    latencyDetail_.setText(formatLatencyDetail(latency));

    const auto notices = collectNotices(app_.lastWarning(), warningPane, app_.startupWarnings());
    if (notices != notices_) rebuildNotices(notices);

    updateMeters();
    heightMayHaveChanged();
    resized();
    repaint();
}

void MainPanel::rebuildNotices(const std::vector<Notice>& notices) {
    notices_ = notices;
    noticeRows_.clear();
    for (const auto& notice : notices_) {
        auto row = std::make_unique<NoticeRow>(notice, [this](mac::PrivacyPane pane) { app_.openPrivacySettings(pane); });
        addAndMakeVisible(*row);
        noticeRows_.push_back(std::move(row));
    }
}

void MainPanel::setLive(bool live) {
    if (live) {
        updateMeters();
        startTimerHz(kMeterHz);
    } else {
        stopTimer();
    }
}

bool MainPanel::isLive() const { return isTimerRunning(); }

void MainPanel::timerCallback() { updateMeters(); }

void MainPanel::updateMeters() {
    inputMeter_.setLevel(app_.inputPeakDb());
    outputMeter_.setLevel(app_.outputPeakDb());
    const bool measuring = app_.engineState() == EngineState::Running && app_.config().aecEnabled
                           && !app_.config().globalBypass;
    erleValue_.setText(measuring ? formatErle(app_.erleDb()) : formatErle(std::numeric_limits<float>::quiet_NaN()));
}

void MainPanel::setChainView(std::unique_ptr<juce::Component> view) {
    if (chainView_ != nullptr) removeChildComponent(chainView_.get());
    chainView_ = std::move(view);
    chainPlaceholder_.setVisible(chainView_ == nullptr);
    if (chainView_ != nullptr) addAndMakeVisible(*chainView_);
    heightMayHaveChanged();
    resized();
}

juce::Component* MainPanel::chainView() const { return chainView_.get(); }

void MainPanel::childBoundsChanged(juce::Component* child) {
    if (child == nullptr || child != chainView_.get()) return;
    heightMayHaveChanged();
    resized();   // the rows under the chain move; settles at once (same bounds, no new call)
}

int MainPanel::preferredHeight(int width) const { return layOut(width).height; }

void MainPanel::heightMayHaveChanged() {
    const int height = preferredHeight(kWidth);
    if (height == lastPreferredHeight_) return;
    lastPreferredHeight_ = height;
    if (onPreferredHeightChanged) onPreferredHeightChanged();
}

MainPanel::Layout MainPanel::layOut(int width) const {
    Layout l;
    const int contentWidth = width - 2 * kPad;
    const int x = kPad;
    int y = 14;
    const auto separator = [&] {
        y += kGroupGap;
        l.separators.push_back(y);
        y += 1 + kGroupGap;
    };
    // A label at the left and a fixed-width control or value at the right, on one row.
    const auto row = [&](juce::Rectangle<int>& label, juce::Rectangle<int>& right, int rightWidth, int height) {
        label = {x, y, contentWidth - rightWidth - 8, height};
        right = {x + contentWidth - rightWidth, y, rightWidth, height};
        y += height;
    };

    // State sentence + bypass switch, then the route strip.
    const int switchColumn = kSwitchWidth + 4 + 52;
    const int statusWidth = contentWidth - switchColumn - 8;
    const int statusHeight = std::max(20, statusText_.heightForWidth(statusWidth));
    l.statusText = {x, y, statusWidth, statusHeight};
    l.bypassSwitch = {x + contentWidth - kSwitchWidth, y - 2, kSwitchWidth, kSwitchHeight};
    l.bypassLabel = {l.bypassSwitch.getX() - 4 - 52, y - 2, 52, kSwitchHeight};
    y += statusHeight + 12;
    l.route = {x, y, contentWidth, RouteStripView::kHeight};
    y += RouteStripView::kHeight;

    for (const auto& notice : noticeRows_) {
        y += 10;
        const int h = notice->heightForWidth(contentWidth);
        l.notices.push_back({x, y, contentWidth, h});
        y += h;
    }
    separator();

    // Levels.
    const int labelWidth = 72;
    l.inputLabel = {x, y, labelWidth, 20};
    l.inputMeter = {x + labelWidth, y, contentWidth - labelWidth, 20};
    y += 24;
    l.outputLabel = {x, y, labelWidth, 20};
    l.outputMeter = {x + labelWidth, y, contentWidth - labelWidth, 20};
    y += 20;
    l.meterScale = {x + labelWidth, y, contentWidth - labelWidth, 13};
    y += 13;
    separator();

    // Echo cancellation.
    row(l.aecLabel, l.aecSwitch, kSwitchWidth, kSwitchHeight);
    l.aecLabel = l.aecLabel.withTrimmedTop(3);
    y += 2;
    row(l.erleLabel, l.erleValue, 80, 16);
    separator();

    // Chain (10b).
    l.chainTitle = {x, y, contentWidth, 18};
    y += 18 + 4;
    const int chainHeight = chainView_ != nullptr ? chainView_->getHeight() : chainPlaceholder_.heightForWidth(contentWidth);
    l.chainBody = {x, y, contentWidth, chainHeight};
    y += chainHeight;
    separator();

    // Latency.
    row(l.latencyLabel, l.latencyValue, 100, 18);
    y += 2;
    l.latencyDetail = {x, y, contentWidth, latencyDetail_.heightForWidth(contentWidth)};
    y += l.latencyDetail.getHeight();
    separator();
    y -= kGroupGap - 4;   // the footer buttons carry their own padding

    // Footer: flat buttons whose text lines up with the content edges (8 pt of padding outside).
    const auto footerButton = [](const juce::TextButton& button) {
        return juce::GlyphArrangement::getStringWidthInt(Theme::font(TextStyle::Body), button.getButtonText()) + 16;
    };
    const int quitWidth = footerButton(quitButton_);
    l.settingsButton = {x - 8, y, footerButton(settingsButton_), 28};
    l.quitButton = {x + contentWidth + 8 - quitWidth, y, quitWidth, 28};
    y += 28 + 8;

    l.height = y;
    return l;
}

void MainPanel::resized() {
    const auto l = layOut(getWidth());
    statusText_.setBounds(l.statusText);
    bypassLabel_.setBounds(l.bypassLabel.withTrimmedTop(4));
    bypassSwitch_.setBounds(l.bypassSwitch);
    route_.setBounds(l.route);
    for (size_t i = 0; i < noticeRows_.size() && i < l.notices.size(); ++i) noticeRows_[i]->setBounds(l.notices[i]);
    inputLabel_.setBounds(l.inputLabel.withTrimmedTop(2));
    inputMeter_.setBounds(l.inputMeter);
    outputLabel_.setBounds(l.outputLabel.withTrimmedTop(2));
    outputMeter_.setBounds(l.outputMeter);
    meterScale_.setBounds(l.meterScale);
    aecLabel_.setBounds(l.aecLabel);
    aecSwitch_.setBounds(l.aecSwitch);
    erleLabel_.setBounds(l.erleLabel);
    erleValue_.setBounds(l.erleValue);
    chainTitle_.setBounds(l.chainTitle);
    if (chainView_ != nullptr)
        chainView_->setBounds(l.chainBody);
    else
        chainPlaceholder_.setBounds(l.chainBody);
    latencyLabel_.setBounds(l.latencyLabel);
    latencyValue_.setBounds(l.latencyValue);
    latencyDetail_.setBounds(l.latencyDetail);
    settingsButton_.setBounds(l.settingsButton);
    quitButton_.setBounds(l.quitButton);
    separators_ = l.separators;
}

void MainPanel::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(p.background);
    g.fillRoundedRectangle(bounds, kCornerRadius);
    g.setColour(p.outline);
    g.drawRoundedRectangle(bounds.reduced(0.5f), kCornerRadius, 1.0f);
    g.setColour(p.separator);
    for (const int y : separators_)
        g.fillRect(juce::Rectangle<float>(static_cast<float>(kPad), static_cast<float>(y),
                                          bounds.getWidth() - 2.0f * kPad, 1.0f));
}

// ---- MainPanelWindow --------------------------------------------------------------------------

MainPanelWindow::MainPanelWindow(AppController& app) : juce::TopLevelWindow("fixomic", false), panel_(app) {
    setOpaque(false);
    setAlwaysOnTop(true);
    setDropShadowEnabled(true);
    addAndMakeVisible(panel_);
    panel_.onPreferredHeightChanged = [this] {
        if (isVisible()) place();
    };
    setSize(MainPanel::kWidth, panel_.preferredHeight(MainPanel::kWidth));
}

MainPanelWindow::~MainPanelWindow() { panel_.setLive(false); }

MainPanel& MainPanelWindow::panel() { return panel_; }

int MainPanelWindow::getDesktopWindowStyleFlags() const {
    return (juce::TopLevelWindow::getDesktopWindowStyleFlags() | juce::ComponentPeer::windowIsTemporary)
           & ~juce::ComponentPeer::windowAppearsOnTaskbar;
}

void MainPanelWindow::showUnder(std::optional<juce::Rectangle<int>> anchor) {
    anchor_ = anchor;
    if (!isOnDesktop()) addToDesktop();
    place();
    setVisible(true);
    panel_.setLive(true);
    juce::Process::makeForegroundProcess();
    toFront(true);
    grabKeyboardFocus();
}

void MainPanelWindow::place() {
    const int width = MainPanel::kWidth;
    const int height = panel_.preferredHeight(width);
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto centre = anchor_ ? anchor_->getCentre() : juce::Desktop::getMousePosition();
    const auto* display = displays.getDisplayForPoint(centre.toFloat());
    if (display == nullptr) display = displays.getPrimaryDisplay();
    const auto area = (display != nullptr ? display->userBounds.getSmallestIntegerContainer()
                                          : juce::Rectangle<int>(0, 0, 1440, 900))
                          .reduced(8, 0);
    const int x = juce::jlimit(area.getX(), std::max(area.getX(), area.getRight() - width), centre.x - width / 2);
    const int y = (anchor_ ? std::max(area.getY(), anchor_->getBottom()) : area.getY()) + 6;
    setBounds(x, y, width, height);
}

void MainPanelWindow::resized() { panel_.setBounds(getLocalBounds()); }

void MainPanelWindow::dismiss() {
    if (!isVisible()) return;
    panel_.setLive(false);
    setVisible(false);
    dismissedAt_ = nowMs();
}

bool MainPanelWindow::dismissedJustNow() const { return nowMs() - dismissedAt_ < kReopenGuardMs; }

bool MainPanelWindow::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        dismiss();
        return true;
    }
    return juce::TopLevelWindow::keyPressed(key);
}

void MainPanelWindow::activeWindowStatusChanged() {
    if (isVisible() && !isActiveWindow()) dismiss();
}

} // namespace fixomic::ui
