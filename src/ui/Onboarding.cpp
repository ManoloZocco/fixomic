#include "ui/Onboarding.h"

#include <algorithm>
#include <utility>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int kInset = Theme::kFlatTextInset;   // bounds past the content edge, each side
constexpr int kHeaderHeight = 24;
constexpr int kMarkerSize = 18;                 // a station
constexpr int kTextX = kInset + kMarkerSize + 10;   // the steps' text, right of the stations
constexpr int kTitleHeight = 20;
constexpr int kStepGap = 14;
constexpr int kButtonHeight = 24;

constexpr const char* kTitles[kSetupStepCount] = {"Microfono", "Audio di sistema", "Driver fixomic Mic",
                                                  "Scegli fixomic Mic nella tua app"};

const char* const kWaitingForAnswer = "In attesa della tua risposta alla richiesta di macOS…";
const char* const kRequestFailed = "La richiesta non \u00e8 riuscita: riprova.";
const char* const kSystemAudioSettings =
    "Impostazioni di Sistema → Privacy e sicurezza → Registrazione schermo e audio di sistema";

int pushButtonWidth(const juce::TextButton& button) { return Theme::pushButtonWidth(button.getButtonText()); }
int flatButtonWidth(const juce::TextButton& button) { return Theme::flatButtonWidth(button); }

const char* statusName(StepStatus status) {
    switch (status) {
    case StepStatus::Done: return "fatto";
    case StepStatus::ToDo: return "da fare";
    case StepStatus::Waiting: return "in attesa";
    case StepStatus::Blocked: return "bloccato";
    }
    return "";
}

} // namespace

struct Onboarding::Layout {
    juce::Rectangle<int> back, title, intro;
    std::array<juce::Rectangle<int>, kSetupStepCount> titles{}, details{};
    std::array<juce::Point<float>, kSetupStepCount> markers{};
    juce::Rectangle<int> body, devices, error, primary, skip;
    int height = 0;
};

Onboarding::Onboarding() {
    setOpaque(false);
    setTitle("Configurazione iniziale");
    setWantsKeyboardFocus(false);

    back_.onClick = [this] {
        if (onClose) onClose();
    };
    title_.setText("Configurazione iniziale");
    intro_.setText(utf8("Quattro passi, poi la tua voce esce pulita da fixomic Mic."));
    for (int i = 0; i < kSetupStepCount; ++i) {
        stepTitles_[static_cast<size_t>(i)].setText(kTitles[i]);
        stepDetails_[static_cast<size_t>(i)].setStyle(TextStyle::Caption);
        stepDetails_[static_cast<size_t>(i)].setInk(TextBlock::Ink::Secondary);
    }
    devices_.onChoose = [this](int index) {
        if (index < 0 || index >= static_cast<int>(data_.devices.size())) return;
        if (onChooseDevice) onChooseDevice(data_.devices[static_cast<size_t>(index)]);
    };
    primary_.setMouseClickGrabsKeyboardFocus(false);
    primary_.onClick = [this] { primaryClicked(); };
    skip_.setButtonText("Salta");
    skip_.setTitle("Salta questo passo");
    skip_.getProperties().set(Theme::kStyleProperty, Theme::kFlatCaptionStyle);   // the size of the button beside it
    skip_.setMouseClickGrabsKeyboardFocus(false);
    skip_.onClick = [this] { skip(); };

    for (juce::Component* c : std::initializer_list<juce::Component*>{&back_, &title_, &intro_, &body_})
        addAndMakeVisible(c);
    for (auto& t : stepTitles_) addAndMakeVisible(t);
    for (auto& d : stepDetails_) addChildComponent(d);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&devices_, &error_, &primary_, &skip_})
        addChildComponent(c);

    update();
}

Onboarding::~Onboarding() = default;

// ---- data -------------------------------------------------------------------------------------

void Onboarding::setData(const Data& data) {
    if (data == data_) return;
    data_ = data;
    update();
}

const Onboarding::Data& Onboarding::data() const { return data_; }

void Onboarding::restart() {
    from_ = 0;
    if (!std::exchange(resultUnseen_, false)) {   // seen: gone; unseen: it shows now, then goes
        driverError_.clear();
        audioRequestFailed_ = false;
    }
    update();
}

void Onboarding::skip() {
    from_ = std::min(currentStep() + 1, kSetupStepCount - 1);
    update();
}

int Onboarding::currentStep() const { return currentSetupStep(steps_, from_); }

void Onboarding::driverOperationFinished(bool ok, const juce::String& error) {
    driverError_ = ok ? juce::String() : (error.isNotEmpty() ? error : juce::String("Installazione non riuscita."));
    if (!ok) resultUnseen_ = !isVisible();   // the panel was closed meanwhile: keep it for the next opening
    update();
}

void Onboarding::systemAudioRequestFinished(bool ok) {
    audioRequestFailed_ = !ok;
    if (!ok) resultUnseen_ = !isVisible();
    update();
}

void Onboarding::update() {
    steps_ = setupSteps(data_.facts);
    const int current = currentStep();
    const auto& open = steps_[static_cast<size_t>(current)];

    // Station names and what was set up.
    for (int i = 0; i < kSetupStepCount; ++i) {
        const auto& step = steps_[static_cast<size_t>(i)];
        auto& title = stepTitles_[static_cast<size_t>(i)];
        title.setStyle(i == current ? TextStyle::BodyStrong : TextStyle::Body);
        title.setInk(i == current || step.status == StepStatus::Done ? TextBlock::Ink::Primary
                                                                      : TextBlock::Ink::Secondary);
        title.setDescription(juce::String("Passo ") + juce::String(i + 1) + " di 4, "
                             + (i == current ? "aperto, " : "") + statusName(step.status));
        juce::String detail;
        if (i != current) {
            if (step.status == StepStatus::Done) {
                switch (step.step) {
                case SetupStep::Microphone:
                    detail = selectedDevice(data_.devices) ? selectedDevice(data_.devices)->text : juce::String("Scelto");
                    break;
                case SetupStep::SystemAudio:
                    detail = data_.facts.systemAudio == mac::Permission::Granted ? "Consentito" : "Richiesta completata";
                    break;
                case SetupStep::Driver: detail = driverStatusText(data_.driver); break;
                case SetupStep::ChooseInApp: break;
                }
            } else if (step.status == StepStatus::Blocked) {
                detail = step.step == SetupStep::SystemAudio && data_.facts.warning == mac::WarningKind::SystemAudioSilent
                             ? "Probabilmente negato"
                             : "Accesso negato";
            }
        }
        stepDetails_[static_cast<size_t>(i)].setText(detail);
        stepDetails_[static_cast<size_t>(i)].setVisible(detail.isNotEmpty());
    }

    // The open step: its explanation and its actions.
    juce::String body, action;
    bool listDevices = false;
    switch (open.step) {
    case SetupStep::Microphone:
        switch (open.action) {
        case StepAction::Ask:
            body = utf8("fixomic elabora la voce del microfono che scegli. macOS ti chiederà il permesso.");
            action = utf8("Consenti l’accesso al microfono");
            break;
        case StepAction::Choose:
            listDevices = !data_.devices.empty();
            body = listDevices ? "Scegli il microfono da usare." : "Nessun microfono collegato: collegane uno.";
            break;
        case StepAction::OpenSettings:
            body = utf8("Accesso al microfono negato: abilita fixomic in Impostazioni di Sistema → Privacy e "
                        "sicurezza → Microfono.");
            action = "Apri Impostazioni di Sistema";
            break;
        case StepAction::None:   // the prompt is up
        case StepAction::Install:
        case StepAction::Finish: body = utf8(kWaitingForAnswer); break;
        }
        break;
    case SetupStep::SystemAudio:
        switch (open.action) {
        case StepAction::Ask:
            body = utf8("Per togliere l’eco, fixomic ascolta l’audio che il Mac riproduce. macOS ti "
                        "chiederà il permesso «Registrazione schermo e audio di sistema».");
            action = utf8("Consenti l’audio di sistema");
            break;
        case StepAction::OpenSettings:
            body = data_.facts.warning == mac::WarningKind::SystemAudioSilent
                       ? utf8("fixomic riceve solo silenzio dall’audio di sistema: il permesso è "
                              "probabilmente negato e l’eco resta. Abilita fixomic in ")
                             + utf8(kSystemAudioSettings) + "."
                       : utf8("Permesso negato: senza, la cancellazione dell’eco non funziona. Abilita fixomic "
                              "in ")
                             + utf8(kSystemAudioSettings) + ".";
            action = "Apri Impostazioni di Sistema";
            break;
        case StepAction::None:   // the prompt is up
        case StepAction::Choose:
        case StepAction::Install:
        case StepAction::Finish: body = utf8(kWaitingForAnswer); break;
        }
        break;
    case SetupStep::Driver:
        if (open.status == StepStatus::Waiting) {
            body = utf8("Installazione in corso…");
        } else if (open.action == StepAction::Install) {
            body = utf8("Il driver crea fixomic Mic, il microfono che le altre app vedono. macOS ti chiederà "
                        "la password di amministratore.");
            action = "Installa driver";
        } else {
            body = "Questa copia di fixomic non contiene il driver.";
        }
        break;
    case SetupStep::ChooseInApp:
        body = utf8("Ora scegli fixomic Mic come microfono in Discord, o nell’app che usi: fixomic si attiva da "
                    "solo quando un’app lo usa.");
        action = "Fine";
        break;
    }
    body_.setText(body);

    std::vector<ChoiceList::Item> items;
    if (listDevices)
        for (const auto& device : data_.devices) items.push_back({device.text, device.selected, !device.connected});
    devices_.setItems(std::move(items));
    devices_.setVisible(listDevices);

    juce::String error;
    if (open.step == SetupStep::Driver && open.status != StepStatus::Waiting)
        error = driverError_;
    else if (open.step == SetupStep::SystemAudio && open.action == StepAction::Ask && audioRequestFailed_)
        error = utf8(kRequestFailed);   // the throwaway IO failed: the prompt may not have been shown
    error_.setText(error);
    error_.setVisible(error.isNotEmpty());

    primary_.setButtonText(action);
    primary_.setVisible(action.isNotEmpty());
    skip_.setVisible(open.step != SetupStep::ChooseInApp);

    fitHeight();
    resized();
    repaint();
}

void Onboarding::primaryClicked() {
    const auto& open = steps_[static_cast<size_t>(currentStep())];
    switch (open.action) {
    case StepAction::Ask:
        if (open.step == SetupStep::Microphone && onRequestMicrophone) onRequestMicrophone();
        if (open.step == SetupStep::SystemAudio) {
            audioRequestFailed_ = false;
            update();
            if (onRequestSystemAudio) onRequestSystemAudio();
        }
        break;
    case StepAction::OpenSettings:
        if (open.pane && onOpenPane) onOpenPane(*open.pane);
        break;
    case StepAction::Install:
        driverError_.clear();
        resultUnseen_ = false;
        update();
        if (onInstallDriver) onInstallDriver();
        break;
    case StepAction::Finish:
        if (onFinish) onFinish();
        break;
    case StepAction::Choose:
    case StepAction::None: break;
    }
}

// ---- state for tests ----------------------------------------------------------------------------

juce::String Onboarding::stepTitle(int step) const {
    return step >= 0 && step < kSetupStepCount ? stepTitles_[static_cast<size_t>(step)].text() : juce::String();
}

juce::String Onboarding::stepDetail(int step) const {
    if (step < 0 || step >= kSetupStepCount) return {};
    const auto& detail = stepDetails_[static_cast<size_t>(step)];
    return detail.isVisible() ? detail.text() : juce::String();
}

juce::String Onboarding::bodyText() const { return body_.text(); }
juce::String Onboarding::errorText() const { return error_.isVisible() ? error_.text() : juce::String(); }
juce::Button& Onboarding::primaryButton() { return primary_; }
juce::Button& Onboarding::skipButton() { return skip_; }
ChoiceList& Onboarding::deviceList() { return devices_; }

bool Onboarding::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (onClose) onClose();
        return true;
    }
    return false;
}

// ---- layout and paint -------------------------------------------------------------------------

Onboarding::Layout Onboarding::layOut(int width) const {
    Layout l;
    const int content = width - 2 * kInset;
    const int textWidth = width - kInset - kTextX;
    l.back = {kInset - 8, 1, GlyphButton::kSize, GlyphButton::kSize};
    l.title = {l.back.getRight() + 2, 3, content, kHeaderHeight - 3};
    int y = kHeaderHeight + 4;
    l.intro = {kInset, y, content, intro_.heightForWidth(content)};
    y += l.intro.getHeight() + 16;

    const int current = currentStep();
    for (int i = 0; i < kSetupStepCount; ++i) {
        const auto index = static_cast<size_t>(i);
        l.markers[index] = {static_cast<float>(kInset) + kMarkerSize / 2.0f, static_cast<float>(y) + kTitleHeight / 2.0f};
        l.titles[index] = {kTextX, y, textWidth, kTitleHeight};
        y += kTitleHeight;
        if (stepDetails_[index].isVisible()) {
            l.details[index] = {kTextX, y, textWidth, stepDetails_[index].heightForWidth(textWidth)};
            y += l.details[index].getHeight();
        }
        if (i == current) {
            y += 2;
            l.body = {kTextX, y, textWidth, body_.heightForWidth(textWidth)};
            y += l.body.getHeight();
            if (devices_.isVisible()) {
                // The list's check column lines up with the steps' text.
                y += 6;
                l.devices = {kTextX - kInset, y, width - (kTextX - kInset), devices_.heightForWidth(width)};
                y += l.devices.getHeight();
            }
            if (error_.isVisible()) {
                y += 6;
                l.error = {kTextX, y, textWidth, error_.heightForWidth(textWidth)};
                y += l.error.getHeight();
            }
            if (primary_.isVisible() || skip_.isVisible()) {
                y += 10;
                int x = kTextX;
                if (primary_.isVisible()) {
                    l.primary = {x, y, std::min(textWidth, pushButtonWidth(primary_)), kButtonHeight};
                    x = l.primary.getRight() + 4;
                }
                if (skip_.isVisible()) {
                    // A flat button: its text lines up with the steps' text when it comes first.
                    const int skipX = primary_.isVisible() ? x : kTextX - kInset;
                    l.skip = {skipX, y, flatButtonWidth(skip_), kButtonHeight};
                }
                y += kButtonHeight;
            }
        }
        if (i < kSetupStepCount - 1) y += kStepGap;
    }
    l.height = y + 4;
    return l;
}

int Onboarding::heightForWidth(int width) const { return layOut(width).height; }

void Onboarding::fitHeight() {
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));
}

void Onboarding::resized() {
    const auto l = layOut(getWidth());
    back_.setBounds(l.back);
    title_.setBounds(l.title);
    intro_.setBounds(l.intro);
    for (size_t i = 0; i < stepTitles_.size(); ++i) {
        stepTitles_[i].setBounds(l.titles[i].withTrimmedTop(1));
        stepDetails_[i].setBounds(l.details[i]);
    }
    body_.setBounds(l.body);
    devices_.setBounds(l.devices);
    error_.setBounds(l.error);
    primary_.setBounds(l.primary);
    skip_.setBounds(l.skip);
    markers_ = l.markers;
}

void Onboarding::paint(juce::Graphics& g) {
    const auto& p = Theme::palette(*this);
    // The route: a line from the first station to the last, the stations on it.
    g.setColour(p.track);
    g.fillRoundedRectangle(juce::Rectangle<float>(markers_.front().x - 1.0f, markers_.front().y, 2.0f,
                                                  markers_.back().y - markers_.front().y),
                           1.0f);
    const int current = currentStep();
    g.setFont(Theme::font(TextStyle::Micro));
    for (int i = 0; i < kSetupStepCount; ++i) {
        const auto centre = markers_[static_cast<size_t>(i)];
        const auto marker = juce::Rectangle<float>(kMarkerSize, kMarkerSize).withCentre(centre);
        const auto status = steps_[static_cast<size_t>(i)].status;
        g.setColour(p.background);   // a ring of ground keeps the station off the line
        g.fillEllipse(marker.expanded(2.0f));
        if (status == StepStatus::Done) {
            g.setColour(p.text);
            g.fillEllipse(marker);
            juce::Path check;
            check.startNewSubPath(centre.x - 4.0f, centre.y + 0.3f);
            check.lineTo(centre.x - 1.2f, centre.y + 3.1f);
            check.lineTo(centre.x + 4.2f, centre.y - 3.2f);
            g.setColour(p.background);
            g.strokePath(check, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        } else if (status == StepStatus::Blocked) {
            g.setColour(p.problem);
            g.fillEllipse(marker);
            g.setColour(p.background);
            g.fillRoundedRectangle(juce::Rectangle<float>(2.2f, 7.0f).withCentre({centre.x, centre.y - 1.6f}), 1.1f);
            g.fillEllipse(juce::Rectangle<float>(2.4f, 2.4f).withCentre({centre.x, centre.y + 4.2f}));
        } else {
            const bool open = i == current;
            g.setColour(open ? p.text : p.textTertiary);
            g.drawEllipse(marker.reduced(open ? 1.0f : 0.75f), open ? 2.0f : 1.5f);
            g.drawText(juce::String(i + 1), marker, juce::Justification::centred, false);
        }
    }
}

} // namespace fixomic::ui
