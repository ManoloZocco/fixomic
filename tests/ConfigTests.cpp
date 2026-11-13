#include <catch2/catch_test_macros.hpp>

#include "core/Config.h"
#include "core/ConfigStore.h"

using namespace fixomic;

namespace {

Config makeSampleConfig() {
    Config c;
    c.inputDeviceUid = "X";
    c.inputDeviceName = "Mic \"X\" & co";
    c.inputChannel = 1;
    c.bufferSize = 64;
    c.aecEnabled = false;
    c.globalBypass = true;
    c.launchAtLogin = true;
    c.onboardingDone = true;

    PluginSlot a;
    a.descriptionXml = "<PLUGIN name=\"A &amp; B\" format=\"VST3\"/>";
    a.state = juce::MemoryBlock(std::array<std::uint8_t, 3>{1, 2, 3}.data(), 3);
    a.bypassed = true;

    PluginSlot b;
    b.descriptionXml = "<PLUGIN name=\"B\" format=\"AudioUnit\"/>";
    b.bypassed = false;

    c.chain = {a, b};
    return c;
}

void requireEqual(const Config& x, const Config& y) {
    REQUIRE(x.version == y.version);
    REQUIRE(x.inputDeviceUid == y.inputDeviceUid);
    REQUIRE(x.inputDeviceName == y.inputDeviceName);
    REQUIRE(x.inputChannel == y.inputChannel);
    REQUIRE(x.bufferSize == y.bufferSize);
    REQUIRE(x.aecEnabled == y.aecEnabled);
    REQUIRE(x.globalBypass == y.globalBypass);
    REQUIRE(x.launchAtLogin == y.launchAtLogin);
    REQUIRE(x.onboardingDone == y.onboardingDone);
    REQUIRE(x.chain.size() == y.chain.size());
    for (size_t i = 0; i < x.chain.size(); ++i) {
        REQUIRE(x.chain[i].descriptionXml == y.chain[i].descriptionXml);
        REQUIRE(x.chain[i].state == y.chain[i].state);
        REQUIRE(x.chain[i].bypassed == y.chain[i].bypassed);
    }
}

juce::File makeTempDir() {
    auto dir = juce::File::createTempFile("fixomic-test");
    REQUIRE(dir.createDirectory().wasOk());
    return dir;
}

} // namespace

TEST_CASE("round-trip preserves all fields") {
    const auto original = makeSampleConfig();
    const auto parsed = fromXmlString(toXmlString(original));
    REQUIRE(parsed.has_value());
    requireEqual(original, *parsed);
    REQUIRE(parsed->chain[0].state.getSize() == 3);
}

TEST_CASE("onboardingDone is stored, and false in a new config") {
    REQUIRE_FALSE(Config{}.onboardingDone);
    Config c;
    c.inputDeviceUid = "mic";
    c.onboardingDone = false;   // set up by hand, guide closed early: stays false
    REQUIRE_FALSE(fromXmlString(toXmlString(c))->onboardingDone);
    c.onboardingDone = true;
    REQUIRE(fromXmlString(toXmlString(c))->onboardingDone);
    REQUIRE(toXmlString(c).contains("onboardingDone=\"1\""));
}

TEST_CASE("a config written before the onboarding flag counts as onboarded once it was set up") {
    // No flag: a chosen microphone or a chain means the user already set fixomic up by hand.
    REQUIRE(fromXmlString("<FixomicConfig version=\"1\" inputDeviceUid=\"BuiltInMicrophoneDevice\"/>")->onboardingDone);
    REQUIRE(fromXmlString("<FixomicConfig version=\"1\" inputDeviceUid=\"\"><Plugin description=\"x\"/></FixomicConfig>")
                ->onboardingDone);
    REQUIRE_FALSE(fromXmlString("<FixomicConfig version=\"1\" inputDeviceUid=\"\"/>")->onboardingDone);
    REQUIRE_FALSE(fromXmlString("<FixomicConfig version=\"1\"/>")->onboardingDone);
    // An explicit flag always wins.
    REQUIRE_FALSE(fromXmlString("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\" onboardingDone=\"0\"/>")
                      ->onboardingDone);
    REQUIRE(fromXmlString("<FixomicConfig version=\"1\" onboardingDone=\"1\"/>")->onboardingDone);
    // And no stored device name: "".
    REQUIRE(fromXmlString("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\"/>")->inputDeviceName.isEmpty());
}

TEST_CASE("parseConfig says whether the document stated onboardingDone") {
    REQUIRE_FALSE(parseConfig("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\"/>")->onboardingFlagStored);
    REQUIRE(parseConfig("<FixomicConfig version=\"1\" onboardingDone=\"0\"/>")->onboardingFlagStored);
    REQUIRE(parseConfig("<FixomicConfig version=\"1\" onboardingDone=\"1\"/>")->onboardingFlagStored);
    REQUIRE_FALSE(parseConfig("<other/>").has_value());
    // The config itself is fromXmlString's.
    REQUIRE(parseConfig("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\"/>")->config.onboardingDone);
}

TEST_CASE("ConfigStore::loadParsed reports the flag as not stored for a missing or corrupt file") {
    auto dir = makeTempDir();
    const auto file = dir.getChildFile("config.xml");
    ConfigStore store(file);
    REQUIRE_FALSE(store.loadParsed().onboardingFlagStored);   // missing
    REQUIRE(file.replaceWithText("garbage"));
    REQUIRE_FALSE(store.loadParsed().onboardingFlagStored);   // corrupt
    REQUIRE(file.replaceWithText("<FixomicConfig version=\"1\" inputDeviceUid=\"mic\"/>"));
    REQUIRE_FALSE(store.loadParsed().onboardingFlagStored);   // written before the flag
    Config c;
    REQUIRE(store.save(c));   // every save states it
    REQUIRE(store.loadParsed().onboardingFlagStored);
    REQUIRE_FALSE(store.loadParsed().config.onboardingDone);
    dir.deleteRecursively();
}

TEST_CASE("malformed xml returns nullopt") {
    REQUIRE_FALSE(fromXmlString("<nope").has_value());
}

TEST_CASE("well-formed xml with the wrong root returns nullopt") {
    REQUIRE_FALSE(fromXmlString("<other/>").has_value());
}

TEST_CASE("out-of-range values fall back to defaults") {
    const auto parsed = fromXmlString("<FixomicConfig version=\"1\" bufferSize=\"77\" inputChannel=\"-3\"/>");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->bufferSize == Config{}.bufferSize);
    REQUIRE(parsed->inputChannel == Config{}.inputChannel);
}

TEST_CASE("load of missing file returns defaults") {
    auto dir = makeTempDir();
    ConfigStore store(dir.getChildFile("config.xml"));
    requireEqual(store.load(), Config{});
    dir.deleteRecursively();
}

TEST_CASE("load of corrupt file returns defaults and keeps a .bak") {
    auto dir = makeTempDir();
    auto file = dir.getChildFile("config.xml");
    REQUIRE(file.replaceWithText("garbage"));

    ConfigStore store(file);
    requireEqual(store.load(), Config{});

    auto bak = dir.getChildFile("config.xml.bak");
    REQUIRE(bak.existsAsFile());
    REQUIRE(bak.loadFileAsString() == "garbage");
    REQUIRE_FALSE(file.exists());
    dir.deleteRecursively();
}

TEST_CASE("save then load round-trips") {
    auto dir = makeTempDir();
    // Parent directory of the target does not exist yet: save() must create it.
    auto file = dir.getChildFile("sub").getChildFile("config.xml");
    ConfigStore store(file);

    const auto original = makeSampleConfig();
    REQUIRE(store.save(original));
    requireEqual(store.load(), original);

    // Overwriting works and leaves no temporary files behind.
    auto changed = original;
    changed.bufferSize = 256;
    REQUIRE(store.save(changed));
    requireEqual(store.load(), changed);
    REQUIRE(file.getParentDirectory().getNumberOfChildFiles(juce::File::findFiles) == 1);
    dir.deleteRecursively();
}

TEST_CASE("default location is ~/Library/Application Support/fixomic/config.xml") {
    const auto expected = juce::File("~/Library/Application Support/fixomic/config.xml");
    REQUIRE(fixomic::ConfigStore::defaultLocation() == expected);
}

TEST_CASE("saveIfChanged skips identical content and reports failures") {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("fixomic-config-test", "");
    REQUIRE(dir.createDirectory());
    const auto file = dir.getChildFile("config.xml");
    fixomic::ConfigStore store(file);
    fixomic::Config config;
    config.inputDeviceUid = "mic-a";

    REQUIRE(store.saveIfChanged(config) == fixomic::ConfigStore::SaveResult::Written);
    REQUIRE(file.setLastModificationTime(juce::Time(2000, 0, 1, 0, 0)));
    REQUIRE(store.saveIfChanged(config) == fixomic::ConfigStore::SaveResult::Unchanged);
    REQUIRE(file.getLastModificationTime().getYear() == 2000);   // not rewritten

    config.inputDeviceUid = "mic-b";
    REQUIRE(store.saveIfChanged(config) == fixomic::ConfigStore::SaveResult::Written);
    REQUIRE(store.load().inputDeviceUid == "mic-b");

    // The parent "directory" is a plain file: the write cannot succeed.
    const auto blocker = dir.getChildFile("blocker");
    REQUIRE(blocker.replaceWithText("x"));
    fixomic::ConfigStore broken(blocker.getChildFile("config.xml"));
    REQUIRE(broken.saveIfChanged(config) == fixomic::ConfigStore::SaveResult::Failed);

    dir.deleteRecursively();
}
