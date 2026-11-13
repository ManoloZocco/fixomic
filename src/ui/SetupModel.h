#pragma once

#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/Permissions.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>
#include <vector>

namespace fixomic::ui {

// The pure logic of the settings view and the first-run guide: plain values in, plain values out.
// No JUCE GUI, no AppController.

// ---- warnings ------------------------------------------------------------------------------------

// The System Settings pane that can fix a backend warning: the system audio pane for a denied or
// silent tap; none when the tap or the prompt's throwaway IO failed (opening a pane cannot fix that,
// and the prompt may not even have been shown).
std::optional<mac::PrivacyPane> warningPane(mac::WarningKind);

// ---- microphone ----------------------------------------------------------------------------------

struct DeviceChoice {
    juce::String uid;
    juce::String name;
    juce::String text;           // the name, or "<name> — non collegato"
    bool selected = false;       // the configured device
    bool connected = true;
    int inputChannels = 0;       // 0 when not connected
    bool supports48k = true;     // assumed for a device that is not connected (unknown)
    bool operator==(const DeviceChoice&) const = default;
};

// The microphones to choose from, in the system's order, the configured one marked. A configured
// device that is not connected comes first, as "<name> — non collegato", selected: the choice is never
// moved to another device by itself. `configuredName` is the name stored with the choice ("" for a
// config written before names were stored: the uid stands in).
std::vector<DeviceChoice> deviceChoices(const std::vector<mac::AudioDeviceInfo>& devices,
                                        const juce::String& configuredUid, const juce::String& configuredName);
// The selected entry, if any.
std::optional<DeviceChoice> selectedDevice(const std::vector<DeviceChoice>&);
// The name to store with the configured microphone: its name, when none is stored yet (a config
// written before names were) and it is connected now; nullopt otherwise.
std::optional<juce::String> inputNameToRemember(const std::vector<mac::AudioDeviceInfo>& devices,
                                                const juce::String& configuredUid, const juce::String& storedName);

// "Canale 1", "Canale 2", …; empty for a device with fewer than two input channels (nothing to choose).
juce::StringArray channelChoices(int inputChannels);

// The UnsupportedSampleRate status text when the selected device is connected and lacks 48 kHz; ""
// otherwise (also when it is not connected: its rates are unknown).
juce::String sampleRateWarning(const std::vector<DeviceChoice>&);

// ---- buffer --------------------------------------------------------------------------------------

inline constexpr std::array<int, 3> kBufferSizes{64, 128, 256};
juce::String formatBufferMs(int samples);       // at 48 kHz: 64 -> "1,3 ms"
juce::String formatBufferChoice(int samples);   // "128 · 2,7 ms"

// ---- driver --------------------------------------------------------------------------------------

struct DriverFacts {
    bool installed = false;           // installed and fixomic Mic published
    juce::String installedVersion;    // "" when not installed / unreadable
    juce::String bundledVersion;      // "" when the app carries no driver
    bool updateAvailable = false;     // both known, bundled > installed
    bool operationInProgress = false;
    bool operator==(const DriverFacts&) const = default;
};

juce::String driverStatusText(const DriverFacts&);     // "Installato · v0.1.0", "Installato", "Non installato"
// The install action: "Installa driver" (not installed, a driver bundled), "Aggiorna driver (v0.1.0 →
// v0.2.0)" (update available); "" when neither applies.
juce::String driverInstallActionText(const DriverFacts&);
inline constexpr const char* kUninstallDriverText = "Disinstalla driver";
// Something to remove: installed, or its files still in place (fixomic Mic not published).
bool canUninstallDriver(const DriverFacts&);

// ---- permissions ---------------------------------------------------------------------------------

juce::String microphonePermissionText(mac::Permission);   // "Consentito", "Negato", "Non ancora chiesto"
// NotDetermined is "unknown" (the preflight is stale in this process, or not available), never
// "denied": "Non noto", or "Da controllare" while the tap is reported silent (not a denial either).
juce::String systemAudioPermissionText(mac::Permission, std::optional<mac::WarningKind>);

// ---- first-run guide -----------------------------------------------------------------------------

// What the first-run guide does at launch, from the config as loaded: whether it stated onboardingDone
// (AppController::onboardingFlagStored) and its value.
enum class LaunchGuide {
    None,       // set up: nothing
    Open,       // a first launch: the panel opens on the guide by itself
    Reminder,   // closed early before: nothing opens, the panel shows its reminder row
};
// Open only when the flag was not stated and the config is not onboarded: no file, or a document from
// before the flag that the old-config rule (Config.h) does not count as set up. A stated false is a
// guide closed early: Reminder. Never Open at every login.
LaunchGuide launchGuide(bool onboardingFlagStored, bool onboardingDone);

enum class SetupStep { Microphone, SystemAudio, Driver, ChooseInApp };
inline constexpr int kSetupStepCount = 4;

enum class StepStatus {
    Done,
    ToDo,
    Waiting,   // a system prompt is up, or the driver operation runs
    Blocked,   // denied: only System Settings can fix it
};

// What the step offers.
enum class StepAction {
    None,
    Ask,            // raise the permission prompt
    Choose,         // pick the microphone
    OpenSettings,   // the System Settings pane in `pane`
    Install,        // install the driver
    Finish,         // "Fine"
};

struct StepState {
    SetupStep step = SetupStep::Microphone;
    StepStatus status = StepStatus::ToDo;
    StepAction action = StepAction::None;
    std::optional<mac::PrivacyPane> pane;
    bool operator==(const StepState&) const = default;
};

struct SetupFacts {
    mac::Permission microphone = mac::Permission::NotDetermined;
    bool microphoneRequestPending = false;    // the microphone prompt is up
    bool microphoneChosen = false;            // config().inputDeviceUid is set
    mac::Permission systemAudio = mac::Permission::NotDetermined;   // NotDetermined = unknown
    bool systemAudioRequestPending = false;   // the System Audio Recording prompt is up
    bool systemAudioAnswered = false;         // a prompt raised in this process was answered
    std::optional<mac::WarningKind> warning;  // the AEC's last warning
    bool driverInstalled = false;
    bool driverOperationInProgress = false;
    bool driverBundled = true;                // the app carries a driver to install
    bool operator==(const SetupFacts&) const = default;
};

// The four steps in order: microphone (permission, then the device), system audio recording, driver,
// "choose fixomic Mic in your app" (never done: it ends with "Fine").
std::array<StepState, kSetupStepCount> setupSteps(const SetupFacts&);

// The step the guide has open: the first one at or after `from` that is not done (steps already
// satisfied are passed over; "Salta" moves `from` past the open one). Always found: the last step is
// never done.
int currentSetupStep(const std::array<StepState, kSetupStepCount>&, int from);

// What the settings offer for a permission: None when granted. The microphone: Ask while macOS has
// not asked (fixomic is not listed in System Settings yet), OpenSettings when denied. System audio:
// OpenSettings unless granted (NotDetermined is unknown, maybe denied).
StepAction microphoneSettingsAction(mac::Permission);
StepAction systemAudioSettingsAction(mac::Permission);

} // namespace fixomic::ui
