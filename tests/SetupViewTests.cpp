#include <catch2/catch_test_macros.hpp>

#include "ui/Onboarding.h"
#include "ui/SettingsPanel.h"

#include <CoreFoundation/CoreFoundation.h>

#include <functional>
#include <optional>
#include <vector>

using fixomic::mac::AudioDeviceInfo;
using fixomic::mac::Permission;
using fixomic::mac::PrivacyPane;
using fixomic::mac::WarningKind;
using namespace fixomic::ui;

// The settings view and the first-run guide, driven like the panel drives them: data in, intents out.
// No window: clicks go through the buttons' onClick, choices from the lists arrive asynchronously
// (the message loop is pumped).

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int kViewWidth = 344;   // MainPanel's content width plus the flat buttons' inset each side

void pumpMessages(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
}

bool pumpUntil(const std::function<bool()>& done, int ms = 2000) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (!done() && juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    return done();
}

template <typename T>
T* findChild(juce::Component& root, const std::function<bool(T&)>& matches) {
    for (auto* child : root.getChildren()) {
        if (auto* typed = dynamic_cast<T*>(child); typed != nullptr && matches(*typed)) return typed;
        if (auto* found = findChild<T>(*child, matches)) return found;
    }
    return nullptr;
}

// The visible button with this text (or accessible title), else nullptr.
juce::Button* button(juce::Component& root, const juce::String& text) {
    return findChild<juce::Button>(root, [&](juce::Button& b) {
        return b.isVisible() && (b.getButtonText() == text || b.getTitle() == text);
    });
}

const std::vector<AudioDeviceInfo> kDevices{{0, "builtin", "Microfono MacBook Air", 1, {44100.0, 48000.0}},
                                            {0, "scarlett", "Scarlett 2i2 USB", 2, {48000.0}},
                                            {0, "old", "USB Mic 44k", 1, {44100.0}}};

SettingsPanel::Data settingsData(const juce::String& uid, const juce::String& name) {
    SettingsPanel::Data d;
    d.devices = deviceChoices(kDevices, uid, name);
    d.bufferSize = 128;
    d.driver = {true, "0.1.0", "0.1.0", false, false};
    d.microphone = Permission::Granted;
    d.systemAudio = Permission::Granted;
    return d;
}

struct SettingsRig {
    juce::ScopedJuceInitialiser_GUI gui;
    SettingsPanel view;
    std::vector<DeviceChoice> devices;
    std::vector<int> channels, buffers;
    std::vector<bool> loginRequests;
    juce::String loginAnswer;
    int installs = 0, uninstalls = 0, reviews = 0, closes = 0;
    std::vector<PrivacyPane> panes;

    SettingsRig() {
        view.setSize(kViewWidth, 10);
        view.onChooseDevice = [this](const DeviceChoice& d) { devices.push_back(d); };
        view.onChooseChannel = [this](int c) { channels.push_back(c); };
        view.onChooseBuffer = [this](int b) { buffers.push_back(b); };
        view.onSetLaunchAtLogin = [this](bool on) {
            loginRequests.push_back(on);
            return loginAnswer;
        };
        view.onInstallDriver = [this] { ++installs; };
        view.onUninstallDriver = [this] { ++uninstalls; };
        view.onOpenPane = [this](PrivacyPane pane) { panes.push_back(pane); };
        view.onReviewSetup = [this] { ++reviews; };
        view.onClose = [this] { ++closes; };
    }
};

} // namespace

// ---- SettingsPanel ----------------------------------------------------------------------------

TEST_CASE("SettingsPanel lists the microphones and never moves the choice by itself", "[ui][setupview]") {
    SettingsRig s;
    SECTION("the configured microphone is not connected: shown first, checked, as non collegato") {
        s.view.setData(settingsData("hyperx", "HyperX Cloud Alpha"));
        auto* found = findChild<ChoiceList>(s.view, [](auto&) { return true; });
        REQUIRE(found != nullptr);
        auto& list = *found;
        REQUIRE(list.items().size() == 4);
        REQUIRE(list.items()[0].text == utf8("HyperX Cloud Alpha — non collegato"));
        REQUIRE(list.items()[0].selected);
        REQUIRE(list.items()[0].dimmed);
        REQUIRE(list.row(0)->getToggleState());
        for (int i = 1; i < 4; ++i) REQUIRE_FALSE(list.row(i)->getToggleState());
        REQUIRE(button(s.view, "Canale 1") == nullptr);   // no channels for a device that is not there
        REQUIRE(s.view.sampleRateText().isEmpty());       // its rates are unknown

        // Choosing another one: reported once the click handler returned, with its name.
        list.row(2)->onClick();
        REQUIRE(s.devices.empty());
        REQUIRE(pumpUntil([&] { return s.devices.size() == 1; }));
        REQUIRE(s.devices[0].uid == "scarlett");
        REQUIRE(s.devices[0].name == "Scarlett 2i2 USB");
        // The chosen one again: nothing to do.
        list.row(0)->onClick();
        pumpMessages(50);
        REQUIRE(s.devices.size() == 1);
    }
    SECTION("a device with two input channels offers the channel") {
        auto data = settingsData("scarlett", "Scarlett 2i2 USB");
        data.channel = 0;
        s.view.setData(data);
        REQUIRE(button(s.view, "Canale 1") != nullptr);
        REQUIRE(button(s.view, "Canale 1")->getToggleState());
        REQUIRE_FALSE(button(s.view, "Canale 2")->getToggleState());
        button(s.view, "Canale 2")->onClick();
        REQUIRE(pumpUntil([&] { return !s.channels.empty(); }));
        REQUIRE(s.channels == std::vector<int>{1});
        button(s.view, "Canale 1")->onClick();   // already chosen
        pumpMessages(50);
        REQUIRE(s.channels == std::vector<int>{1});

        s.view.setData(settingsData("builtin", "Microfono MacBook Air"));   // one channel: nothing to choose
        REQUIRE(button(s.view, "Canale 1") == nullptr);
    }
    SECTION("a microphone without 48 kHz: the UnsupportedSampleRate text under 48 kHz") {
        s.view.setData(settingsData("old", "USB Mic 44k"));
        REQUIRE(s.view.sampleRateText() == "Questo microfono non supporta 48 kHz");
        s.view.setData(settingsData("builtin", "Microfono MacBook Air"));
        REQUIRE(s.view.sampleRateText().isEmpty());
    }
}

TEST_CASE("SettingsPanel buffer sizes with their duration", "[ui][setupview]") {
    SettingsRig s;
    s.view.setData(settingsData("builtin", "x"));
    auto* b64 = button(s.view, utf8("64 · 1,3 ms"));
    auto* b128 = button(s.view, utf8("128 · 2,7 ms"));
    auto* b256 = button(s.view, utf8("256 · 5,3 ms"));
    REQUIRE(b64 != nullptr);
    REQUIRE(b128 != nullptr);
    REQUIRE(b256 != nullptr);
    REQUIRE(b128->getToggleState());
    REQUIRE_FALSE(b64->getToggleState());
    b128->onClick();   // the current one: nothing
    b256->onClick();
    REQUIRE(pumpUntil([&] { return !s.buffers.empty(); }));
    pumpMessages(50);
    REQUIRE(s.buffers == std::vector<int>{256});
}

TEST_CASE("SettingsPanel launch at login shows why it failed and the system state", "[ui][setupview]") {
    SettingsRig s;
    s.view.setData(settingsData("builtin", "x"));
    auto* toggle = findChild<juce::ToggleButton>(s.view, [](auto& b) { return b.getButtonText().startsWith("Avvio"); });
    REQUIRE(toggle != nullptr);
    REQUIRE_FALSE(toggle->getToggleState());

    s.loginAnswer = utf8("Impossibile attivare l’avvio all’accesso.");
    toggle->setToggleState(true, juce::dontSendNotification);   // what a click does first
    toggle->onClick();
    REQUIRE(s.loginRequests == std::vector<bool>{true});
    REQUIRE(s.view.loginError() == s.loginAnswer);
    REQUIRE_FALSE(toggle->getToggleState());   // back to the system state

    s.loginAnswer = {};
    toggle->setToggleState(true, juce::dontSendNotification);
    toggle->onClick();
    REQUIRE(s.view.loginError().isEmpty());
    auto data = s.view.data();
    data.launchAtLogin = true;   // the app re-reads the system state
    s.view.setData(data);
    REQUIRE(toggle->getToggleState());
}

TEST_CASE("SettingsPanel driver: install, update, uninstall confirmed inline, progress and result", "[ui][setupview]") {
    SettingsRig s;
    auto data = settingsData("builtin", "x");

    SECTION("not installed: install") {
        data.driver = {false, {}, "0.1.0", false, false};
        s.view.setData(data);
        REQUIRE(button(s.view, "Installa driver") != nullptr);
        REQUIRE(button(s.view, "Disinstalla driver") == nullptr);
        button(s.view, "Installa driver")->onClick();
        REQUIRE(s.installs == 1);
        REQUIRE(s.view.driverOperationStarted());
        REQUIRE(s.view.driverMessage() == utf8("Operazione in corso…"));
        REQUIRE_FALSE(button(s.view, "Installa driver")->isEnabled());
        data.driver.operationInProgress = true;   // what the app reports meanwhile
        s.view.setData(data);
        button(s.view, "Installa driver")->onClick();   // disabled in the real UI; ignored anyway
        REQUIRE(s.installs == 1);

        s.view.driverOperationFinished(true, {});
        data.driver = {true, "0.1.0", "0.1.0", false, false};
        s.view.setData(data);
        REQUIRE_FALSE(s.view.driverOperationStarted());
        REQUIRE(s.view.driverMessage() == "Driver installato.");
        REQUIRE(button(s.view, "Installa driver") == nullptr);
        REQUIRE(button(s.view, "Disinstalla driver")->isEnabled());
    }
    SECTION("an update: its versions, and the failure shown inline") {
        data.driver = {true, "0.1.0", "0.2.0", true, false};
        s.view.setData(data);
        auto* update = button(s.view, utf8("Aggiorna driver (v0.1.0 → v0.2.0)"));
        REQUIRE(update != nullptr);
        REQUIRE(button(s.view, "Disinstalla driver") != nullptr);
        update->onClick();
        REQUIRE(s.installs == 1);
        s.view.driverOperationFinished(false, "Installazione annullata");
        REQUIRE(s.view.driverMessage() == "Installazione annullata");
        s.view.reset();   // reopened: the old result is gone
        REQUIRE(s.view.driverMessage().isEmpty());
    }
    SECTION("uninstall asks first, inside the view") {
        s.view.setData(data);
        button(s.view, "Disinstalla driver")->onClick();
        REQUIRE(s.uninstalls == 0);
        REQUIRE(s.view.confirmingUninstall());
        REQUIRE(button(s.view, "Disinstalla driver") == nullptr);
        button(s.view, "Annulla")->onClick();
        REQUIRE_FALSE(s.view.confirmingUninstall());
        REQUIRE(s.uninstalls == 0);

        button(s.view, "Disinstalla driver")->onClick();
        button(s.view, "Disinstalla")->onClick();
        REQUIRE(s.uninstalls == 1);
        REQUIRE_FALSE(s.view.confirmingUninstall());
        REQUIRE(s.view.driverMessage() == utf8("Operazione in corso…"));
        s.view.driverOperationFinished(true, {});
        data.driver = {false, {}, "0.1.0", false, false};
        s.view.setData(data);
        REQUIRE(s.view.driverMessage() == "Driver disinstallato.");
        REQUIRE(button(s.view, "Installa driver") != nullptr);
    }
    SECTION("an operation started elsewhere disables the actions") {
        data.driver.operationInProgress = true;
        s.view.setData(data);
        REQUIRE_FALSE(button(s.view, "Disinstalla driver")->isEnabled());
        REQUIRE(s.view.driverMessage() == utf8("Operazione in corso…"));
        REQUIRE_FALSE(s.view.driverOperationStarted());
    }
    SECTION("nothing bundled, nothing installed: said so, no action") {
        data.driver = {false, {}, {}, false, false};
        s.view.setData(data);
        REQUIRE(button(s.view, "Installa driver") == nullptr);
        REQUIRE(s.view.driverMessage() == "Questa copia di fixomic non contiene il driver.");
    }
}

TEST_CASE("SettingsPanel permissions: System Settings for each one not granted", "[ui][setupview]") {
    SettingsRig s;
    auto data = settingsData("builtin", "x");
    s.view.setData(data);
    REQUIRE(button(s.view, "Apri Impostazioni di Sistema: microfono") == nullptr);
    REQUIRE(button(s.view, "Apri Impostazioni di Sistema: audio di sistema") == nullptr);

    data.microphone = Permission::Denied;
    data.systemAudio = Permission::NotDetermined;   // unknown, not denied
    s.view.setData(data);
    REQUIRE(findChild<TextBlock>(s.view, [](auto& t) { return t.text() == "Negato" && t.isVisible(); }) != nullptr);
    REQUIRE(findChild<TextBlock>(s.view, [](auto& t) { return t.text() == "Non noto" && t.isVisible(); }) != nullptr);
    button(s.view, "Apri Impostazioni di Sistema: microfono")->onClick();
    button(s.view, "Apri Impostazioni di Sistema: audio di sistema")->onClick();
    REQUIRE(s.panes == std::vector<PrivacyPane>{PrivacyPane::Microphone, PrivacyPane::SystemAudio});

    button(s.view, "Rivedi la configurazione iniziale")->onClick();
    REQUIRE(s.reviews == 1);
    REQUIRE(s.view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    REQUIRE(s.closes == 1);
}

// ---- NoticeRow --------------------------------------------------------------------------------

TEST_CASE("a warning row offers Apri Impostazioni only when a pane can fix it", "[ui][setupview]") {
    juce::ScopedJuceInitialiser_GUI gui;
    for (const auto kind : {WarningKind::SystemAudioDenied, WarningKind::SystemAudioSilent,
                            WarningKind::SystemAudioUnavailable}) {
        const auto notices = collectNotices("warning", warningPane(kind), {});
        REQUIRE(notices.size() == 1);
        std::optional<PrivacyPane> opened;
        NoticeRow row(notices[0], [&](PrivacyPane pane) { opened = pane; });
        if (kind == WarningKind::SystemAudioUnavailable) {
            REQUIRE(row.button() == nullptr);
        } else {
            REQUIRE(row.button() != nullptr);
            REQUIRE(row.button()->getButtonText() == "Apri Impostazioni");
            row.button()->onClick();
            REQUIRE(opened == PrivacyPane::SystemAudio);
        }
    }
}

// ---- Onboarding -------------------------------------------------------------------------------

namespace {

struct GuideRig {
    juce::ScopedJuceInitialiser_GUI gui;
    Onboarding guide;
    int micRequests = 0, audioRequests = 0, installs = 0, finishes = 0, closes = 0;
    std::vector<DeviceChoice> chosen;
    std::vector<PrivacyPane> panes;

    GuideRig() {
        guide.setSize(kViewWidth, 10);
        guide.onRequestMicrophone = [this] { ++micRequests; };
        guide.onRequestSystemAudio = [this] { ++audioRequests; };
        guide.onInstallDriver = [this] { ++installs; };
        guide.onFinish = [this] { ++finishes; };
        guide.onClose = [this] { ++closes; };
        guide.onChooseDevice = [this](const DeviceChoice& d) { chosen.push_back(d); };
        guide.onOpenPane = [this](PrivacyPane pane) { panes.push_back(pane); };
    }

    void set(const SetupFacts& facts, const juce::String& uid = {}) {
        Onboarding::Data data;
        data.facts = facts;
        data.devices = deviceChoices(kDevices, uid, {});
        data.driver = {facts.driverInstalled, facts.driverInstalled ? "0.1.0" : "", "0.1.0", false,
                       facts.driverOperationInProgress};
        guide.setData(data);
    }
    juce::String primary() { return guide.primaryButton().isVisible() ? guide.primaryButton().getButtonText() : ""; }
};

} // namespace

TEST_CASE("Onboarding walks the four steps in order, passing over the ones already done", "[ui][setupview]") {
    GuideRig g;
    SetupFacts f;   // nothing done
    g.set(f);
    REQUIRE(g.guide.stepTitle(0) == "Microfono");
    REQUIRE(g.guide.stepTitle(1) == "Audio di sistema");
    REQUIRE(g.guide.stepTitle(2) == "Driver fixomic Mic");
    REQUIRE(g.guide.stepTitle(3) == "Scegli fixomic Mic nella tua app");
    REQUIRE(g.guide.currentStep() == 0);
    REQUIRE(g.primary() == utf8("Consenti l’accesso al microfono"));
    g.guide.primaryButton().onClick();
    REQUIRE(g.micRequests == 1);

    f.microphoneRequestPending = true;   // the prompt is up
    g.set(f);
    REQUIRE(g.primary().isEmpty());
    REQUIRE(g.guide.bodyText().startsWith("In attesa"));

    f.microphoneRequestPending = false;
    f.microphone = Permission::Granted;   // granted: choose the device
    g.set(f);
    REQUIRE(g.guide.currentStep() == 0);
    REQUIRE(g.guide.deviceList().isVisible());
    REQUIRE(g.guide.deviceList().items().size() == 3);
    g.guide.deviceList().row(1)->onClick();
    REQUIRE(pumpUntil([&] { return !g.chosen.empty(); }));
    REQUIRE(g.chosen.size() == 1);
    REQUIRE(g.chosen[0].uid == "scarlett");

    f.microphoneChosen = true;   // mic done: on to system audio
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 1);
    REQUIRE(g.guide.stepDetail(0) == "Scarlett 2i2 USB");
    REQUIRE_FALSE(g.guide.deviceList().isVisible());
    REQUIRE(g.primary() == utf8("Consenti l’audio di sistema"));
    g.guide.primaryButton().onClick();
    REQUIRE(g.audioRequests == 1);

    f.systemAudioAnswered = true;   // ok: answered, the outcome is unknown
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 2);
    REQUIRE(g.guide.stepDetail(1) == "Richiesta completata");
    REQUIRE(g.primary() == "Installa driver");
    g.guide.primaryButton().onClick();
    REQUIRE(g.installs == 1);
    g.guide.driverOperationFinished(false, "Installazione annullata");
    REQUIRE(g.guide.errorText() == "Installazione annullata");

    f.driverInstalled = true;
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 3);
    REQUIRE(g.guide.stepDetail(2) == utf8("Installato · v0.1.0"));
    REQUIRE(g.guide.bodyText().contains("Discord"));
    REQUIRE(g.primary() == "Fine");
    REQUIRE_FALSE(g.guide.skipButton().isVisible());
    g.guide.primaryButton().onClick();
    REQUIRE(g.finishes == 1);
}

TEST_CASE("Onboarding: Salta moves past a step, restart goes back to the first one not done", "[ui][setupview]") {
    GuideRig g;
    SetupFacts f;
    f.microphone = Permission::Denied;   // blocked: System Settings
    g.set(f);
    REQUIRE(g.guide.currentStep() == 0);
    REQUIRE(g.primary() == "Apri Impostazioni di Sistema");
    g.guide.primaryButton().onClick();
    REQUIRE(g.panes == std::vector<PrivacyPane>{PrivacyPane::Microphone});

    g.guide.skipButton().onClick();
    REQUIRE(g.guide.currentStep() == 1);
    REQUIRE(g.guide.stepDetail(0) == "Accesso negato");
    g.guide.skip();
    REQUIRE(g.guide.currentStep() == 2);
    g.guide.skip();
    REQUIRE(g.guide.currentStep() == 3);
    g.guide.skip();   // nowhere further
    REQUIRE(g.guide.currentStep() == 3);
    REQUIRE(g.finishes == 0);

    g.guide.restart();
    REQUIRE(g.guide.currentStep() == 0);
    REQUIRE(g.guide.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    REQUIRE(g.closes == 1);
    REQUIRE(g.finishes == 0);   // closed early: not done
}

TEST_CASE("Onboarding: a denied or silent tap reported later reopens the system audio step", "[ui][setupview]") {
    GuideRig g;
    SetupFacts f;
    f.microphone = Permission::Granted;
    f.microphoneChosen = true;
    f.systemAudioAnswered = true;
    f.driverInstalled = true;
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 3);

    f.warning = WarningKind::SystemAudioSilent;
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 1);
    REQUIRE(g.guide.bodyText().contains("probabilmente negato"));
    REQUIRE(g.primary() == "Apri Impostazioni di Sistema");
    g.guide.primaryButton().onClick();
    REQUIRE(g.panes == std::vector<PrivacyPane>{PrivacyPane::SystemAudio});

    f.warning = WarningKind::SystemAudioUnavailable;   // no pane fixes it: the step stays done
    g.set(f, "scarlett");
    REQUIRE(g.guide.currentStep() == 3);
}
