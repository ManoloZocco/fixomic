#include "platform/mac/DriverInstaller.h"

#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/FixomicDevices.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace fixomic::mac {

namespace {

std::atomic<bool> operationRunning{false};

// Italian text with non-ASCII characters (juce::String(const char*) expects ASCII).
juce::String text(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

juce::String toJuce(NSString* string)
{
    return string != nil ? juce::String::fromUTF8(string.UTF8String) : juce::String();
}

NSString* toNs(const juce::String& string)
{
    return [NSString stringWithUTF8String:string.toRawUTF8()];
}

juce::String plistValue(const juce::File& bundle, NSString* key)
{
    @autoreleasepool {
        const auto plist = bundle.getChildFile("Contents/Info.plist");
        NSDictionary* info = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:toNs(plist.getFullPathName())]
                                                                 error:nil];
        id value = info[key];
        return [value isKindOfClass:[NSString class]] ? toJuce(value) : juce::String();
    }
}

bool micPresent()
{
    return findDeviceByUid(devices::kMicUid).has_value();
}

// Polls (background thread) until fixomic Mic is present == `present`, up to kDriverWaitSeconds.
bool waitForMic(bool present)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(kDriverWaitSeconds);
    while (micPresent() != present) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;
}

// By value: blocks capture C++ reference variables by reference, which would dangle on the main
// queue once the background block has returned.
void finish(DriverCompletion completion, bool ok, juce::String error)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        // Cleared on the main queue: until the completion runs (and the engine re-checks the
        // devices) the operation is still in progress. Cleared before it, so the completion and the
        // UI it refreshes see none running and may start the next one.
        operationRunning = false;
        if (completion)
            completion(ok, error);
    });
}

} // namespace

juce::File bundledDriverLocation()
{
    @autoreleasepool {
        NSString* resources = [NSBundle mainBundle].resourcePath;
        if (resources == nil)
            return {};
        return juce::File(toJuce(resources)).getChildFile("fixomic.driver");
    }
}

bool driverInstalled()
{
    return juce::File(kInstalledDriverPath).isDirectory() && micPresent();
}

juce::String bundledDriverVersion()
{
    const auto bundle = bundledDriverLocation();
    return bundle.isDirectory() ? plistValue(bundle, @"CFBundleShortVersionString") : juce::String();
}

juce::String installedDriverVersion()
{
    return plistValue(juce::File(kInstalledDriverPath), @"CFBundleShortVersionString");
}

int compareVersions(const juce::String& a, const juce::String& b)
{
    juce::StringArray left, right;
    left.addTokens(a.trim(), ".", "");
    right.addTokens(b.trim(), ".", "");
    for (int i = 0; i < std::max(left.size(), right.size()); ++i) {
        const auto x = i < left.size() ? left[i].trim().getLargeIntValue() : 0;
        const auto y = i < right.size() ? right[i].trim().getLargeIntValue() : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

bool isDriverUpdate(const juce::String& bundled, const juce::String& installed)
{
    return bundled.isNotEmpty() && installed.isNotEmpty() && compareVersions(bundled, installed) > 0;
}

bool driverOperationInProgress()
{
    return operationRunning;
}

void installDriverAsync(DriverCompletion completion)
{
    driver_script::runOperation(std::move(completion), [] () -> juce::String {
        const auto error = driver_script::installFrom(bundledDriverLocation(), kInstalledDriverPath, driver_script::run);
        if (error.isNotEmpty())
            return error;
        if (!waitForMic(true))
            return text("Il driver \u00e8 stato installato, ma fixomic Mic non \u00e8 comparso entro ")
                   + juce::String(static_cast<int>(kDriverWaitSeconds)) + " secondi. Riavvia il Mac se non compare.";
        return {};
    });
}

void uninstallDriverAsync(DriverCompletion completion)
{
    driver_script::runOperation(std::move(completion), [] () -> juce::String {
        if (!juce::File(kInstalledDriverPath).exists())
            return {};   // nothing to remove: no password, no coreaudiod restart
        const auto result = driver_script::run(driver_script::uninstall(kInstalledDriverPath));
        if (result.cancelled)
            return "Disinstallazione annullata";
        if (!result.ok)
            return "Disinstallazione del driver non riuscita: " + result.message;
        if (!waitForMic(false))
            return text("Il driver \u00e8 stato rimosso, ma fixomic Mic \u00e8 ancora presente dopo ")
                   + juce::String(static_cast<int>(kDriverWaitSeconds)) + " secondi. Riavvia il Mac.";
        return {};
    });
}

// ---- building blocks --------------------------------------------------------------------------

namespace driver_script {

juce::String appleScriptString(const juce::String& text)
{
    return "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
}

namespace {

juce::String doShellScript(const juce::String& commandExpression, const Options& options)
{
    return "do shell script " + commandExpression + (options.administrator ? " with administrator privileges" : "");
}

// AppleScript expression for the coreaudiod restart (appended to a shell command).
juce::String restartTail(const Options& options)
{
    return options.restartCoreAudio
               ? " & \" && { /usr/bin/killall coreaudiod || /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod; }\""
               : juce::String();
}

} // namespace

juce::String install(const juce::String& source, const juce::String& destination, Options options)
{
    const juce::String parent = juce::File(destination).getParentDirectory().getFullPathName();
    juce::String script;
    script << "set src to " << appleScriptString(source) << "\n"
           << "set dst to " << appleScriptString(destination) << "\n"
           << "set dir to " << appleScriptString(parent) << "\n";
    // A driver from a downloaded release carries com.apple.quarantine. `ditto --noqtn` does not
    // propagate quarantine, but still copies the attribute the source already has (macOS 27.2), so
    // it is removed from the copy explicitly (-s: symbolic links themselves, never their targets).
    juce::String command = "\"/bin/mkdir -p \" & quoted form of dir & \" && /bin/rm -rf \" & quoted form of dst"
                           " & \" && /usr/bin/ditto --noqtn \" & quoted form of src & \" \" & quoted form of dst"
                           " & \" && /usr/bin/xattr -drs com.apple.quarantine \" & quoted form of dst";
    if (options.administrator)
        command << " & \" && /usr/sbin/chown -R root:wheel \" & quoted form of dst";
    command << " & \" && /bin/chmod -R go-w \" & quoted form of dst" << restartTail(options);
    script << doShellScript(command, options) << "\n";
    return script;
}

juce::String uninstall(const juce::String& destination, Options options)
{
    juce::String script;
    script << "set dst to " << appleScriptString(destination) << "\n";
    const juce::String command = "\"/bin/rm -rf \" & quoted form of dst" + restartTail(options);
    script << doShellScript(command, options) << "\n";
    return script;
}

juce::String verifySignature(const juce::File& bundle)
{
    const juce::String refused = text("La firma del driver incluso nell'app non \u00e8 valida: installazione "
                                      "rifiutata (");
    juce::ChildProcess codesign;
    if (!codesign.start(juce::StringArray{"/usr/bin/codesign", "--verify", "--strict", bundle.getFullPathName()},
                        juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return refused + "codesign non avviato)";
    const juce::String output = codesign.readAllProcessOutput().trim();
    if (!codesign.waitForProcessToFinish(30000)) {
        codesign.kill();
        return refused + "codesign non ha risposto)";
    }
    if (codesign.getExitCode() == 0)
        return {};
    // "<path>: <reason>": the path is already known, the reason is what helps.
    juce::String reason = output.upToFirstOccurrenceOf("\n", false, false);
    if (reason.startsWith(bundle.getFullPathName() + ": "))
        reason = reason.fromFirstOccurrenceOf(": ", false, false);
    if (reason.isEmpty())
        reason = "codesign: uscita " + juce::String(static_cast<int>(codesign.getExitCode()));
    return refused + reason + ")";
}

juce::String installFrom(const juce::File& source, const juce::String& destination,
                         const std::function<Result(const juce::String&)>& runScript)
{
    if (!source.isDirectory())
        return text("Il driver non \u00e8 incluso nell'app (") + source.getFullPathName() + ")";
    if (plistValue(source, @"CFBundleIdentifier") != kDriverBundleId)
        return text("Il driver incluso nell'app non \u00e8 valido (") + source.getFullPathName() + ")";
    // Before root copies it: a bundle that was changed (or never signed) is not installed.
    if (const auto signature = verifySignature(source); signature.isNotEmpty())
        return signature;

    const auto result = runScript(install(source.getFullPathName(), destination));
    if (result.cancelled)
        return "Installazione annullata";
    if (!result.ok)
        return "Installazione del driver non riuscita: " + result.message;
    return {};
}

void runOperation(DriverCompletion completion, std::function<juce::String()> work)
{
    if (operationRunning.exchange(true)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion)
                completion(false, text("Un'altra operazione sul driver \u00e8 in corso"));
        });
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const juce::String error = work();
        finish(completion, error.isEmpty(), error);
    });
}

Result interpret(bool scriptCreated, bool descriptorReturned, bool hasErrorInfo, int errorNumber,
                 const juce::String& errorMessage)
{
    Result result;
    if (!scriptCreated) {
        result.message = text("lo script di installazione non \u00e8 stato creato");
        return result;
    }
    if (hasErrorInfo) {
        result.cancelled = errorNumber == -128;   // userCanceledErr
        result.message = errorMessage.isNotEmpty() ? errorMessage : "errore AppleScript " + juce::String(errorNumber);
        return result;
    }
    if (!descriptorReturned) {
        result.message = "lo script non ha dato alcun risultato";
        return result;
    }
    result.ok = true;
    return result;
}

Result run(const juce::String& source)
{
    @autoreleasepool {
        NSString* sourceText = toNs(source);   // nil for a string NSString cannot take
        NSAppleScript* script = sourceText != nil ? [[NSAppleScript alloc] initWithSource:sourceText] : nil;
        NSDictionary* errorInfo = nil;
        NSAppleEventDescriptor* descriptor = script != nil ? [script executeAndReturnError:&errorInfo] : nil;
        return interpret(script != nil, descriptor != nil, errorInfo != nil,
                         [errorInfo[NSAppleScriptErrorNumber] intValue], toJuce(errorInfo[NSAppleScriptErrorMessage]));
    }
}

} // namespace driver_script

} // namespace fixomic::mac
