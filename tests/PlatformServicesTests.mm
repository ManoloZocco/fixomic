#include <catch2/catch_test_macros.hpp>

#include "platform/mac/DriverInstaller.h"
#include "platform/mac/LoginItem.h"
#include "platform/mac/Permissions.h"
#include "platform/mac/TapSilenceWatch.h"

#import <Foundation/Foundation.h>

#include <sys/xattr.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace mac = fixomic::mac;
namespace script = fixomic::mac::driver_script;

TEST_CASE("compareVersions orders dotted versions numerically", "[driver][installer]")
{
    REQUIRE(mac::compareVersions("0.1.0", "0.1.0") == 0);
    REQUIRE(mac::compareVersions("0.1", "0.1.0") == 0);
    REQUIRE(mac::compareVersions("0.1.1", "0.1.0") > 0);
    REQUIRE(mac::compareVersions("0.10.0", "0.9.9") > 0);
    REQUIRE(mac::compareVersions("1.0", "0.99.99") > 0);
    REQUIRE(mac::compareVersions("0.2.0", "0.10.0") < 0);
    REQUIRE(mac::compareVersions(" 1.2.3 ", "1.2.3") == 0);
    REQUIRE(mac::compareVersions("1.2.3b", "1.2.3") == 0);   // leading digits only

    REQUIRE(mac::isDriverUpdate("0.2.0", "0.1.0"));
    REQUIRE_FALSE(mac::isDriverUpdate("0.1.0", "0.1.0"));
    REQUIRE_FALSE(mac::isDriverUpdate("0.1.0", "0.2.0"));
    REQUIRE_FALSE(mac::isDriverUpdate("0.2.0", ""));   // not installed: an install, not an update
    REQUIRE_FALSE(mac::isDriverUpdate("", "0.1.0"));
}

TEST_CASE("appleScriptString escapes quotes and backslashes", "[driver][installer]")
{
    REQUIRE(script::appleScriptString("plain") == "\"plain\"");
    REQUIRE(script::appleScriptString("a \"b\" c") == "\"a \\\"b\\\" c\"");
    REQUIRE(script::appleScriptString("C:\\x") == "\"C:\\\\x\"");
    REQUIRE(script::appleScriptString("it's") == "\"it's\"");
}

TEST_CASE("install and uninstall scripts pass paths only through quoted form of", "[driver][installer]")
{
    const juce::String src = "/Applications/My \"fixomic\" app.app/Contents/Resources/fixomic.driver";
    const auto install = script::install(src, mac::kInstalledDriverPath);
    REQUIRE(install.contains("set src to " + script::appleScriptString(src)));
    REQUIRE(install.contains("set dst to \"/Library/Audio/Plug-Ins/HAL/fixomic.driver\""));
    REQUIRE(install.contains("set dir to \"/Library/Audio/Plug-Ins/HAL\""));
    REQUIRE(install.contains("/usr/bin/ditto --noqtn \" & quoted form of src & \" \" & quoted form of dst"));
    REQUIRE(install.contains("/usr/bin/xattr -drs com.apple.quarantine \" & quoted form of dst"));
    REQUIRE(install.contains("/bin/rm -rf \" & quoted form of dst"));
    REQUIRE(install.contains("/usr/sbin/chown -R root:wheel"));
    REQUIRE(install.contains("killall coreaudiod || /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod"));
    REQUIRE(install.trimEnd().endsWith("with administrator privileges"));
    REQUIRE_FALSE(install.contains("HAL/*"));   // never a wildcard in the HAL folder

    const auto uninstall = script::uninstall(mac::kInstalledDriverPath);
    REQUIRE(uninstall.contains("/bin/rm -rf \" & quoted form of dst"));
    REQUIRE(uninstall.contains("killall coreaudiod"));
    REQUIRE(uninstall.trimEnd().endsWith("with administrator privileges"));

    const auto plain = script::install(src, "/tmp/x/fixomic.driver", {false, false});
    REQUIRE_FALSE(plain.contains("administrator"));
    REQUIRE_FALSE(plain.contains("chown"));
    REQUIRE_FALSE(plain.contains("coreaudiod"));
}

namespace {

constexpr const char* kQuarantine = "com.apple.quarantine";

bool quarantined(const juce::File& file)
{
    return getxattr(file.getFullPathName().toRawUTF8(), kQuarantine, nullptr, 0, 0, XATTR_NOFOLLOW) >= 0;
}

bool quarantine(const juce::File& file)
{
    // What a browser / Archive Utility puts on every file of a downloaded release.
    const char value[] = "0083;6af60800;Safari;8E5A1B2C-1111-2222-3333-444455556666";
    return setxattr(file.getFullPathName().toRawUTF8(), kQuarantine, value, sizeof(value) - 1, 0, XATTR_NOFOLLOW) == 0;
}

// A uniquely named temporary directory, removed at the end of the scope (test processes may run
// concurrently).
struct TempDirectory {
    juce::File root;
    explicit TempDirectory(const juce::String& prefix)
        : root(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile(prefix + " " + juce::Uuid().toDashedString()))
    {
    }
    ~TempDirectory() { root.deleteRecursively(); }
};

} // namespace

TEST_CASE("install and uninstall scripts work with spaces, quotes and shell characters", "[driver][installer]")
{
    // The same scripts, without administrator privileges and without the coreaudiod restart,
    // on temporary paths full of characters a naive quoting would break on.
    const TempDirectory temp("fixomic installer test 'q' \"dq\" $HOME `x` \\b");
    const auto& root = temp.root;
    REQUIRE(root.createDirectory());
    const auto source = root.getChildFile("src dir 'a'/fixomic.driver");
    REQUIRE(source.getChildFile("Contents/MacOS").createDirectory());
    REQUIRE(source.getChildFile("Contents/Info.plist").replaceWithText("plist"));
    REQUIRE(source.getChildFile("Contents/MacOS/fixomic").replaceWithText("binary"));
    // A driver from a downloaded (quarantined) release: the installed copy must not stay quarantined.
    for (const auto& file : {source, source.getChildFile("Contents"), source.getChildFile("Contents/Info.plist"),
                             source.getChildFile("Contents/MacOS"), source.getChildFile("Contents/MacOS/fixomic")})
        REQUIRE(quarantine(file));
    const auto destination = root.getChildFile("HAL \"dest\" $(echo no)/fixomic.driver");
    REQUIRE(destination.getChildFile("Contents/old.txt").create());   // replaced, not merged

    auto result = script::run(script::install(source.getFullPathName(), destination.getFullPathName(), {false, false}));
    INFO(result.message);
    REQUIRE(result.ok);
    REQUIRE(destination.getChildFile("Contents/Info.plist").loadFileAsString() == "plist");
    REQUIRE_FALSE(destination.getChildFile("Contents/old.txt").exists());
    REQUIRE(source.getChildFile("Contents/Info.plist").exists());
    for (const auto& file : {destination, destination.getChildFile("Contents"),
                             destination.getChildFile("Contents/Info.plist"),
                             destination.getChildFile("Contents/MacOS"),
                             destination.getChildFile("Contents/MacOS/fixomic")}) {
        INFO(file.getFullPathName());
        REQUIRE_FALSE(quarantined(file));
    }
    REQUIRE(quarantined(source.getChildFile("Contents/Info.plist")));   // the source is left alone

    result = script::run(script::uninstall(destination.getFullPathName(), {false, false}));
    REQUIRE(result.ok);
    REQUIRE_FALSE(destination.exists());
    REQUIRE(destination.getParentDirectory().isDirectory());   // only the driver bundle goes
    REQUIRE(source.isDirectory());
}

TEST_CASE("driver_script::run reports AppleScript errors", "[driver][installer]")
{
    const auto failed = script::run("do shell script \"exit 3\"");
    REQUIRE_FALSE(failed.ok);
    REQUIRE_FALSE(failed.cancelled);
    REQUIRE(failed.message.isNotEmpty());

    const auto cancelled = script::run("error number -128");
    REQUIRE_FALSE(cancelled.ok);
    REQUIRE(cancelled.cancelled);
}

TEST_CASE("driver versions are read from the Info.plist", "[driver][installer]")
{
    // fixomic_tests is not an app bundle: nothing is bundled.
    REQUIRE(mac::bundledDriverVersion().isEmpty());
    if (juce::File(mac::kInstalledDriverPath).isDirectory())
        REQUIRE(mac::compareVersions(mac::installedDriverVersion(), "0.0.1") >= 0);
    else
        REQUIRE(mac::installedDriverVersion().isEmpty());
}

TEST_CASE("TapSilenceWatch warns after 10 s of silence while output runs", "[permissions]")
{
    SECTION("silent tap, output running")
    {
        mac::TapSilenceWatch watch;
        for (int i = 0; i < 9; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        REQUIRE(watch.update(false, true, 1.0));
        REQUIRE_FALSE(watch.update(false, true, 1.0));   // once
    }
    SECTION("output idle pauses the count")
    {
        mac::TapSilenceWatch watch;
        for (int i = 0; i < 5; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        for (int i = 0; i < 60; ++i)
            REQUIRE_FALSE(watch.update(false, false, 1.0));
        for (int i = 0; i < 4; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        REQUIRE(watch.update(false, true, 1.0));
    }
    SECTION("any signal settles it")
    {
        mac::TapSilenceWatch watch;
        for (int i = 0; i < 9; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        REQUIRE_FALSE(watch.update(true, true, 1.0));
        for (int i = 0; i < 30; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        watch.reset();
        for (int i = 0; i < 9; ++i)
            REQUIRE_FALSE(watch.update(false, true, 1.0));
        REQUIRE(watch.update(false, true, 1.0));
    }
}

TEST_CASE("systemAudioPermission answers without prompting", "[permissions]")
{
    const auto status = mac::systemAudioPermission();
    REQUIRE((status == mac::Permission::Granted || status == mac::Permission::Denied
             || status == mac::Permission::NotDetermined));
}

TEST_CASE("launch at login is readable outside an app bundle", "[loginitem]")
{
    REQUIRE_FALSE(mac::launchAtLoginEnabled());   // fixomic_tests is never a login item
}

namespace {

template <typename Predicate>
void pumpMainQueue(double seconds, Predicate done)
{
    const auto end = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < end && !done())
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
}

} // namespace

TEST_CASE("driver operations run off the main thread, one at a time, and complete on the main queue",
          "[driver][installer]")
{
    std::atomic<bool> release{false};
    std::atomic<bool> workOnMain{true};
    std::atomic<bool> workReturning{false};
    std::vector<std::pair<bool, juce::String>> results;
    bool completionOnMain = false;
    bool inProgressInCompletion = true;

    script::runOperation(
        [&](bool ok, juce::String error) {
            completionOnMain = [NSThread isMainThread];
            inProgressInCompletion = mac::driverOperationInProgress();
            results.emplace_back(ok, error);
        },
        [&]() -> juce::String {
            workOnMain = [NSThread isMainThread];
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            workReturning = true;
            return juce::String("boom ") + juce::String(42);   // a heap string, freed if captured badly
        });
    REQUIRE(mac::driverOperationInProgress());

    // A second operation while the first runs fails at once, without running its work.
    bool secondRan = false;
    script::runOperation([&](bool ok, juce::String error) { results.emplace_back(ok, error); },
                         [&]() -> juce::String {
                             secondRan = true;
                             return {};
                         });
    pumpMainQueue(2.0, [&] { return results.size() == 1; });
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0].first);
    REQUIRE(results[0].second.contains("in corso"));

    release = true;
    // The work has returned on its background thread, the main queue has not run yet: until the
    // completion runs (AppController re-checks the engine there) the operation is in progress.
    while (!workReturning)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(mac::driverOperationInProgress());
    REQUIRE(results.size() == 1);

    pumpMainQueue(5.0, [&] { return results.size() == 2; });
    REQUIRE(results.size() == 2);
    REQUIRE_FALSE(results[1].first);
    REQUIRE(results[1].second == "boom 42");
    REQUIRE(completionOnMain);
    // Cleared just before the completion: it (and the UI it refreshes) sees no operation running
    // and may start the next one.
    REQUIRE_FALSE(inProgressInCompletion);
    REQUIRE_FALSE(workOnMain);
    REQUIRE_FALSE(secondRan);
    REQUIRE_FALSE(mac::driverOperationInProgress());

    script::runOperation([&](bool ok, juce::String error) { results.emplace_back(ok, error); },
                         [] { return juce::String(); });
    pumpMainQueue(5.0, [&] { return results.size() == 3; });
    REQUIRE(results.size() == 3);
    REQUIRE(results[2].first);
    REQUIRE(results[2].second.isEmpty());
}

// ---- final review fixes: the bundled driver's signature, NSAppleScript's outcome -----------------

namespace {

// Runs a tool and returns its exit code (output discarded).
int runTool(const juce::StringArray& args)
{
    juce::ChildProcess process;
    if (!process.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return -1;
    process.readAllProcessOutput();
    process.waitForProcessToFinish(30000);
    return static_cast<int>(process.getExitCode());
}

// A minimal driver-like bundle: the fixomic bundle id and a copy of /usr/bin/true as its
// executable, signed ad-hoc unless `sign` is false.
juce::File makeDriverBundle(const juce::File& bundle, bool sign)
{
    REQUIRE(bundle.getChildFile("Contents/MacOS").createDirectory());
    REQUIRE(bundle.getChildFile("Contents/Info.plist").replaceWithText(
        juce::String("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                     "<plist version=\"1.0\"><dict>"
                     "<key>CFBundleIdentifier</key><string>")
        + mac::kDriverBundleId
        + "</string><key>CFBundleExecutable</key><string>fixomic</string>"
          "<key>CFBundlePackageType</key><string>BNDL</string></dict></plist>\n"));
    const auto executable = bundle.getChildFile("Contents/MacOS/fixomic");
    REQUIRE(juce::File("/usr/bin/true").copyFileTo(executable));
    REQUIRE(runTool({"/usr/bin/codesign", "--remove-signature", executable.getFullPathName()}) == 0);
    if (sign)
        REQUIRE(runTool({"/usr/bin/codesign", "--force", "--sign", "-", bundle.getFullPathName()}) == 0);
    return bundle;
}

} // namespace

TEST_CASE("the installer refuses a bundled driver whose signature does not verify", "[driver][installer]")
{
    const TempDirectory temp("fixomic signature test");
    REQUIRE(temp.root.createDirectory());
    const juce::String destination = temp.root.getChildFile("HAL/fixomic.driver").getFullPathName();
    std::vector<juce::String> scripts;
    script::Result answer{true, false, {}};
    const auto runner = [&](const juce::String& source) {
        scripts.push_back(source);
        return answer;
    };
    const juce::String refused = juce::String::fromUTF8("La firma del driver incluso nell'app non è valida");

    SECTION("signed and intact: verified, then the install script runs")
    {
        const auto bundle = makeDriverBundle(temp.root.getChildFile("signed/fixomic.driver"), true);
        REQUIRE(script::verifySignature(bundle).isEmpty());
        REQUIRE(script::installFrom(bundle, destination, runner).isEmpty());
        REQUIRE(scripts.size() == 1);
        REQUIRE(scripts[0] == script::install(bundle.getFullPathName(), destination));

        answer = {false, true, "User canceled."};
        REQUIRE(script::installFrom(bundle, destination, runner) == "Installazione annullata");
        answer = {false, false, "boom"};
        REQUIRE(script::installFrom(bundle, destination, runner) == "Installazione del driver non riuscita: boom");
    }
    SECTION("changed after signing: refused, no script")
    {
        const auto bundle = makeDriverBundle(temp.root.getChildFile("tampered/fixomic.driver"), true);
        REQUIRE(bundle.getChildFile("Contents/MacOS/fixomic").appendText("tampered"));
        const auto error = script::verifySignature(bundle);
        REQUIRE(error.startsWith(refused));
        REQUIRE(error.endsWith(")"));   // codesign's own message, in parentheses
        REQUIRE(script::installFrom(bundle, destination, runner).startsWith(refused));
        REQUIRE(scripts.empty());
    }
    SECTION("not signed at all: refused, no script")
    {
        const auto bundle = makeDriverBundle(temp.root.getChildFile("unsigned/fixomic.driver"), false);
        REQUIRE(script::verifySignature(bundle).startsWith(refused));
        REQUIRE(script::installFrom(bundle, destination, runner).startsWith(refused));
        REQUIRE(scripts.empty());
    }
    SECTION("missing or not the fixomic driver: refused before the signature is even checked")
    {
        REQUIRE(script::installFrom(temp.root.getChildFile("nothing/fixomic.driver"), destination, runner)
                    .startsWith(juce::String::fromUTF8("Il driver non è incluso nell'app")));
        const auto other = temp.root.getChildFile("other/fixomic.driver");
        REQUIRE(other.getChildFile("Contents").createDirectory());
        REQUIRE(other.getChildFile("Contents/Info.plist").replaceWithText("not a plist"));
        REQUIRE(script::installFrom(other, destination, runner)
                    .startsWith(juce::String::fromUTF8("Il driver incluso nell'app non è valido")));
        REQUIRE(scripts.empty());
    }
}

TEST_CASE("driver_script::interpret never counts a missing script or result as a success", "[driver][installer]")
{
    REQUIRE(script::interpret(true, true, false, 0, {}).ok);

    const auto noScript = script::interpret(false, false, false, 0, {});
    REQUIRE_FALSE(noScript.ok);
    REQUIRE_FALSE(noScript.cancelled);
    REQUIRE(noScript.message.isNotEmpty());
    // run() serves install and uninstall alike: the message names neither.
    REQUIRE(noScript.message == juce::String::fromUTF8("lo script per il driver non \u00e8 stato creato"));

    const auto noResult = script::interpret(true, false, false, 0, {});
    REQUIRE_FALSE(noResult.ok);
    REQUIRE_FALSE(noResult.cancelled);
    REQUIRE(noResult.message.isNotEmpty());
    REQUIRE(noResult.message != noScript.message);

    const auto cancelled = script::interpret(true, false, true, -128, "User canceled.");
    REQUIRE_FALSE(cancelled.ok);
    REQUIRE(cancelled.cancelled);
    REQUIRE(cancelled.message == "User canceled.");

    REQUIRE(script::interpret(true, false, true, 7, {}).message == "errore AppleScript 7");
    REQUIRE_FALSE(script::interpret(true, true, true, 7, "x").ok);   // error information wins
}
