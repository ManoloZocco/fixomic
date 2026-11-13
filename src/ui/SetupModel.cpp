#include "ui/SetupModel.h"

#include "ui/StatusModel.h"

#include <algorithm>
#include <cmath>

namespace fixomic::ui {

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr double kSampleRate = 48000.0;

bool offers48k(const std::vector<double>& rates) {
    return std::any_of(rates.begin(), rates.end(), [](double rate) { return std::abs(rate - kSampleRate) < 0.5; });
}

} // namespace

// ---- warnings ---------------------------------------------------------------------------------

std::optional<mac::PrivacyPane> warningPane(mac::WarningKind kind) {
    switch (kind) {
    case mac::WarningKind::SystemAudioDenied:
    case mac::WarningKind::SystemAudioSilent: return mac::PrivacyPane::SystemAudio;
    case mac::WarningKind::SystemAudioUnavailable: break;
    }
    return std::nullopt;
}

// ---- microphone -------------------------------------------------------------------------------

std::vector<DeviceChoice> deviceChoices(const std::vector<mac::AudioDeviceInfo>& devices,
                                        const juce::String& configuredUid, const juce::String& configuredName) {
    std::vector<DeviceChoice> choices;
    const bool connected = std::any_of(devices.begin(), devices.end(),
                                       [&](const mac::AudioDeviceInfo& d) { return d.uid == configuredUid; });
    if (configuredUid.isNotEmpty() && !connected) {
        const auto name = configuredName.isNotEmpty() ? configuredName : configuredUid;
        choices.push_back({configuredUid, name, name + utf8(" — non collegato"), true, false, 0, true});
    }
    for (const auto& d : devices)
        choices.push_back({d.uid, d.name, d.name, d.uid == configuredUid, true, d.inputChannels, offers48k(d.sampleRates)});
    return choices;
}

std::optional<DeviceChoice> selectedDevice(const std::vector<DeviceChoice>& choices) {
    const auto it = std::find_if(choices.begin(), choices.end(), [](const DeviceChoice& c) { return c.selected; });
    if (it == choices.end()) return std::nullopt;
    return *it;
}

std::optional<juce::String> inputNameToRemember(const std::vector<mac::AudioDeviceInfo>& devices,
                                                const juce::String& configuredUid, const juce::String& storedName) {
    if (configuredUid.isEmpty() || storedName.isNotEmpty()) return std::nullopt;
    for (const auto& d : devices)
        if (d.uid == configuredUid) return d.name;
    return std::nullopt;
}

juce::StringArray channelChoices(int inputChannels) {
    juce::StringArray channels;
    if (inputChannels < 2) return channels;
    for (int i = 1; i <= inputChannels; ++i) channels.add("Canale " + juce::String(i));
    return channels;
}

juce::String sampleRateWarning(const std::vector<DeviceChoice>& choices) {
    const auto selected = selectedDevice(choices);
    if (!selected || !selected->connected || selected->supports48k) return {};
    EngineSnapshot unsupported;
    unsupported.state = EngineState::UnsupportedSampleRate;
    return presentStatus(unsupported).text;
}

// ---- buffer -----------------------------------------------------------------------------------

juce::String formatBufferMs(int samples) { return formatMs(static_cast<double>(samples) / kSampleRate * 1000.0); }

juce::String formatBufferChoice(int samples) { return juce::String(samples) + utf8(" · ") + formatBufferMs(samples); }

// ---- driver -----------------------------------------------------------------------------------

juce::String driverStatusText(const DriverFacts& driver) {
    if (!driver.installed) return "Non installato";
    if (driver.installedVersion.isEmpty()) return "Installato";
    return utf8("Installato · v") + driver.installedVersion;
}

juce::String driverInstallActionText(const DriverFacts& driver) {
    if (driver.installed) {
        if (!driver.updateAvailable) return {};
        return "Aggiorna driver (v" + driver.installedVersion + utf8(" → v") + driver.bundledVersion + ")";
    }
    return driver.bundledVersion.isNotEmpty() ? juce::String("Installa driver") : juce::String();
}

bool canUninstallDriver(const DriverFacts& driver) { return driver.installed || driver.installedVersion.isNotEmpty(); }

// ---- permissions ------------------------------------------------------------------------------

juce::String microphonePermissionText(mac::Permission permission) {
    switch (permission) {
    case mac::Permission::Granted: return "Consentito";
    case mac::Permission::Denied: return "Negato";
    case mac::Permission::NotDetermined: break;
    }
    return "Non ancora chiesto";
}

juce::String systemAudioPermissionText(mac::Permission permission, std::optional<mac::WarningKind> warning) {
    switch (permission) {
    case mac::Permission::Granted: return "Consentito";
    case mac::Permission::Denied: return "Negato";
    case mac::Permission::NotDetermined: break;
    }
    return warning == mac::WarningKind::SystemAudioSilent ? "Probabilmente negato" : "Non noto";
}

// ---- first-run guide --------------------------------------------------------------------------

std::array<StepState, kSetupStepCount> setupSteps(const SetupFacts& f) {
    std::array<StepState, kSetupStepCount> steps;

    auto& mic = steps[0];
    mic.step = SetupStep::Microphone;
    switch (f.microphone) {
    case mac::Permission::Granted:
        mic.status = f.microphoneChosen ? StepStatus::Done : StepStatus::ToDo;
        mic.action = f.microphoneChosen ? StepAction::None : StepAction::Choose;
        break;
    case mac::Permission::Denied:
        mic.status = StepStatus::Blocked;
        mic.action = StepAction::OpenSettings;
        mic.pane = mac::PrivacyPane::Microphone;
        break;
    case mac::Permission::NotDetermined:
        mic.status = f.microphoneRequestPending ? StepStatus::Waiting : StepStatus::ToDo;
        mic.action = f.microphoneRequestPending ? StepAction::None : StepAction::Ask;
        break;
    }

    auto& audio = steps[1];
    audio.step = SetupStep::SystemAudio;
    const bool deniedOrSilent =
        f.warning == mac::WarningKind::SystemAudioDenied || f.warning == mac::WarningKind::SystemAudioSilent;
    if (f.systemAudio == mac::Permission::Denied || deniedOrSilent) {
        audio.status = StepStatus::Blocked;
        audio.action = StepAction::OpenSettings;
        audio.pane = mac::PrivacyPane::SystemAudio;
    } else if (f.systemAudio == mac::Permission::Granted || (f.systemAudioAnswered && !f.systemAudioRequestPending)) {
        audio.status = StepStatus::Done;   // answered: the outcome shows later, as a warning if denied
    } else if (f.systemAudioRequestPending) {
        audio.status = StepStatus::Waiting;
    } else {
        audio.status = StepStatus::ToDo;
        audio.action = StepAction::Ask;
    }

    auto& driver = steps[2];
    driver.step = SetupStep::Driver;
    if (f.driverInstalled) {
        driver.status = StepStatus::Done;
    } else if (f.driverOperationInProgress) {
        driver.status = StepStatus::Waiting;
    } else {
        driver.status = StepStatus::ToDo;
        driver.action = f.driverBundled ? StepAction::Install : StepAction::None;
    }

    steps[3] = {SetupStep::ChooseInApp, StepStatus::ToDo, StepAction::Finish, std::nullopt};
    return steps;
}

int currentSetupStep(const std::array<StepState, kSetupStepCount>& steps, int from) {
    for (int i = std::max(0, from); i < kSetupStepCount; ++i)
        if (steps[static_cast<size_t>(i)].status != StepStatus::Done) return i;
    return kSetupStepCount - 1;
}

} // namespace fixomic::ui
