#include <catch2/catch_test_macros.hpp>

#include "platform/mac/DriverInstaller.h"
#include "platform/mac/LoginItem.h"
#include "platform/mac/Permissions.h"
#include "platform/mac/TapSilenceWatch.h"

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
    REQUIRE(install.contains("/usr/bin/ditto \" & quoted form of src & \" \" & quoted form of dst"));
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

TEST_CASE("install and uninstall scripts work with spaces, quotes and shell characters", "[driver][installer]")
{
    // The same scripts, without administrator privileges and without the coreaudiod restart,
    // on temporary paths full of characters a naive quoting would break on.
    const auto root = juce::File::createTempFile("").getSiblingFile("fixomic installer test 'q' \"dq\" $HOME `x` \\b");
    root.deleteRecursively();
    REQUIRE(root.createDirectory());
    const auto source = root.getChildFile("src dir 'a'/fixomic.driver");
    REQUIRE(source.getChildFile("Contents").createDirectory());
    REQUIRE(source.getChildFile("Contents/Info.plist").replaceWithText("plist"));
    const auto destination = root.getChildFile("HAL \"dest\" $(echo no)/fixomic.driver");
    REQUIRE(destination.getChildFile("Contents/old.txt").create());   // replaced, not merged

    auto result = script::run(script::install(source.getFullPathName(), destination.getFullPathName(), {false, false}));
    INFO(result.message);
    REQUIRE(result.ok);
    REQUIRE(destination.getChildFile("Contents/Info.plist").loadFileAsString() == "plist");
    REQUIRE_FALSE(destination.getChildFile("Contents/old.txt").exists());
    REQUIRE(source.getChildFile("Contents/Info.plist").exists());

    result = script::run(script::uninstall(destination.getFullPathName(), {false, false}));
    REQUIRE(result.ok);
    REQUIRE_FALSE(destination.exists());
    REQUIRE(destination.getParentDirectory().isDirectory());   // only the driver bundle goes
    REQUIRE(source.isDirectory());

    REQUIRE(root.deleteRecursively());
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
