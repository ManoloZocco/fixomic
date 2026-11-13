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

// Checks the bundled driver (driver_script::installFrom: a directory, the fixomic bundle id, a valid
// code signature), then copies it over kInstalledDriverPath through NSAppleScript "do shell script ...
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

// How run() reads NSAppleScript's outcome: ok only for a script that was created and returned a
// result descriptor without error information. A script that could not be created, or no result
// and no error, is a failure with a message, never a success. errorNumber / errorMessage are read
// only with hasErrorInfo (-128 = the user cancelled).
Result interpret(bool scriptCreated, bool descriptorReturned, bool hasErrorInfo, int errorNumber,
                 const juce::String& errorMessage);

// `/usr/bin/codesign --verify --strict <bundle>`: "" when the bundle's code signature is valid,
// otherwise an Italian error ending with codesign's own message in parentheses. Blocking (runs
// codesign, up to 30 s); any thread.
juce::String verifySignature(const juce::File& bundle);

// installDriverAsync's work up to the privileged copy: `source` must be a directory with the fixomic
// bundle id (kDriverBundleId) and a valid code signature (verifySignature); only then is the install
// script (install(source, destination)) handed to `runScript` (run() in the app), so a refused
// driver never reaches root. "" once the script ran; otherwise the Italian error ("Installazione
// annullata" when the user cancelled the authorization).
juce::String installFrom(const juce::File& source, const juce::String& destination,
                         const std::function<Result(const juce::String& script)>& runScript);

// The machinery of installDriverAsync / uninstallDriverAsync: runs `work` (returns the error, ""
// on success) on a background queue, at most one at a time (otherwise fails at once), and calls
// `completion` on the main queue.
void runOperation(DriverCompletion completion, std::function<juce::String()> work);

} // namespace driver_script

} // namespace fixomic::mac
