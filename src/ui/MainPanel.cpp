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

} // namespace

// ---- MainPanel --------------------------------------------------------------------------------

struct MainPanel::Layout {
    juce::Rectangle<int> statusText, bypassLabel, bypassSwitch, route;
    juce::Rectangle<int> setupRow;
    std::vector<juce::Rectangle<int>> notices;
    juce::Rectangle<int> inputLabel, inputMeter, outputLabel, outputMeter, meterScale;
    juce::Rectangle<int> aecLabel, aecSwitch, erleLabel, erleValue;
    juce::Rectangle<int> chainTitle, chainBody;
    juce::Rectangle<int> latencyLabel, latencyValue, latencyDetail;
    juce::Rectangle<int> settingsButton, quitButton;
    juce::Rectangle<int> page;   // the settings or the guide, in the panel's place
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
    // Chain edits: the AppController applies them at once (the audio thread never waits); the
    // list is re-read once the row's own event handler has returned.
    chainList_.onMove = [this](int from, int to) {
        app_.movePlugin(from, to);
        refreshChainSoon();
    };
    chainList_.onBypassChanged = [this](int index, bool bypassed) {
        app_.setPluginBypassed(index, bypassed);
        refreshChainSoon();
    };
    chainList_.onRemove = [this](int index) {
        app_.removePlugin(index);
        refreshChainSoon();
    };
    chainList_.onOpenEditor = [this](int index) { app_.openEditor(index); };
    chainList_.onAdd = [this] { showPicker(); };
    picker_.onChoose = [this](const juce::PluginDescription& effect) {
        auto error = app_.addPlugin(effect);
        if (error.isEmpty()) {
            closePicker();
            refreshChainSoon();
        }
        return error;
    };
    picker_.onRescan = [this] {
        app_.scanPlugins();   // onModelChanged when it is done
        picker_.setScanning(app_.isScanning());
    };
    picker_.onClose = [this] { closePicker(); };

    latencyLabel_.setText("Latenza");

    settingsButton_.setButtonText(utf8("Impostazioni\u2026"));
    quitButton_.setButtonText("Esci");
    for (auto* button : {&settingsButton_, &quitButton_}) {
        button->getProperties().set(Theme::kStyleProperty, Theme::kFlatStyle);
        button->setMouseClickGrabsKeyboardFocus(false);
    }
    settingsButton_.onClick = [this] { showSettings(); };
    quitButton_.onClick = [this] {
        if (onQuit) onQuit();
    };

    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &statusText_, &bypassLabel_, &bypassSwitch_, &route_, &inputLabel_, &outputLabel_, &inputMeter_,
             &outputMeter_, &meterScale_, &aecLabel_, &aecSwitch_, &erleLabel_, &erleValue_, &chainTitle_,
             &chainList_, &latencyLabel_, &latencyValue_, &latencyDetail_, &settingsButton_, &quitButton_})
        addAndMakeVisible(c);
    addChildComponent(picker_);

    setupRow_ = std::make_unique<NoticeRow>(utf8("La configurazione iniziale non \u00e8 completa."),
                                            "Completa la configurazione", [this] { showOnboarding(); });
    addChildComponent(*setupRow_);
    addChildComponent(settings_);
    addChildComponent(onboarding_);
    wirePages();

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

    refreshChain();
    updateRoute();

    inputMeter_.setInk(status.kind, running);
    outputMeter_.setInk(status.kind, running);

    // What global bypass skips recedes; it stays operable.
    for (juce::Component* c : std::initializer_list<juce::Component*>{&aecLabel_, &aecSwitch_, &erleLabel_,
                                                                      &erleValue_, &chainTitle_, &chainList_, &picker_})
        dim(*c, bypassed);

    showLatency(app_.latency());

    const auto notices = collectNotices(app_.lastWarning(), app_.lastWarningPane(), app_.startupWarnings());
    if (notices != notices_) rebuildNotices(notices);
    setupReminder_ = !config.onboardingDone;
    refreshPage();
    updateVisibility();

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
        addChildComponent(*row);
        row->setVisible(page_ == Page::Main);
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

void MainPanel::updateRoute() {
    const auto& slots = chainList_.slots();
    const bool chainProcessing = std::any_of(slots.begin(), slots.end(), [](const PluginChain::SlotInfo& slot) {
        return !slot.missing && !slot.bypassed;
    });
    route_.setRoute(routeFor(status_, {app_.config().aecEnabled, app_.lastWarning().isNotEmpty(), chainProcessing}));
}

void MainPanel::showLatency(const LatencyBreakdown& latency) {
    latencyLabel_.setText(app_.engineState() == EngineState::Running ? "Latenza" : "Latenza stimata");
    latencyValue_.setText(formatLatencyTotal(latency));
    latencyDetail_.setText(formatLatencyDetail(latency));
}

void MainPanel::showChain(std::vector<PluginChain::SlotInfo> slots, const LatencyBreakdown& latency) {
    chainList_.setSlots(std::move(slots));
    updateRoute();
    showLatency(latency);
    heightMayHaveChanged();
    resized();
    repaint();
}

void MainPanel::refreshChain() {
    chainList_.setSlots(app_.slots());
    if (pickerOpen_) {
        picker_.setEffects(app_.effects());
        picker_.setScanning(app_.isScanning());
    }
}

void MainPanel::refreshChainSoon() {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainPanel>(this)] {
        if (safe != nullptr) safe->refreshChain();
    });
}

void MainPanel::showPicker() {
    if (page_ != Page::Main) setPage(Page::Main);
    if (!pickerOpen_) {
        pickerOpen_ = true;
        picker_.reset();
        refreshChain();
        updateVisibility();
        heightMayHaveChanged();
        resized();
    }
    scrollIntoView(picker_);
    picker_.focusSearch();
}

bool MainPanel::closePicker() {
    if (!pickerOpen_) return false;
    pickerOpen_ = false;
    releaseFocus(picker_);
    updateVisibility();
    picker_.reset();
    heightMayHaveChanged();
    resized();
    return true;
}

// Hidden with the keyboard focus, a view would pass it to this panel, which gives it to its first
// focusable control, the global bypass switch, and Return (just pressed in the view) would turn
// processing off: the window takes it first.
void MainPanel::releaseFocus(juce::Component& leaving) {
    if (!leaving.hasKeyboardFocus(true)) return;
    auto* window = getTopLevelComponent();
    if (window != this && window->getWantsKeyboardFocus())
        window->grabKeyboardFocus();
    else
        leaving.giveAwayKeyboardFocus();
}

void MainPanel::updateVisibility() {
    const bool main = page_ == Page::Main;
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &statusText_, &bypassLabel_, &bypassSwitch_, &route_, &inputLabel_, &outputLabel_, &inputMeter_,
             &outputMeter_, &meterScale_, &aecLabel_, &aecSwitch_, &erleLabel_, &erleValue_, &latencyLabel_,
             &latencyValue_, &latencyDetail_, &settingsButton_, &quitButton_})
        c->setVisible(main);
    for (auto& row : noticeRows_) row->setVisible(main);
    setupRow_->setVisible(main && setupReminder_);
    chainTitle_.setVisible(main && !pickerOpen_);
    chainList_.setVisible(main && !pickerOpen_);
    picker_.setVisible(main && pickerOpen_);
    settings_.setVisible(page_ == Page::Settings);
    onboarding_.setVisible(page_ == Page::Onboarding);
}

// ---- settings and first-run guide -------------------------------------------------------------

void MainPanel::wirePages() {
    settings_.onClose = [this] { closePage(); };
    settings_.onChooseDevice = [this](const DeviceChoice& device) { chooseDevice(device); };
    settings_.onChooseChannel = [this](int channel) {
        app_.updateConfig([channel](Config& c) { c.inputChannel = channel; });
    };
    // Applied at once: the engine reopens the IO, and the AEC and the plugins are prepared for the
    // new size while it is closed (MacEngineBackend::openIO).
    settings_.onChooseBuffer = [this](int size) { app_.updateConfig([size](Config& c) { c.bufferSize = size; }); };
    settings_.onSetLaunchAtLogin = [this](bool enabled) {
        juce::String error;
        app_.setLaunchAtLogin(enabled, error);
        refreshPage();   // the switch shows the system state
        return error;
    };
    settings_.onInstallDriver = [this] {
        app_.installDriver([safe = juce::Component::SafePointer<MainPanel>(this)](bool ok, juce::String error) {
            if (safe != nullptr) safe->settings_.driverOperationFinished(ok, error);
        });
        refreshPage();   // in progress
    };
    settings_.onUninstallDriver = [this] {
        app_.uninstallDriver([safe = juce::Component::SafePointer<MainPanel>(this)](bool ok, juce::String error) {
            if (safe != nullptr) safe->settings_.driverOperationFinished(ok, error);
        });
        refreshPage();
    };
    settings_.onOpenPane = [this](mac::PrivacyPane pane) { app_.openPrivacySettings(pane); };
    settings_.onReviewSetup = [this] { showOnboarding(); };

    onboarding_.onClose = [this] { closePage(); };   // closed early: onboardingDone stays false
    onboarding_.onRequestMicrophone = [this] {
        // The answer re-evaluates the engine and notifies; the prompt's pending state shows at once.
        app_.requestMicrophone([](bool) {});
    };
    onboarding_.onRequestSystemAudio = [this] { app_.requestSystemAudio([](bool) {}); };
    onboarding_.onChooseDevice = [this](const DeviceChoice& device) { chooseDevice(device); };
    onboarding_.onOpenPane = [this](mac::PrivacyPane pane) { app_.openPrivacySettings(pane); };
    onboarding_.onInstallDriver = [this] {
        app_.installDriver([safe = juce::Component::SafePointer<MainPanel>(this)](bool ok, juce::String error) {
            if (safe != nullptr) safe->onboarding_.driverOperationFinished(ok, error);
        });
        refreshPage();
    };
    onboarding_.onFinish = [this] {
        app_.updateConfig([](Config& c) { c.onboardingDone = true; });
        closePage();
    };
}

void MainPanel::chooseDevice(const DeviceChoice& device) {
    app_.updateConfig([uid = device.uid, name = device.name](Config& c) {
        c.inputDeviceUid = uid;
        c.inputDeviceName = name;
        c.inputChannel = 0;
    });
}

DriverFacts MainPanel::driverFacts() const {
    return {app_.driverInstalled(), app_.installedDriverVersion(), app_.bundledDriverVersion(),
            app_.driverUpdateAvailable(), app_.driverOperationInProgress()};
}

SettingsPanel::Data MainPanel::settingsData() const {
    const auto& config = app_.config();
    SettingsPanel::Data data;
    data.devices = deviceChoices(app_.inputDevices(), config.inputDeviceUid, config.inputDeviceName);
    data.channel = config.inputChannel;
    data.bufferSize = config.bufferSize;
    data.launchAtLogin = app_.launchAtLogin();
    data.driver = driverFacts();
    data.microphone = app_.microphonePermission();
    data.systemAudio = app_.systemAudioPermission();
    data.warning = app_.lastWarningKind();
    return data;
}

Onboarding::Data MainPanel::onboardingData() const {
    const auto& config = app_.config();
    Onboarding::Data data;
    data.devices = deviceChoices(app_.inputDevices(), config.inputDeviceUid, config.inputDeviceName);
    data.driver = driverFacts();
    auto& f = data.facts;
    f.microphone = app_.microphonePermission();
    f.microphoneRequestPending = app_.microphoneRequestPending();
    f.microphoneChosen = config.inputDeviceUid.isNotEmpty();
    f.systemAudio = app_.systemAudioPermission();
    f.systemAudioRequestPending = app_.systemAudioRequestPending();
    f.systemAudioAnswered = app_.systemAudioAnswered();
    f.warning = app_.lastWarningKind();
    f.driverInstalled = data.driver.installed;
    f.driverOperationInProgress = data.driver.operationInProgress;
    f.driverBundled = data.driver.bundledVersion.isNotEmpty();
    return data;
}

void MainPanel::refreshPage() {
    switch (page_) {
    case Page::Settings: settings_.setData(settingsData()); break;
    case Page::Onboarding: onboarding_.setData(onboardingData()); break;
    case Page::Main: break;
    }
}

void MainPanel::showSettings() {
    closePicker();
    settings_.reset();
    setPage(Page::Settings);
}

void MainPanel::showOnboarding() {
    closePicker();
    if (page_ != Page::Onboarding) onboardingBack_ = page_ == Page::Settings ? Page::Settings : Page::Main;
    onboarding_.restart();
    setPage(Page::Onboarding);
}

bool MainPanel::closePage() {
    if (page_ == Page::Main) return false;
    setPage(page_ == Page::Onboarding ? onboardingBack_ : Page::Main);
    return true;
}

void MainPanel::setPage(Page page) {
    if (page == page_) {
        refreshPage();
        return;
    }
    releaseFocus(*this);
    page_ = page;
    refreshPage();
    updateVisibility();
    heightMayHaveChanged();
    resized();
    repaint();
    if (auto* viewport = findParentComponentOfClass<juce::Viewport>()) viewport->setViewPosition(0, 0);
}

MainPanel::Page MainPanel::page() const { return page_; }
SettingsPanel& MainPanel::settings() { return settings_; }
Onboarding& MainPanel::onboarding() { return onboarding_; }

bool MainPanel::holdsOpen() const {
    return page_ == Page::Onboarding || (page_ == Page::Settings && settings_.driverOperationStarted());
}

bool MainPanel::showsSetupReminder() const { return setupRow_->isVisible(); }

bool MainPanel::isPickerOpen() const { return pickerOpen_; }

ChainList& MainPanel::chainList() { return chainList_; }

PluginPicker& MainPanel::picker() { return picker_; }

// In a panel clamped to the display (scrolling in its window), brings `component` into view.
void MainPanel::scrollIntoView(const juce::Component& component) {
    auto* viewport = findParentComponentOfClass<juce::Viewport>();
    if (viewport == nullptr) return;
    const auto area = viewport->getViewArea();
    const auto bounds = component.getBounds().expanded(0, kGroupGap);
    if (area.contains(bounds)) return;
    viewport->setViewPosition(0, std::min(bounds.getY(), std::max(area.getY(), bounds.getBottom() - area.getHeight())));
}

void MainPanel::childBoundsChanged(juce::Component* child) {
    if (child != &chainList_ && child != &picker_ && child != &settings_ && child != &onboarding_) return;
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
    // The views in the panel's place (and the chain section) reach past the content edges by the
    // flat buttons' inset, so their text lines up with it.
    const int sectionX = x - Theme::kFlatTextInset;
    const int sectionWidth = contentWidth + 2 * Theme::kFlatTextInset;
    if (page_ != Page::Main) {
        const int h = page_ == Page::Settings ? settings_.heightForWidth(sectionWidth)
                                              : onboarding_.heightForWidth(sectionWidth);
        l.page = {sectionX, y, sectionWidth, h};
        l.height = y + h + 14;
        return l;
    }
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

    if (setupReminder_) {
        y += 10;
        l.setupRow = {x, y, contentWidth, setupRow_->heightForWidth(contentWidth)};
        y += l.setupRow.getHeight();
    }
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

    // Chain: "Catena" over the list, or the picker (with its own header) in their place.
    if (pickerOpen_) {
        l.chainBody = {sectionX, y, sectionWidth, picker_.heightForWidth(sectionWidth)};
    } else {
        l.chainTitle = {x, y, contentWidth, 18};
        y += 18 + 4;
        l.chainBody = {sectionX, y, sectionWidth, chainList_.heightForWidth(sectionWidth)};
    }
    y += l.chainBody.getHeight();
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
        return juce::GlyphArrangement::getStringWidthInt(Theme::font(TextStyle::Body), button.getButtonText())
               + 2 * Theme::kFlatTextInset;
    };
    const int quitWidth = footerButton(quitButton_);
    l.settingsButton = {x - Theme::kFlatTextInset, y, footerButton(settingsButton_), 28};
    l.quitButton = {x + contentWidth + Theme::kFlatTextInset - quitWidth, y, quitWidth, 28};
    y += 28 + 8;

    l.height = y;
    return l;
}

void MainPanel::resized() {
    const auto l = layOut(getWidth());
    separators_ = l.separators;
    if (page_ != Page::Main) {
        (page_ == Page::Settings ? static_cast<juce::Component&>(settings_) : onboarding_).setBounds(l.page);
        return;
    }
    statusText_.setBounds(l.statusText);
    bypassLabel_.setBounds(l.bypassLabel.withTrimmedTop(4));
    bypassSwitch_.setBounds(l.bypassSwitch);
    route_.setBounds(l.route);
    setupRow_->setBounds(l.setupRow);
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
    (pickerOpen_ ? static_cast<juce::Component&>(picker_) : chainList_).setBounds(l.chainBody);
    latencyLabel_.setBounds(l.latencyLabel);
    latencyValue_.setBounds(l.latencyValue);
    latencyDetail_.setBounds(l.latencyDetail);
    settingsButton_.setBounds(l.settingsButton);
    quitButton_.setBounds(l.quitButton);
}

void MainPanel::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    // The ground is the visible part: the whole panel, or the part a clamped window scrolls to.
    auto ground = getLocalBounds().toFloat();
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
        ground = viewport->getViewArea().toFloat();
    g.setColour(p.background);
    g.fillRoundedRectangle(ground, kCornerRadius);
    g.setColour(p.outline);
    g.drawRoundedRectangle(ground.reduced(0.5f), kCornerRadius, 1.0f);
    g.setColour(p.separator);
    for (const int y : separators_)
        g.fillRect(juce::Rectangle<float>(static_cast<float>(kPad), static_cast<float>(y),
                                          static_cast<float>(getWidth()) - 2.0f * kPad, 1.0f));
}

juce::Rectangle<int> panelWindowBounds(std::optional<juce::Rectangle<int>> anchor, juce::Point<int> mouse,
                                       juce::Rectangle<int> userArea, int width, int preferredHeight) {
    constexpr int kScreenMargin = 8, kBelowMenuBar = 6, kMinHeight = 160;
    const auto area = userArea.reduced(kScreenMargin, 0);
    const auto centre = anchor ? anchor->getCentre() : mouse;
    const int x = juce::jlimit(area.getX(), std::max(area.getX(), area.getRight() - width), centre.x - width / 2);
    const int y = (anchor ? std::max(area.getY(), anchor->getBottom()) : area.getY()) + kBelowMenuBar;
    const int available = area.getBottom() - kScreenMargin - y;
    const int height = std::min(preferredHeight, std::max(available, kMinHeight));
    return {x, y, width, height};
}

// ---- MainPanelWindow --------------------------------------------------------------------------

MainPanelWindow::MainPanelWindow(AppController& app) : juce::TopLevelWindow("fixomic", false), panel_(app) {
    setOpaque(false);
    setAlwaysOnTop(true);
    setDropShadowEnabled(true);
    // The panel keeps its full height; the window is clamped to the display (panelWindowBounds) and
    // the viewport then scrolls it with the trackpad or wheel, without a scrollbar.
    viewport_.setScrollBarsShown(false, false, true, false);
    viewport_.setViewedComponent(&panel_, false);
    viewport_.setWantsKeyboardFocus(false);
    addAndMakeVisible(viewport_);
    panel_.onPreferredHeightChanged = [this] {
        fitPanel();
        if (isVisible()) place();
    };
    fitPanel();
    setSize(MainPanel::kWidth, panel_.getHeight());
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
    fitPanel();
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto mouse = juce::Desktop::getMousePosition();
    const auto* display = displays.getDisplayForPoint((anchor_ ? anchor_->getCentre() : mouse).toFloat());
    if (display == nullptr) display = displays.getPrimaryDisplay();
    const auto userArea = display != nullptr ? display->userBounds.getSmallestIntegerContainer()
                                             : juce::Rectangle<int>(0, 0, 1440, 900);
    setBounds(panelWindowBounds(anchor_, mouse, userArea, MainPanel::kWidth, panel_.getHeight()));
}

void MainPanelWindow::fitPanel() { panel_.setSize(MainPanel::kWidth, panel_.preferredHeight(MainPanel::kWidth)); }

void MainPanelWindow::resized() { viewport_.setBounds(getLocalBounds()); }

void MainPanelWindow::dismiss() {
    if (!isVisible()) return;
    panel_.setLive(false);
    setVisible(false);
    panel_.chainList().cancelDrag();
    panel_.closePicker();   // it reopens on the chain
    while (panel_.closePage()) {}   // and on the panel: the guide, closed early, is not done
    dismissedAt_ = nowMs();
}

bool MainPanelWindow::dismissedJustNow() const { return nowMs() - dismissedAt_ < kReopenGuardMs; }

bool MainPanelWindow::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        // One step back per Escape: a drag in progress, the plugin picker, the settings or the guide,
        // then the panel.
        if (!panel_.chainList().cancelDrag() && !panel_.closePicker() && !panel_.closePage()) dismiss();
        return true;
    }
    return juce::TopLevelWindow::keyPressed(key);
}

void MainPanelWindow::activeWindowStatusChanged() {
    if (isVisible()) activeStatusChanged(isActiveWindow());
}

void MainPanelWindow::activeStatusChanged(bool active) {
    if (active)
        panel_.refreshPage();   // back from a prompt or System Settings
    else if (!panel_.holdsOpen())
        dismiss();
}

} // namespace fixomic::ui
