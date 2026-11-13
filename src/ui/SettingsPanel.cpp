#include "ui/SettingsPanel.h"

#include <algorithm>
#include <utility>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int kInset = Theme::kFlatTextInset;   // bounds past the content edge, each side
constexpr int kHeaderHeight = 24;
constexpr int kGroupGap = 12;
constexpr int kSwitchWidth = 40;                // as MainPanel's switches
constexpr int kSwitchHeight = 24;
constexpr int kButtonHeight = 24;               // push buttons
constexpr int kButtonGap = 8;
constexpr int kRowHeight = 20;                  // a label and its value
constexpr int kRateWidth = 70;                  // "48 kHz"
constexpr int kPermissionWidth = 150;           // "Non ancora chiesto", the longest

int pushButtonWidth(const juce::TextButton& button) { return Theme::pushButtonWidth(button.getButtonText()); }
int flatButtonWidth(const juce::TextButton& button) { return Theme::flatButtonWidth(button); }

void setUp(juce::Button& button, const juce::String& text, const juce::String& accessibleTitle = {}) {
    button.setButtonText(text);
    if (accessibleTitle.isNotEmpty()) button.setTitle(accessibleTitle);
    button.setMouseClickGrabsKeyboardFocus(false);
}

} // namespace

struct SettingsPanel::Layout {
    juce::Rectangle<int> back, title;
    juce::Rectangle<int> micTitle, devices, noDevices, channelLabel, channels;
    juce::Rectangle<int> rateLabel, rateValue, rateWarning, bufferLabel, buffers;
    juce::Rectangle<int> loginLabel, loginSwitch, loginError;
    juce::Rectangle<int> driverTitle, driverStatus, install, uninstall, confirmText, confirmUninstall, cancelUninstall,
        driverMessage;
    juce::Rectangle<int> permissionsTitle, micPermissionLabel, micPermission, micAsk, micSettings, audioPermissionLabel,
        audioPermission, audioNote, audioSettings;
    juce::Rectangle<int> review;
    std::vector<int> separators;
    int height = 0;
};

SettingsPanel::SettingsPanel() {
    setOpaque(false);
    setTitle("Impostazioni");
    setWantsKeyboardFocus(false);

    back_.onClick = [this] {
        if (onClose) onClose();
    };
    title_.setText("Impostazioni");

    micTitle_.setText("Microfono");
    noDevices_.setText("Nessun microfono collegato.");
    devices_.onChoose = [this](int index) {
        if (index < 0 || index >= static_cast<int>(data_.devices.size())) return;
        const auto& choice = data_.devices[static_cast<size_t>(index)];
        if (!choice.selected && onChooseDevice) onChooseDevice(choice);
    };
    channelLabel_.setText("Canale");
    channels_.onChoose = [this](int channel) {
        if (channel != data_.channel && onChooseChannel) onChooseChannel(channel);
    };

    rateLabel_.setText("Frequenza di campionamento");
    rateValue_.setText("48 kHz");
    bufferLabel_.setText("Buffer");
    buffers_.onChoose = [this](int index) {
        const int size = kBufferSizes[static_cast<size_t>(juce::jlimit(0, static_cast<int>(kBufferSizes.size()) - 1, index))];
        if (size != data_.bufferSize && onChooseBuffer) onChooseBuffer(size);
    };

    loginLabel_.setText(utf8("Avvio all’accesso"));
    setUp(loginSwitch_, utf8("Avvio all’accesso"));
    loginSwitch_.onClick = [this] { setLaunchAtLogin(); };

    driverTitle_.setText("Driver");
    setUp(install_, "Installa driver");
    install_.onClick = [this] { startInstall(); };
    setUp(uninstall_, kUninstallDriverText);
    uninstall_.onClick = [this] {
        confirming_ = true;
        finished_ = Operation::None;
        driverError_.clear();
        resultUnseen_ = false;
        update();
    };
    confirmText_.setText(utf8("Disinstallare il driver? fixomic Mic sparirà da tutte le app finché non lo "
                              "reinstalli."));
    setUp(confirmUninstall_, "Disinstalla", "Conferma: disinstalla il driver");
    confirmUninstall_.onClick = [this] { startUninstall(); };
    setUp(cancelUninstall_, "Annulla", "Annulla la disinstallazione");
    cancelUninstall_.onClick = [this] {
        confirming_ = false;
        update();
    };

    permissionsTitle_.setText("Permessi");
    micPermissionLabel_.setText("Microfono");
    setUp(micAsk_, "Consenti il microfono");
    micAsk_.onClick = [this] {
        if (onRequestMicrophone) onRequestMicrophone();
    };
    setUp(micSettings_, "Apri Impostazioni di Sistema", "Apri Impostazioni di Sistema: microfono");
    micSettings_.onClick = [this] {
        if (onOpenPane) onOpenPane(mac::PrivacyPane::Microphone);
    };
    audioPermissionLabel_.setText("Audio di sistema");
    setUp(audioSettings_, "Apri Impostazioni di Sistema", "Apri Impostazioni di Sistema: audio di sistema");
    audioSettings_.onClick = [this] {
        if (onOpenPane) onOpenPane(mac::PrivacyPane::SystemAudio);
    };

    setUp(review_, "Rivedi la configurazione iniziale");
    review_.getProperties().set(Theme::kStyleProperty, Theme::kFlatStyle);
    review_.onClick = [this] {
        if (onReviewSetup) onReviewSetup();
    };

    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &back_, &title_, &micTitle_, &rateLabel_, &rateValue_, &bufferLabel_, &buffers_, &loginLabel_,
             &loginSwitch_, &driverTitle_, &driverStatus_, &permissionsTitle_, &micPermissionLabel_, &micPermission_,
             &audioPermissionLabel_, &audioPermission_, &review_})
        addAndMakeVisible(c);
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &devices_, &noDevices_, &channelLabel_, &channels_, &rateWarning_, &loginErrorText_, &install_, &uninstall_,
             &confirmText_, &confirmUninstall_, &cancelUninstall_, &driverMessage_, &micAsk_, &micSettings_, &audioNote_,
             &audioSettings_})
        addChildComponent(c);

    update();
}

SettingsPanel::~SettingsPanel() = default;

// ---- data -------------------------------------------------------------------------------------

void SettingsPanel::setData(const Data& data) {
    if (data == data_) return;
    data_ = data;
    if (!canUninstallDriver(data_.driver)) confirming_ = false;   // nothing left to remove
    update();
}

const SettingsPanel::Data& SettingsPanel::data() const { return data_; }

void SettingsPanel::reset() {
    // A running operation stays tracked: its result shows when the view is opened again.
    confirming_ = false;
    if (!std::exchange(resultUnseen_, false)) {   // seen: gone; unseen: it shows now, then goes
        finished_ = Operation::None;
        driverError_.clear();
    }
    loginError_.clear();
    update();
}

void SettingsPanel::update() {
    // Microfono.
    std::vector<ChoiceList::Item> items;
    for (const auto& device : data_.devices) items.push_back({device.text, device.selected, !device.connected});
    devices_.setItems(std::move(items));
    devices_.setVisible(!data_.devices.empty());
    noDevices_.setVisible(data_.devices.empty());
    const auto selected = selectedDevice(data_.devices);
    const auto channels = selected && selected->connected ? channelChoices(selected->inputChannels) : juce::StringArray();
    channels_.setOptions(channels, juce::jlimit(0, std::max(0, channels.size() - 1), data_.channel));
    channels_.setVisible(!channels.isEmpty());
    channelLabel_.setVisible(!channels.isEmpty());

    // Frequenza, buffer.
    rateWarning_.setText(sampleRateWarning(data_.devices));
    rateWarning_.setVisible(rateWarning_.text().isNotEmpty());
    juce::StringArray buffers;
    int bufferIndex = -1;
    for (size_t i = 0; i < kBufferSizes.size(); ++i) {
        buffers.add(formatBufferChoice(kBufferSizes[i]));
        if (kBufferSizes[i] == data_.bufferSize) bufferIndex = static_cast<int>(i);
    }
    buffers_.setOptions(buffers, bufferIndex);

    // Avvio all'accesso.
    loginSwitch_.setToggleState(data_.launchAtLogin, juce::dontSendNotification);
    loginErrorText_.setText(loginError_);
    loginErrorText_.setVisible(loginError_.isNotEmpty());

    // Driver.
    const auto& driver = data_.driver;
    const bool busy = driver.operationInProgress || operation_ != Operation::None;
    driverStatus_.setText(driverStatusText(driver));
    install_.setButtonText(driverInstallActionText(driver));
    install_.setVisible(!confirming_ && install_.getButtonText().isNotEmpty());
    uninstall_.setVisible(!confirming_ && canUninstallDriver(driver));
    for (auto* button : {&install_, &uninstall_, &confirmUninstall_, &cancelUninstall_}) button->setEnabled(!busy);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&confirmText_, &confirmUninstall_, &cancelUninstall_})
        c->setVisible(confirming_);
    juce::String message;
    auto ink = TextBlock::Ink::Secondary;
    if (busy) {
        message = utf8("Operazione in corso…");
    } else if (finished_ != Operation::None && driverError_.isNotEmpty()) {
        message = driverError_;
        ink = TextBlock::Ink::Problem;
    } else if (finished_ == Operation::Install) {
        message = "Driver installato.";
    } else if (finished_ == Operation::Update) {
        message = "Driver aggiornato.";
    } else if (finished_ == Operation::Uninstall) {
        message = "Driver disinstallato.";
    } else if (!driver.installed && driver.bundledVersion.isEmpty()) {
        message = "Questa copia di fixomic non contiene il driver.";
    }
    driverMessage_.setText(message);
    driverMessage_.setInk(ink);
    driverMessage_.setVisible(message.isNotEmpty());

    // Permessi.
    micPermission_.setText(microphonePermissionText(data_.microphone));
    const auto micAction = microphoneSettingsAction(data_.microphone);
    micAsk_.setVisible(micAction == StepAction::Ask);
    micAsk_.setEnabled(!data_.microphoneRequestPending);
    micSettings_.setVisible(micAction == StepAction::OpenSettings);
    audioPermission_.setText(systemAudioPermissionText(data_.systemAudio, data_.warning));
    juce::String note;
    if (data_.systemAudio == mac::Permission::NotDetermined)
        note = data_.warning == mac::WarningKind::SystemAudioSilent
                   ? utf8("fixomic riceve solo silenzio dall’audio di sistema: se l’eco resta, controlla in "
                          "Impostazioni di Sistema.")
                   : utf8("macOS non dice alle app se l’hai consentito: se l’eco resta, controlla in "
                          "Impostazioni di Sistema.");
    audioNote_.setText(note);
    audioNote_.setVisible(note.isNotEmpty());
    audioSettings_.setVisible(systemAudioSettingsAction(data_.systemAudio) == StepAction::OpenSettings);

    fitHeight();
    resized();
    repaint();
}

// ---- actions ----------------------------------------------------------------------------------

void SettingsPanel::startInstall() {
    if (data_.driver.operationInProgress || operation_ != Operation::None) return;
    operation_ = data_.driver.installed ? Operation::Update : Operation::Install;
    finished_ = Operation::None;
    driverError_.clear();
    resultUnseen_ = false;
    confirming_ = false;
    update();
    if (onInstallDriver) onInstallDriver();
}

void SettingsPanel::startUninstall() {
    if (data_.driver.operationInProgress || operation_ != Operation::None) return;
    confirming_ = false;
    operation_ = Operation::Uninstall;
    finished_ = Operation::None;
    driverError_.clear();
    resultUnseen_ = false;
    update();
    if (onUninstallDriver) onUninstallDriver();
}

void SettingsPanel::driverOperationFinished(bool ok, const juce::String& error) {
    finished_ = operation_ != Operation::None ? operation_ : Operation::Install;
    operation_ = Operation::None;
    driverError_ = ok ? juce::String() : (error.isNotEmpty() ? error : juce::String("Operazione non riuscita."));
    resultUnseen_ = !isVisible();   // the panel was closed meanwhile: keep it for the next opening
    update();
}

bool SettingsPanel::driverOperationStarted() const { return operation_ != Operation::None; }

void SettingsPanel::setLaunchAtLogin() {
    const bool enabled = loginSwitch_.getToggleState();
    loginError_ = onSetLaunchAtLogin ? onSetLaunchAtLogin(enabled) : juce::String();
    update();   // the switch shows the system state: back where it was if that did not work
}

bool SettingsPanel::confirmingUninstall() const { return confirming_; }
juce::String SettingsPanel::driverMessage() const { return driverMessage_.isVisible() ? driverMessage_.text() : juce::String(); }
juce::String SettingsPanel::loginError() const { return loginError_; }
juce::String SettingsPanel::sampleRateText() const { return rateWarning_.isVisible() ? rateWarning_.text() : juce::String(); }

bool SettingsPanel::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (onClose) onClose();
        return true;
    }
    return false;
}

// ---- layout -----------------------------------------------------------------------------------

SettingsPanel::Layout SettingsPanel::layOut(int width) const {
    Layout l;
    const int x = kInset;
    const int content = width - 2 * kInset;
    int y = 0;
    const auto separator = [&] {
        y += kGroupGap;
        l.separators.push_back(y);
        y += 1 + kGroupGap;
    };
    const auto labelAndValue = [&](juce::Rectangle<int>& label, juce::Rectangle<int>& value, int valueWidth) {
        label = {x, y, content - valueWidth - 8, kRowHeight};
        value = {x + content - valueWidth, y, valueWidth, kRowHeight};
        y += kRowHeight;
    };
    const auto text = [&](juce::Rectangle<int>& bounds, const TextBlock& block, int gapBefore) {
        y += gapBefore;
        bounds = {x, y, content, block.heightForWidth(content)};
        y += bounds.getHeight();
    };
    const auto pushButton = [&](juce::Rectangle<int>& bounds, const juce::TextButton& button) {
        y += 4;
        bounds = {x, y, std::min(content, pushButtonWidth(button)), kButtonHeight};
        y += kButtonHeight;
    };

    // Header: the chevron's stroke starts at the content edge.
    l.back = {kInset - 8, 1, GlyphButton::kSize, GlyphButton::kSize};
    l.title = {l.back.getRight() + 2, 3, content, kHeaderHeight - 3};
    y = kHeaderHeight;
    separator();

    // Microfono.
    l.micTitle = {x, y, content, 18};
    y += 18 + 4;
    if (devices_.isVisible()) {
        l.devices = {0, y, width, devices_.heightForWidth(width)};
        y += l.devices.getHeight();
    } else {
        text(l.noDevices, noDevices_, 0);
    }
    if (channels_.isVisible()) {
        y += 8;
        l.channelLabel = {x, y, content, 18};
        y += 18 + 6;
        l.channels = {x, y, content, channels_.heightForWidth(content)};
        y += l.channels.getHeight();
    }
    separator();

    // Frequenza di campionamento, buffer.
    labelAndValue(l.rateLabel, l.rateValue, kRateWidth);
    if (rateWarning_.isVisible()) text(l.rateWarning, rateWarning_, 2);
    y += kGroupGap;
    l.bufferLabel = {x, y, content, 18};
    y += 18 + 6;
    l.buffers = {x, y, content, buffers_.heightForWidth(content)};
    y += l.buffers.getHeight();
    separator();

    // Avvio all'accesso.
    l.loginLabel = {x, y + 3, content - kSwitchWidth - 8, kRowHeight};
    l.loginSwitch = {x + content - kSwitchWidth, y, kSwitchWidth, kSwitchHeight};
    y += kSwitchHeight;
    if (loginErrorText_.isVisible()) text(l.loginError, loginErrorText_, 4);
    separator();

    // Driver.
    l.driverTitle = {x, y, 80, 18};
    l.driverStatus = {x + 80, y, content - 80, 18};
    y += 18;
    if (confirming_) {
        text(l.confirmText, confirmText_, 8);
        y += 6;
        l.confirmUninstall = {x, y, pushButtonWidth(confirmUninstall_), kButtonHeight};
        l.cancelUninstall = {l.confirmUninstall.getRight() + kButtonGap, y, pushButtonWidth(cancelUninstall_),
                             kButtonHeight};
        y += kButtonHeight;
    } else if (install_.isVisible() || uninstall_.isVisible()) {
        // Left to right; the second one goes under the first when they do not fit side by side.
        y += 8;
        int left = x;
        bool first = true;
        for (auto [button, bounds] : {std::pair{&install_, &l.install}, std::pair{&uninstall_, &l.uninstall}}) {
            if (!button->isVisible()) continue;
            const int w = std::min(content, pushButtonWidth(*button));
            if (!first && left + w > x + content) {
                left = x;
                y += kButtonHeight + 6;
            }
            *bounds = {left, y, w, kButtonHeight};
            left += w + kButtonGap;
            first = false;
        }
        y += kButtonHeight;
    }
    if (driverMessage_.isVisible()) text(l.driverMessage, driverMessage_, 6);
    separator();

    // Permessi.
    l.permissionsTitle = {x, y, content, 18};
    y += 18 + 6;
    labelAndValue(l.micPermissionLabel, l.micPermission, kPermissionWidth);
    if (micAsk_.isVisible()) pushButton(l.micAsk, micAsk_);
    if (micSettings_.isVisible()) pushButton(l.micSettings, micSettings_);
    y += 8;
    labelAndValue(l.audioPermissionLabel, l.audioPermission, kPermissionWidth);
    if (audioNote_.isVisible()) text(l.audioNote, audioNote_, 2);
    if (audioSettings_.isVisible()) pushButton(l.audioSettings, audioSettings_);
    separator();
    y -= kGroupGap - 4;   // the flat button carries its own padding

    l.review = {0, y, flatButtonWidth(review_), 28};
    y += 28;
    l.height = y;
    return l;
}

int SettingsPanel::heightForWidth(int width) const { return layOut(width).height; }

void SettingsPanel::fitHeight() {
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));   // before the first layout: nothing to fit
}

void SettingsPanel::resized() {
    const auto l = layOut(getWidth());
    const std::pair<juce::Component*, juce::Rectangle<int>> bounds[] = {
        {&back_, l.back},
        {&title_, l.title},
        {&micTitle_, l.micTitle},
        {&devices_, l.devices},
        {&noDevices_, l.noDevices},
        {&channelLabel_, l.channelLabel},
        {&channels_, l.channels},
        {&rateLabel_, l.rateLabel},
        {&rateValue_, l.rateValue},
        {&rateWarning_, l.rateWarning},
        {&bufferLabel_, l.bufferLabel},
        {&buffers_, l.buffers},
        {&loginLabel_, l.loginLabel},
        {&loginSwitch_, l.loginSwitch},
        {&loginErrorText_, l.loginError},
        {&driverTitle_, l.driverTitle},
        {&driverStatus_, l.driverStatus},
        {&install_, l.install},
        {&uninstall_, l.uninstall},
        {&confirmText_, l.confirmText},
        {&confirmUninstall_, l.confirmUninstall},
        {&cancelUninstall_, l.cancelUninstall},
        {&driverMessage_, l.driverMessage},
        {&permissionsTitle_, l.permissionsTitle},
        {&micPermissionLabel_, l.micPermissionLabel},
        {&micPermission_, l.micPermission},
        {&micAsk_, l.micAsk},
        {&micSettings_, l.micSettings},
        {&audioPermissionLabel_, l.audioPermissionLabel},
        {&audioPermission_, l.audioPermission},
        {&audioNote_, l.audioNote},
        {&audioSettings_, l.audioSettings},
        {&review_, l.review},
    };
    for (const auto& [component, area] : bounds) component->setBounds(area);
    separators_ = l.separators;
}

void SettingsPanel::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    g.setColour(p.separator);
    for (const int y : separators_)
        g.fillRect(juce::Rectangle<float>(static_cast<float>(kInset), static_cast<float>(y),
                                          static_cast<float>(getWidth() - 2 * kInset), 1.0f));
}

} // namespace fixomic::ui
