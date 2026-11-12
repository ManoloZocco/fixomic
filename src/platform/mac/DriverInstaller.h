#pragma once

// Installs / removes the fixomic HAL driver (fixomic Mic + fixomic Bridge). The app carries the
// driver at Contents/Resources/fixomic.driver (copied there by CMake from build/driver). Plain C++
// interface; implemented in DriverInstaller.mm.

#include <juce_core/juce_core.h>

#include <functional>

namespace fixomic::mac {

inline constexpr const char* kHalPlugInsDir = "/Library/Audio/Plug-Ins/HAL";
inline constexpr const char* kInstalledDriverPath = "/Library/Audio/Plug-Ins/HAL/fixomic.driver";
inline constexpr const char* kDriverBundleId = "it.manolozocco.fixomic.driver";
inline constexpr double kDriverWaitSeconds = 10.0;   // for fixomic Mic to appear / disappear

// <main bundle>/Contents/Resources/fixomic.driver (it may not exist, e.g. outside the app bundle).
juce::File bundledDriverLocation();

// kInstalledDriverPath exists and Core Audio publishes fixomic Mic (kMicUid). Any thread.
bool driverInstalled();

// CFBundleShortVersionString of the bundled / installed driver, "" when absent or unreadable.
// Read from the Info.plist on every call (no NSBundle cache), so a reinstall shows at once.
juce::String bundledDriverVersion();
juce::String installedDriverVersion();

// Numeric dotted comparison ("0.10.0" > "0.9.1"; missing components count as 0; a component's
// leading digits are its value). <0, 0, >0 like strcmp.
int compareVersions(const juce::String& a, const juce::String& b);
// Both known and bundled > installed.
bool isDriverUpdate(const juce::String& bundled, const juce::String& installed);

// The completion runs on the main queue. ok = false with an Italian `error`; a cancelled
// authorization dialog gives "Installazione annullata" / "Disinstallazione annullata".
using DriverCompletion = std::function<void(bool ok, juce::String error)>;

// Copies the bundled driver over kInstalledDriverPath through NSAppleScript "do shell script ...
// with administrator privileges" (rm + ditto, quarantine removed from the copy, chown root:wheel,
// then coreaudiod restarted with killall, or launchctl kickstart if killall fails), then waits up to
// kDriverWaitSeconds for fixomic Mic to appear. Everything runs on a background queue: the
// authorization dialog is modal and the script blocks until it is answered. Only fixomic.driver is
// touched in the HAL folder.
void installDriverAsync(DriverCompletion);
// Removes kInstalledDriverPath (same mechanism) and waits up to kDriverWaitSeconds for fixomic Mic
// to disappear.
void uninstallDriverAsync(DriverCompletion);
// An install / uninstall is running (a second one fails at once with an error): from the call
// until its completion starts on the main queue.
bool driverOperationInProgress();

// ---- building blocks (pure, unit-tested) -------------------------------------------------------
namespace driver_script {

// An AppleScript string literal: the text in double quotes, with \ and " escaped.
juce::String appleScriptString(const juce::String&);

struct Options {
    bool administrator = true;      // "with administrator privileges"; also chown -R root:wheel
    bool restartCoreAudio = true;   // killall coreaudiod || launchctl kickstart -k
};

// AppleScript source. Paths reach the shell only through `quoted form of`, so any character
// (spaces, quotes, $, backslashes) is safe.
juce::String install(const juce::String& source, const juce::String& destination, Options = {});
juce::String uninstall(const juce::String& destination, Options = {});

struct Result {
    bool ok = false;
    bool cancelled = false;   // the user dismissed the authorization dialog (error -128)
    juce::String message;     // AppleScript error message when !ok
};
// Compiles and runs `source` with NSAppleScript. Blocking; any thread.
Result run(const juce::String& source);

// The machinery of installDriverAsync / uninstallDriverAsync: runs `work` (returns the error, ""
// on success) on a background queue, at most one at a time (otherwise fails at once), and calls
// `completion` on the main queue.
void runOperation(DriverCompletion completion, std::function<juce::String()> work);

} // namespace driver_script

} // namespace fixomic::mac
