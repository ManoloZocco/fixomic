#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>

#include "plugins/PluginCatalog.h"

using fixomic::PluginCatalog;

// Scanning tests launch build/fixomic_test_scan_worker (tests/support/ScanWorkerMain.cpp), the
// production worker plus a test override that recognises marker file names:
//   *fixomic-crash*            -> std::abort() inside the worker
//   *fixomic-hang*             -> never returns (the worker watchdog must kill it)
//   *fixomic-fake-effect*      -> one effect description
//   *fixomic-fake-instrument*  -> one instrument description
// Every other file goes through the real VST3 format.

namespace {

juce::PluginDescription description(const juce::String& name, bool instrument, int inputs = 2) {
    juce::PluginDescription d;
    d.name = d.descriptiveName = name;
    d.pluginFormatName = "VST3";
    d.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/" + name + ".vst3";
    d.manufacturerName = "fixomic";
    d.uniqueId = d.deprecatedUid = name.hashCode();
    d.isInstrument = instrument;
    d.numInputChannels = inputs;
    d.numOutputChannels = 2;
    return d;
}

void writeCache(const juce::File& file, const std::vector<juce::PluginDescription>& types,
                const juce::StringArray& blacklist = {}) {
    juce::KnownPluginList list;
    for (const auto& d : types) list.addType(d);
    for (const auto& b : blacklist) list.addToBlacklist(b);
    REQUIRE(list.createXml()->writeTo(file));
}

std::vector<juce::String> names(const std::vector<juce::PluginDescription>& types) {
    std::vector<juce::String> out;
    for (const auto& d : types) out.push_back(d.name);
    return out;
}

struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("fixomic-catalog-test", "");
    TempDir() { REQUIRE(dir.createDirectory()); }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File plugin(const juce::String& name) const {
        auto f = dir.getChildFile("plugins").getChildFile(name + ".vst3");
        REQUIRE(f.createDirectory());
        return f;
    }
};

PluginCatalog::Options testOptions(const TempDir& tmp, int timeoutSeconds = 30) {
    PluginCatalog::Options o;
    o.workerExecutable = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                             .getSiblingFile("fixomic_test_scan_worker");
    REQUIRE(o.workerExecutable.existsAsFile());
    o.scanAudioUnits = false;
    o.vst3SearchPath = juce::FileSearchPath(tmp.dir.getChildFile("plugins").getFullPathName());
    o.scanTimeoutSeconds = timeoutSeconds;
    return o;
}

// Services the main run loop (JUCE's message queue on macOS) for about `ms` milliseconds.
void pumpMessages(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
}

// Runs the message loop until the scan's onDone fires (or ~60 s pass).
bool scanAndWait(PluginCatalog& catalog) {
    bool done = false;
    catalog.scanAsync([&] { done = true; });
    for (int i = 0; i < 600 && !done; ++i) pumpMessages(100);
    return done;
}

} // namespace

TEST_CASE("PluginCatalog effects() excludes instruments and sorts by name", "[catalog]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    writeCache(cache, {description("Zeta Delay", false), description("Big Synth", true),
                       description("alpha EQ", false), description("Tone Generator", false, 0)});

    PluginCatalog catalog(cache);
    REQUIRE(names(catalog.effects()) == std::vector<juce::String>{"alpha EQ", "Zeta Delay"});
}

TEST_CASE("PluginCatalog reads the KnownPluginList cache, blacklist included", "[catalog]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    writeCache(cache, {description("Gate", false)}, {"/bad/Crasher.vst3"});

    PluginCatalog catalog(cache);
    REQUIRE(names(catalog.effects()) == std::vector<juce::String>{"Gate"});
    REQUIRE(catalog.blacklisted() == juce::StringArray{"/bad/Crasher.vst3"});
}

TEST_CASE("PluginCatalog with a missing or corrupt cache starts empty", "[catalog]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    {
        PluginCatalog catalog(cache);
        REQUIRE(catalog.effects().empty());
    }
    REQUIRE(cache.replaceWithText("<not xml"));
    PluginCatalog catalog(cache);
    REQUIRE(catalog.effects().empty());
}

TEST_CASE("PluginCatalog scans out of process and writes the cache", "[catalog][scan]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    const auto effect = tmp.plugin("fixomic-fake-effect Comp");
    tmp.plugin("fixomic-fake-instrument Keys");

    {
        PluginCatalog catalog(cache, testOptions(tmp));
        REQUIRE(scanAndWait(catalog));
        REQUIRE_FALSE(catalog.isScanning());
        const auto effects = catalog.effects();
        REQUIRE(names(effects) == std::vector<juce::String>{"fixomic-fake-effect Comp"});
        REQUIRE(effects[0].fileOrIdentifier == effect.getFullPathName());
        REQUIRE(catalog.blacklisted().isEmpty());
    }

    // The cache is a KnownPluginList document that a fresh catalog reads back.
    REQUIRE(cache.existsAsFile());
    juce::KnownPluginList reread;
    reread.recreateFromXml(*juce::parseXML(cache));
    REQUIRE(reread.getNumTypes() == 2);
    PluginCatalog again(cache);
    REQUIRE(names(again.effects()) == std::vector<juce::String>{"fixomic-fake-effect Comp"});
}

TEST_CASE("PluginCatalog blacklists a plugin that crashes the worker and keeps scanning", "[catalog][scan]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    const auto crasher = tmp.plugin("a fixomic-crash");
    tmp.plugin("b fixomic-fake-effect After");
    tmp.plugin("0 fixomic-fake-effect Before");

    {
        PluginCatalog catalog(cache, testOptions(tmp));
        REQUIRE(scanAndWait(catalog));
        REQUIRE(catalog.blacklisted() == juce::StringArray{crasher.getFullPathName()});
        // Scanned in name order: one effect before the crash, one after (on a relaunched worker).
        REQUIRE(names(catalog.effects())
                == std::vector<juce::String>{"0 fixomic-fake-effect Before", "b fixomic-fake-effect After"});
    }

    // The blacklist is persisted, and a second scan skips the crasher (no new crash, same result).
    PluginCatalog catalog(cache, testOptions(tmp));
    REQUIRE(catalog.blacklisted() == juce::StringArray{crasher.getFullPathName()});
    REQUIRE(scanAndWait(catalog));
    REQUIRE(catalog.blacklisted() == juce::StringArray{crasher.getFullPathName()});
    REQUIRE(catalog.effects().size() == 2);
}

TEST_CASE("PluginCatalog blacklists a plugin that hangs the worker past the timeout", "[catalog][scan]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    const auto hanger = tmp.plugin("a fixomic-hang");
    tmp.plugin("b fixomic-fake-effect After");

    PluginCatalog catalog(cache, testOptions(tmp, 2));
    REQUIRE(scanAndWait(catalog));
    REQUIRE(catalog.blacklisted() == juce::StringArray{hanger.getFullPathName()});
    REQUIRE(names(catalog.effects()) == std::vector<juce::String>{"b fixomic-fake-effect After"});
}

TEST_CASE("PluginCatalog destroyed mid-scan stops without blacklisting", "[catalog][scan]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    const auto cache = tmp.dir.getChildFile("plugins.xml");
    tmp.plugin("a fixomic-hang");
    bool called = false;
    {
        PluginCatalog catalog(cache, testOptions(tmp, 30));
        catalog.scanAsync([&] { called = true; });
        pumpMessages(1500);   // worker is now stuck
        REQUIRE(catalog.isScanning());
    }   // must return promptly, not after the 30 s watchdog
    pumpMessages(200);
    REQUIRE_FALSE(called);
    PluginCatalog reread(cache);
    REQUIRE(reread.blacklisted().isEmpty());
}

TEST_CASE("PluginCatalog factory reports an error for a plugin that cannot be loaded", "[catalog]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TempDir tmp;
    PluginCatalog catalog(tmp.dir.getChildFile("plugins.xml"));
    juce::String error;
    auto instance = catalog.factory()(description("Nowhere", false), 48000.0, 128, error);
    REQUIRE(instance == nullptr);
    REQUIRE(error.isNotEmpty());
}
