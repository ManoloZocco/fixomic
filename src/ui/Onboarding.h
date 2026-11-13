#pragma once

#include "ui/PanelParts.h"
#include "ui/SetupModel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace fixomic::ui {

// The first-run guide, in the panel's place (never a window, no modal alert). A header with back (‹)
// and "Configurazione iniziale", a line about it, then the four steps as a vertical route, one station
// each: microphone (the permission, then the device), system audio recording (for the AEC), the
// driver, "Ora scegli fixomic Mic in Discord" (or the app of choice).
//
// Stations: done = filled with a check, and what was set up under the name; blocked = red (denied:
// only System Settings fixes it); the open step = a ring, with its explanation and its actions (ask
// for the permission, choose the microphone, open System Settings, install the driver, "Salta",
// "Fine"); to do = hollow. The open step is currentSetupStep(setupSteps(facts), from): steps already
// satisfied are passed over, "Salta" moves past the open one. A system audio prompt that was answered
// counts as done; a denied or silent tap reported later reopens it as blocked, with the System Settings
// link and a note.
//
// Data in through setData(), intents out through the callbacks; the permission prompts and the driver
// install go through the AppController, which shares the prompts with the engine (one prompt each).
// Its bounds reach Theme::kFlatTextInset past the content edges; it sets its own height. Escape and
// back call onClose (closing early: the guide is not done). Message thread only.
class Onboarding : public juce::Component {
public:
    struct Data {
        SetupFacts facts;
        std::vector<DeviceChoice> devices;   // for the microphone step
        DriverFacts driver;
        bool operator==(const Data&) const = default;
    };

    Onboarding();
    ~Onboarding() override;

    void setData(const Data&);
    const Data& data() const;
    void restart();     // from the first step not done; no driver error
    void skip();        // "Salta": past the open step
    int currentStep() const;
    int heightForWidth(int width) const;

    void driverOperationFinished(bool ok, const juce::String& error);

    std::function<void()> onRequestMicrophone;
    std::function<void()> onRequestSystemAudio;
    std::function<void(const DeviceChoice&)> onChooseDevice;
    std::function<void(mac::PrivacyPane)> onOpenPane;
    std::function<void()> onInstallDriver;
    std::function<void()> onFinish;    // "Fine"
    std::function<void()> onClose;     // back, Escape: closed early

    // State, for tests and the snapshot.
    juce::String stepTitle(int step) const;
    juce::String stepDetail(int step) const;   // under a done or blocked step's name
    juce::String bodyText() const;             // the open step's explanation
    juce::String errorText() const;
    juce::Button& primaryButton();             // the open step's action (hidden when it has none)
    juce::Button& skipButton();
    ChoiceList& deviceList();

    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    struct Layout;
    Layout layOut(int width) const;
    void update();
    void fitHeight();
    void primaryClicked();

    Data data_;
    std::array<StepState, kSetupStepCount> steps_{};
    int from_ = 0;
    juce::String driverError_;
    std::array<juce::Point<float>, kSetupStepCount> markers_{};   // station centres, from the last layout

    GlyphButton back_{GlyphButton::Glyph::Back, "Chiudi la configurazione iniziale"};
    TextBlock title_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    TextBlock intro_{TextStyle::Caption, TextBlock::Ink::Secondary};
    std::array<TextBlock, kSetupStepCount> stepTitles_;
    std::array<TextBlock, kSetupStepCount> stepDetails_;
    TextBlock body_{TextStyle::Caption, TextBlock::Ink::Secondary};
    ChoiceList devices_{"Microfono"};
    TextBlock error_{TextStyle::Caption, TextBlock::Ink::Problem};
    juce::TextButton primary_;
    juce::TextButton skip_;
};

} // namespace fixomic::ui
