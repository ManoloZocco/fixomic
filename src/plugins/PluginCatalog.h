#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/PluginChain.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace fixomic {

// The plugins the user can put in the chain: a juce::KnownPluginList of AU and VST3 effects,
// cached as KnownPluginList XML (blacklist included) and refreshed by an out-of-process scan.
//
// Scanning follows JUCE's AudioPluginHost: a KnownPluginList::CustomScanner sends each plugin
// (format name + file / identifier) to a worker process, the app binary relaunched with
// `--scan-worker:<pipe>` (juce::ChildProcessCoordinator / PluginScanWorker below), which loads
// it and answers with its descriptions. A plugin that kills the worker (crash, or a hang past
// scanTimeoutSeconds, after which the worker's watchdog exits) is blacklisted by the
// KnownPluginList and never scanned again; the next plugin gets a fresh worker. The scan is
// incremental: up-to-date entries and blacklisted files are skipped, entries whose plugin was
// removed from disk are dropped. Only AU effects ("aufx", "aumf") are sent to the worker;
// VST3 instruments are scanned (a VST3 bundle can hold both) and filtered by effects().
//
// Threading: message thread only, except that the scan itself runs on a background thread.
class PluginCatalog {
public:
    struct Options {
        juce::File workerExecutable;              // default: this executable
        bool scanAudioUnits = true;
        juce::FileSearchPath vst3SearchPath;      // empty: the VST3 format's default locations
        int scanTimeoutSeconds = 120;             // per plugin, enforced by the worker
    };

    explicit PluginCatalog(juce::File cacheFile);   // ~/Library/Application Support/fixomic/plugins.xml
    PluginCatalog(juce::File cacheFile, Options);
    ~PluginCatalog();                               // stops a running scan (no blacklisting) and its worker
    PluginCatalog(const PluginCatalog&) = delete;
    PluginCatalog& operator=(const PluginCatalog&) = delete;

    static juce::File defaultCacheLocation();

    // Starts a background scan; onDone runs on the message thread when it finishes (never after
    // the catalog is destroyed). While a scan runs, further calls just queue their onDone.
    void scanAsync(std::function<void()> onDone);
    bool isScanning() const;

    // Every known type that is neither an instrument nor without audio input, sorted by name
    // (case-insensitive), then format.
    std::vector<juce::PluginDescription> effects() const;
    juce::StringArray blacklisted() const;

    // Creates instances synchronously on the message thread with this catalog's formats (the
    // catalog must outlive the factory). sampleRate / blockSize <= 0 fall back to 48 kHz / 128.
    PluginFactory factory();

    // Command-line id of the worker: the worker is launched with `--scan-worker:<pipe name>`.
    static constexpr const char* kWorkerId = "scan-worker";

private:
    class Scanner;
    void runScan();                 // scan thread
    void saveCache() const;         // scan thread
    void scanFinished();            // message thread
    juce::File workerExecutable() const;

    juce::File cacheFile_;
    Options options_;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList list_;    // internally locked: read here, filled by the scan thread
    Scanner* scanner_ = nullptr;    // owned by list_
    std::thread thread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> stop_{false};
    std::vector<std::function<void()>> onDone_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// The worker side of the scan, created in the worker process from its command line. It loads
// each requested plugin with the same formats (on the message thread when the format requires
// it) and replies with the descriptions found. Fatal signals in a plugin end the worker quietly
// (no crash report dialog), a scan longer than the coordinator's timeout ends it from a watchdog
// thread, and a lost connection quits the application.
class PluginScanWorker {
public:
    // Test hook: called before the real format scan; returns true if it handled the request
    // (results filled), false to scan normally.
    using Override = std::function<bool(const juce::String& formatName, const juce::String& identifier,
                                        juce::OwnedArray<juce::PluginDescription>& results)>;

    // nullptr if `commandLine` is not a worker invocation (or the coordinator is gone).
    static std::unique_ptr<PluginScanWorker> connect(const juce::String& commandLine, Override = {});
    static bool isWorkerCommandLine(const juce::String& commandLine);

    ~PluginScanWorker();
    PluginScanWorker(const PluginScanWorker&) = delete;
    PluginScanWorker& operator=(const PluginScanWorker&) = delete;

private:
    struct Impl;
    explicit PluginScanWorker(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic
