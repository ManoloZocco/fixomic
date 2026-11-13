#include <catch2/catch_test_macros.hpp>

#include "core/Config.h"
#include "ui/SetupModel.h"
#include "ui/StatusModel.h"

using fixomic::Config;
using fixomic::EngineState;
using fixomic::mac::AudioDeviceInfo;
using fixomic::mac::Permission;
using fixomic::mac::PrivacyPane;
using fixomic::mac::WarningKind;
using namespace fixomic::ui;

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

AudioDeviceInfo device(const char* uid, const char* name, int channels, std::vector<double> rates = {44100.0, 48000.0}) {
    return {0, uid, name, channels, std::move(rates)};
}

const std::vector<AudioDeviceInfo> kDevices{device("BuiltInMicrophoneDevice", "Microfono MacBook Air", 1),
                                            device("Scarlett", "Scarlett 2i2 USB", 2),
                                            device("OldUSB", "USB Mic 44k", 1, {44100.0})};

SetupFacts allDone() {
    SetupFacts f;
    f.microphone = Permission::Granted;
    f.microphoneChosen = true;
    f.systemAudio = Permission::Granted;
    f.driverInstalled = true;
    return f;
}

} // namespace

// ---- warnings ---------------------------------------------------------------------------------

TEST_CASE("a warning offers System Settings only where opening it can fix the problem", "[ui][setup]") {
    REQUIRE(warningPane(WarningKind::SystemAudioDenied) == PrivacyPane::SystemAudio);
    REQUIRE(warningPane(WarningKind::SystemAudioSilent) == PrivacyPane::SystemAudio);
    // The tap or the throwaway IO failed: the prompt may not even have been shown.
    REQUIRE_FALSE(warningPane(WarningKind::SystemAudioUnavailable).has_value());
}

// ---- microphone -------------------------------------------------------------------------------

TEST_CASE("deviceChoices marks the configured microphone", "[ui][setup]") {
    const auto choices = deviceChoices(kDevices, "Scarlett", "Scarlett 2i2 USB");
    REQUIRE(choices.size() == 3);
    REQUIRE(choices[0].text == "Microfono MacBook Air");
    REQUIRE_FALSE(choices[0].selected);
    REQUIRE(choices[1].uid == "Scarlett");
    REQUIRE(choices[1].text == "Scarlett 2i2 USB");
    REQUIRE(choices[1].selected);
    REQUIRE(choices[1].connected);
    REQUIRE(choices[1].inputChannels == 2);
    REQUIRE(choices[1].supports48k);
    REQUIRE_FALSE(choices[2].supports48k);
    REQUIRE(selectedDevice(choices)->uid == "Scarlett");

    // The live name wins over the stored one (a renamed device).
    REQUIRE(deviceChoices(kDevices, "Scarlett", "Old name")[1].text == "Scarlett 2i2 USB");
}

TEST_CASE("a configured microphone that is not connected stays selected, first, as non collegato", "[ui][setup]") {
    const auto choices = deviceChoices(kDevices, "HyperX", "HyperX Cloud Alpha");
    REQUIRE(choices.size() == 4);
    REQUIRE(choices[0].uid == "HyperX");
    REQUIRE(choices[0].text == utf8("HyperX Cloud Alpha — non collegato"));
    REQUIRE(choices[0].selected);
    REQUIRE_FALSE(choices[0].connected);
    REQUIRE(choices[0].inputChannels == 0);
    REQUIRE(choices[0].supports48k);   // unknown: no warning about it
    for (size_t i = 1; i < choices.size(); ++i) REQUIRE_FALSE(choices[i].selected);   // never another one

    // A config written before names were stored: the uid stands in.
    REQUIRE(deviceChoices(kDevices, "HyperX", {})[0].text == utf8("HyperX — non collegato"));
    // No devices at all.
    const auto alone = deviceChoices({}, "HyperX", "HyperX Cloud Alpha");
    REQUIRE(alone.size() == 1);
    REQUIRE(alone[0].selected);
}

TEST_CASE("a microphone chosen before names were stored learns its name once connected", "[ui][setup]") {
    REQUIRE(inputNameToRemember(kDevices, "Scarlett", {}) == juce::String("Scarlett 2i2 USB"));
    REQUIRE_FALSE(inputNameToRemember(kDevices, "Scarlett", "Scarlett 2i2 USB").has_value());   // known
    REQUIRE_FALSE(inputNameToRemember(kDevices, "HyperX", {}).has_value());                     // not connected
    REQUIRE_FALSE(inputNameToRemember(kDevices, {}, {}).has_value());                           // none chosen
}

TEST_CASE("no microphone chosen: nothing selected", "[ui][setup]") {
    const auto choices = deviceChoices(kDevices, {}, {});
    REQUIRE(choices.size() == 3);
    for (const auto& c : choices) REQUIRE_FALSE(c.selected);
    REQUIRE_FALSE(selectedDevice(choices).has_value());
    REQUIRE(deviceChoices({}, {}, {}).empty());
}

TEST_CASE("channelChoices offers a channel only when there is more than one", "[ui][setup]") {
    REQUIRE(channelChoices(0).isEmpty());
    REQUIRE(channelChoices(1).isEmpty());
    REQUIRE(channelChoices(2) == juce::StringArray{"Canale 1", "Canale 2"});
    REQUIRE(channelChoices(8).size() == 8);
    REQUIRE(channelChoices(8)[7] == "Canale 8");
}

TEST_CASE("sampleRateWarning uses the UnsupportedSampleRate status text", "[ui][setup]") {
    const auto unsupported = presentStatus({EngineState::UnsupportedSampleRate}).text;
    REQUIRE(unsupported == "Questo microfono non supporta 48 kHz");
    REQUIRE(sampleRateWarning(deviceChoices(kDevices, "OldUSB", "USB Mic 44k")) == unsupported);
    REQUIRE(sampleRateWarning(deviceChoices(kDevices, "Scarlett", "Scarlett 2i2 USB")).isEmpty());
    REQUIRE(sampleRateWarning(deviceChoices(kDevices, "HyperX", "HyperX")).isEmpty());   // not connected
    REQUIRE(sampleRateWarning(deviceChoices(kDevices, {}, {})).isEmpty());
}

// ---- buffer -----------------------------------------------------------------------------------

TEST_CASE("buffer sizes are shown with their duration at 48 kHz", "[ui][setup]") {
    REQUIRE(kBufferSizes == std::array<int, 3>{64, 128, 256});
    REQUIRE(formatBufferMs(64) == "1,3 ms");
    REQUIRE(formatBufferMs(128) == "2,7 ms");
    REQUIRE(formatBufferMs(256) == "5,3 ms");
    REQUIRE(formatBufferChoice(128) == utf8("128 · 2,7 ms"));
}

// ---- driver -----------------------------------------------------------------------------------

TEST_CASE("driver status and actions", "[ui][setup]") {
    DriverFacts none{false, {}, "0.1.0", false, false};
    REQUIRE(driverStatusText(none) == "Non installato");
    REQUIRE(driverInstallActionText(none) == "Installa driver");
    REQUIRE_FALSE(canUninstallDriver(none));

    DriverFacts current{true, "0.1.0", "0.1.0", false, false};
    REQUIRE(driverStatusText(current) == utf8("Installato · v0.1.0"));
    REQUIRE(driverInstallActionText(current).isEmpty());
    REQUIRE(canUninstallDriver(current));
    REQUIRE(juce::String(kUninstallDriverText) == "Disinstalla driver");

    DriverFacts old{true, "0.1.0", "0.2.0", true, false};
    REQUIRE(driverInstallActionText(old) == utf8("Aggiorna driver (v0.1.0 → v0.2.0)"));
    REQUIRE(canUninstallDriver(old));

    DriverFacts unknownVersion{true, {}, {}, false, false};
    REQUIRE(driverStatusText(unknownVersion) == "Installato");
    REQUIRE(driverInstallActionText(unknownVersion).isEmpty());

    // Nothing bundled: nothing to install.
    DriverFacts notBundled{false, {}, {}, false, false};
    REQUIRE(driverInstallActionText(notBundled).isEmpty());

    // Files in place but fixomic Mic not published: reinstall or remove.
    DriverFacts stale{false, "0.1.0", "0.1.0", false, false};
    REQUIRE(driverStatusText(stale) == "Non installato");
    REQUIRE(driverInstallActionText(stale) == "Installa driver");
    REQUIRE(canUninstallDriver(stale));
}

// ---- permissions ------------------------------------------------------------------------------

TEST_CASE("permission texts: system audio NotDetermined is unknown, never denied", "[ui][setup]") {
    REQUIRE(microphonePermissionText(Permission::Granted) == "Consentito");
    REQUIRE(microphonePermissionText(Permission::Denied) == "Negato");
    REQUIRE(microphonePermissionText(Permission::NotDetermined) == "Non ancora chiesto");

    REQUIRE(systemAudioPermissionText(Permission::Granted, std::nullopt) == "Consentito");
    REQUIRE(systemAudioPermissionText(Permission::Denied, std::nullopt) == "Negato");
    REQUIRE(systemAudioPermissionText(Permission::NotDetermined, std::nullopt) == "Non noto");
    REQUIRE(systemAudioPermissionText(Permission::NotDetermined, WarningKind::SystemAudioSilent)
            == "Probabilmente negato");
    REQUIRE(systemAudioPermissionText(Permission::NotDetermined, WarningKind::SystemAudioUnavailable) == "Non noto");
}

// ---- first-run guide --------------------------------------------------------------------------

TEST_CASE("launchGuide opens the guide by itself only on a first launch", "[ui][setup]") {
    // No config file: Config{}, the flag not stored.
    REQUIRE(launchGuide(false, Config{}.onboardingDone) == LaunchGuide::Open);
    const auto atLaunch = [](const char* xml) {
        const auto parsed = fixomic::parseConfig(xml);
        REQUIRE(parsed.has_value());
        return launchGuide(parsed->onboardingFlagStored, parsed->config.onboardingDone);
    };
    // Written before the flag: open only if the old-config rule does not count it as set up.
    REQUIRE(atLaunch("<FixomicConfig version=\"1\" inputDeviceUid=\"\"/>") == LaunchGuide::Open);
    REQUIRE(atLaunch("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\"/>") == LaunchGuide::None);
    REQUIRE(atLaunch("<FixomicConfig version=\"1\"><Plugin description=\"x\"/></FixomicConfig>") == LaunchGuide::None);
    // Stated false: the guide was closed early; only the panel's reminder row, nothing opens.
    REQUIRE(atLaunch("<FixomicConfig version=\"1\" onboardingDone=\"0\"/>") == LaunchGuide::Reminder);
    REQUIRE(atLaunch("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\" onboardingDone=\"0\"/>")
            == LaunchGuide::Reminder);
    // Stated true: nothing.
    REQUIRE(atLaunch("<FixomicConfig version=\"1\" onboardingDone=\"1\"/>") == LaunchGuide::None);
}

TEST_CASE("the settings ask for a permission macOS has not asked yet, and link only where it helps", "[ui][setup]") {
    // Not asked yet: fixomic is not listed in Privacy -> Microfono, so the link would lead nowhere.
    REQUIRE(microphoneSettingsAction(Permission::NotDetermined) == StepAction::Ask);
    REQUIRE(microphoneSettingsAction(Permission::Denied) == StepAction::OpenSettings);
    REQUIRE(microphoneSettingsAction(Permission::Granted) == StepAction::None);
    // System audio: NotDetermined is "unknown" (maybe denied): the link stays.
    REQUIRE(systemAudioSettingsAction(Permission::NotDetermined) == StepAction::OpenSettings);
    REQUIRE(systemAudioSettingsAction(Permission::Denied) == StepAction::OpenSettings);
    REQUIRE(systemAudioSettingsAction(Permission::Granted) == StepAction::None);
}

TEST_CASE("setupSteps: everything already satisfied", "[ui][setup]") {
    const auto steps = setupSteps(allDone());
    REQUIRE(steps[0] == StepState{SetupStep::Microphone, StepStatus::Done, StepAction::None, std::nullopt});
    REQUIRE(steps[1] == StepState{SetupStep::SystemAudio, StepStatus::Done, StepAction::None, std::nullopt});
    REQUIRE(steps[2] == StepState{SetupStep::Driver, StepStatus::Done, StepAction::None, std::nullopt});
    REQUIRE(steps[3] == StepState{SetupStep::ChooseInApp, StepStatus::ToDo, StepAction::Finish, std::nullopt});
    REQUIRE(currentSetupStep(steps, 0) == 3);   // satisfied steps are passed over
}

TEST_CASE("setupSteps: the microphone step", "[ui][setup]") {
    auto f = allDone();
    f.microphone = Permission::NotDetermined;
    f.microphoneChosen = false;
    REQUIRE(setupSteps(f)[0] == StepState{SetupStep::Microphone, StepStatus::ToDo, StepAction::Ask, std::nullopt});
    f.microphoneRequestPending = true;
    REQUIRE(setupSteps(f)[0].status == StepStatus::Waiting);
    REQUIRE(setupSteps(f)[0].action == StepAction::None);

    f.microphoneRequestPending = false;
    f.microphone = Permission::Granted;   // granted, no device chosen yet
    REQUIRE(setupSteps(f)[0] == StepState{SetupStep::Microphone, StepStatus::ToDo, StepAction::Choose, std::nullopt});

    f.microphone = Permission::Denied;
    REQUIRE(setupSteps(f)[0]
            == StepState{SetupStep::Microphone, StepStatus::Blocked, StepAction::OpenSettings, PrivacyPane::Microphone});
}

TEST_CASE("setupSteps: the system audio step treats an answered prompt as done", "[ui][setup]") {
    auto f = allDone();
    f.systemAudio = Permission::NotDetermined;   // unknown
    REQUIRE(setupSteps(f)[1] == StepState{SetupStep::SystemAudio, StepStatus::ToDo, StepAction::Ask, std::nullopt});
    REQUIRE(currentSetupStep(setupSteps(f), 0) == 1);

    f.systemAudioRequestPending = true;   // the prompt is up (raised here or by the engine)
    REQUIRE(setupSteps(f)[1].status == StepStatus::Waiting);
    REQUIRE(setupSteps(f)[1].action == StepAction::None);

    f.systemAudioRequestPending = false;
    f.systemAudioAnswered = true;   // ok from requestSystemAudio: answered, outcome unknown
    REQUIRE(setupSteps(f)[1].status == StepStatus::Done);

    // The AEC later reports a denied or silent reference: System Settings, with a note.
    for (const auto kind : {WarningKind::SystemAudioDenied, WarningKind::SystemAudioSilent}) {
        f.warning = kind;
        REQUIRE(setupSteps(f)[1]
                == StepState{SetupStep::SystemAudio, StepStatus::Blocked, StepAction::OpenSettings,
                             PrivacyPane::SystemAudio});
    }
    // A tap that could not be created is no reason to send the user to System Settings.
    f.warning = WarningKind::SystemAudioUnavailable;
    REQUIRE(setupSteps(f)[1].status == StepStatus::Done);

    f = allDone();
    f.systemAudio = Permission::Denied;
    REQUIRE(setupSteps(f)[1].status == StepStatus::Blocked);
    REQUIRE(setupSteps(f)[1].pane == PrivacyPane::SystemAudio);
}

TEST_CASE("setupSteps: the driver step", "[ui][setup]") {
    auto f = allDone();
    f.driverInstalled = false;
    REQUIRE(setupSteps(f)[2] == StepState{SetupStep::Driver, StepStatus::ToDo, StepAction::Install, std::nullopt});
    f.driverOperationInProgress = true;
    REQUIRE(setupSteps(f)[2].status == StepStatus::Waiting);
    REQUIRE(setupSteps(f)[2].action == StepAction::None);
    f.driverOperationInProgress = false;
    f.driverBundled = false;   // nothing to install: still to do, no action
    REQUIRE(setupSteps(f)[2] == StepState{SetupStep::Driver, StepStatus::ToDo, StepAction::None, std::nullopt});
}

TEST_CASE("currentSetupStep: the first step not done from where the guide is", "[ui][setup]") {
    SetupFacts first;   // nothing done
    const auto steps = setupSteps(first);
    REQUIRE(currentSetupStep(steps, 0) == 0);
    REQUIRE(currentSetupStep(steps, 1) == 1);   // "Salta" on the first
    REQUIRE(currentSetupStep(steps, 3) == 3);
    REQUIRE(currentSetupStep(steps, 7) == 3);   // clamped
    REQUIRE(currentSetupStep(steps, -2) == 0);

    auto mid = allDone();   // microphone done, system audio pending: the guide opens on step 2
    mid.systemAudio = Permission::NotDetermined;
    mid.driverInstalled = false;
    REQUIRE(currentSetupStep(setupSteps(mid), 0) == 1);
    REQUIRE(currentSetupStep(setupSteps(mid), 2) == 2);
    mid.systemAudioAnswered = true;   // answered: on to the driver
    REQUIRE(currentSetupStep(setupSteps(mid), 0) == 2);
}
