#include "app/AppController.h"

#include <stdexcept>

namespace fixomic {

namespace {

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

AudioObjectPropertyAddress systemProperty(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

} // namespace

AppController::AppController() : AppController(Options{}) {}

AppController::AppController(Options options)
    : store_(options.configFile),
      config_(store_.load()),
      catalog_(options.pluginCacheFile),
      graph_(aec_, chain_),
      backend_(graph_, aec_, chain_, [this] { return config_; }),
      engine_(backend_, [] { return nowMs() / 1000.0; }),
      demand_([this](bool inUse) { post([inUse](AppController& c) { c.engine_.onDemandChanged(inUse); }); }),
      wake_([this] { post([](AppController& c) { c.engine_.onSystemWake(); }); }),
      devicesWatcher_(kAudioObjectSystemObject, systemProperty(kAudioHardwarePropertyDevices),
                      [this] {
                          post([](AppController& c) {
                              c.engine_.onMicListChanged();
                              c.notify();   // the list of selectable microphones changed
                          });
                      }),
      defaultOutputWatcher_(kAudioObjectSystemObject, systemProperty(kAudioHardwarePropertyDefaultOutputDevice),
                            [this] { post([](AppController& c) { c.engine_.onDefaultOutputChanged(); }); }) {
    // Prepare before restoring, so the factory and the plugins see the real rate and block size.
    // The AEC is prepared too, for the latency estimate while the IO is closed (the backend
    // prepares both again on its first open).
    chain_.prepare(kSampleRate, config_.bufferSize);
    aec_.prepare(kSampleRate, config_.bufferSize);
    startupWarnings_ = chain_.restore(config_.chain, catalog_.factory());

    chain_.onChanged = [this] { post([](AppController& c) { c.changed(); }); };
    chain_.onPluginStateChanged = [this] { post([](AppController& c) { c.scheduleSave(); }); };
    windows_.onClosed = [this](juce::AudioPluginInstance&) { scheduleSave(); };
    backend_.onWarning = [this](juce::String warning) {
        post([warning](AppController& c) {
            c.lastWarning_ = warning;
            c.notify();
        });
    };
    backend_.onDeviceLost = [this] { post([](AppController& c) { c.engine_.onDeviceLost(); }); };
    // Answer to the microphone prompt raised by openIO(): re-evaluate (opens on demand if granted,
    // reports the denial otherwise).
    backend_.onMicrophonePermission = [this](bool) {
        post([](AppController& c) {
            c.engine_.onMicListChanged();
            c.notify();
        });
    };
    engine_.onStateChanged = [this](EngineState state) {
        post([state](AppController& c) {
            if (state == EngineState::Starting) c.lastWarning_.clear();   // a fresh open reports anew
            c.notify();
        });
    };

    engine_.setConfig(config_);
    demand_.start();
    startTimer(kTimerMs);
    scanPlugins();
}

AppController::~AppController() {
    stopTimer();
    demand_.stop();
    saveNow();             // while every plugin is alive
    windows_.closeAll();   // editors before plugins
    // Members then go in reverse order: watchers, wake, demand, engine and backend (IO closed),
    // graph, windows, chain (plugins destroyed), AEC, catalog (scan stopped), config.
    engine_.onStateChanged = nullptr;
    chain_.onChanged = nullptr;
    chain_.onPluginStateChanged = nullptr;
}

// ---- callbacks --------------------------------------------------------------------------------

void AppController::post(std::function<void(AppController&)> action) {
    juce::MessageManager::callAsync([weak = juce::WeakReference<AppController>(this), fn = std::move(action)] {
        if (auto* self = weak.get()) fn(*self);
    });
}

void AppController::timerCallback() {
    engine_.tick();
    chain_.collectGarbage();
    if (firstUnsavedChange_ >= 0.0) {
        const double now = nowMs();
        if (now - lastChange_ >= kSaveDebounceMs || now - firstUnsavedChange_ >= kSaveMaxDelayMs) saveNow();
    }
}

void AppController::scheduleSave() {
    lastChange_ = nowMs();
    if (firstUnsavedChange_ < 0.0) firstUnsavedChange_ = lastChange_;
}

void AppController::changed() {
    scheduleSave();
    notify();
}

void AppController::notify() {
    if (onModelChanged) onModelChanged();
}

// ---- engine -----------------------------------------------------------------------------------

EngineState AppController::engineState() const { return engine_.state(); }
juce::String AppController::lastError() const { return engine_.lastError(); }
juce::String AppController::lastWarning() const { return lastWarning_; }

// ---- config -----------------------------------------------------------------------------------

const Config& AppController::config() const { return config_; }

void AppController::updateConfig(std::function<void(Config&)> edit) {
    Config next = config_;
    edit(next);
    next.chain = config_.chain;   // owned by the controller
    config_ = std::move(next);
    engine_.setConfig(config_);
    changed();
}

bool AppController::saveNow() {
    // VST3 edits reach the plugin only inside a process call. With the IO closed nothing processes:
    // flush here. With it open, the audio thread flushes the slots it skips (bypassed slots, every
    // slot under global bypass); the wait is bounded so a stalled IO cannot hang the UI.
    if (backend_.ioOpen())
        chain_.flushPluginParametersOnAudioThread(kFlushWaitMs);
    else
        chain_.flushPluginParameters();
    config_.chain = chain_.saveState();
    const bool hadError = lastSaveError_.isNotEmpty();
    if (store_.saveIfChanged(config_) == ConfigStore::SaveResult::Failed) {
        // Keep the change pending: retried kSaveDebounceMs later by the timer.
        lastSaveError_ = "Impossibile salvare la configurazione in " + store_.file().getFullPathName();
        firstUnsavedChange_ = lastChange_ = nowMs();
        if (!hadError) notify();
        return false;
    }
    lastSaveError_.clear();
    firstUnsavedChange_ = -1.0;
    if (hadError) notify();
    return true;
}

juce::String AppController::lastSaveError() const { return lastSaveError_; }

// ---- plugin chain -----------------------------------------------------------------------------

juce::String AppController::addPlugin(const juce::PluginDescription& description) {
    const auto failure = [&](const juce::String& why) {
        return "Impossibile caricare il plugin \"" + description.name + "\": " + why;
    };
    juce::String error;
    auto instance = catalog_.factory()(description, kSampleRate, config_.bufferSize, error);
    if (instance == nullptr) return failure(error);
    try {
        chain_.add(std::move(instance));
    } catch (const std::exception& e) {
        return failure(e.what());
    }
    return {};
}

void AppController::removePlugin(int index) {
    if (auto* plugin = chain_.pluginAt(index)) windows_.close(*plugin);
    chain_.remove(index);
}

void AppController::movePlugin(int from, int to) { chain_.move(from, to); }

void AppController::setPluginBypassed(int index, bool bypassed) { chain_.setBypassed(index, bypassed); }

bool AppController::openEditor(int index) {
    auto* plugin = chain_.pluginAt(index);
    if (plugin == nullptr) return false;
    windows_.open(*plugin);
    return true;
}

juce::AudioPluginInstance* AppController::plugin(int index) const { return chain_.pluginAt(index); }

std::vector<PluginChain::SlotInfo> AppController::slots() const { return chain_.slots(); }
std::vector<juce::String> AppController::startupWarnings() const { return startupWarnings_; }

// ---- catalog ----------------------------------------------------------------------------------

std::vector<juce::PluginDescription> AppController::effects() const { return catalog_.effects(); }

void AppController::scanPlugins() {
    catalog_.scanAsync([this] { post([](AppController& c) { c.notify(); }); });
}

bool AppController::isScanning() const { return catalog_.isScanning(); }
PluginCatalog& AppController::catalog() { return catalog_; }

// ---- meters / diagnostics ---------------------------------------------------------------------

float AppController::inputPeakDb() const { return graph_.inputMeter.peakDb(); }
float AppController::outputPeakDb() const { return graph_.outputMeter.peakDb(); }
float AppController::erleDb() const { return aec_.erleDb(); }

LatencyBreakdown AppController::latency() const {
    if (auto open = backend_.latency()) return *open;
    // Same conventions as the backend: no AEC or plugin frames while globally bypassed.
    const bool processing = !config_.globalBypass;
    return {kSampleRate,
            0,
            0,
            2 * config_.bufferSize,
            processing && config_.aecEnabled ? aec_.latencySamples() : 0,
            processing ? chain_.totalLatencySamples() : 0};
}

std::vector<mac::AudioDeviceInfo> AppController::inputDevices() const { return mac::listInputDevices(); }

} // namespace fixomic
