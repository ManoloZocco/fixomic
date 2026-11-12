// fixomic_test_scan_worker: the production plugin scan worker (PluginScanWorker) with a test
// override that recognises marker file names, so PluginCatalogTests can simulate plugins that
// crash or hang the worker without real crashing plugins. See tests/PluginCatalogTests.cpp.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <thread>

#include "plugins/PluginCatalog.h"

namespace {

bool testOverride(const juce::String& formatName, const juce::String& identifier,
                  juce::OwnedArray<juce::PluginDescription>& results) {
    const juce::File file(identifier);
    const auto name = file.getFileNameWithoutExtension();
    if (name.contains("fixomic-crash")) std::abort();
    if (name.contains("fixomic-hang"))
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));

    const bool effect = name.contains("fixomic-fake-effect");
    if (!effect && !name.contains("fixomic-fake-instrument")) return false;

    auto d = std::make_unique<juce::PluginDescription>();
    d->name = d->descriptiveName = name;
    d->pluginFormatName = formatName;
    d->fileOrIdentifier = identifier;
    d->lastFileModTime = file.getLastModificationTime();
    d->manufacturerName = "fixomic";
    d->uniqueId = d->deprecatedUid = name.hashCode();
    d->isInstrument = !effect;
    d->numInputChannels = effect ? 2 : 0;
    d->numOutputChannels = 2;
    results.add(std::move(d));
    return true;
}

class ScanWorkerApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "fixomic_test_scan_worker"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        worker_ = fixomic::PluginScanWorker::connect(commandLine, testOverride);
        if (worker_ == nullptr) quit();
    }
    void shutdown() override { worker_.reset(); }

private:
    std::unique_ptr<fixomic::PluginScanWorker> worker_;
};

} // namespace

START_JUCE_APPLICATION(ScanWorkerApp)
