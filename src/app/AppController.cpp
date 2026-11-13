#include "app/AppController.h"

#include "platform/mac/AggregateIO.h"
#include "platform/mac/DriverInstaller.h"
#include "platform/mac/LoginItem.h"
#include "ui/SetupModel.h"

#include <stdexcept>

namespace fixomic {

namespace {

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

Config loadConfig(const ConfigStore& store, bool& onboardingFlagStored) {
    auto loaded = store.loadParsed();
    onboardingFlagStored = loaded.onboardingFlagStored;
    return std::move(loaded.config);
}

AudioObjectPropertyAddress systemProperty(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

} // namespace

AppController::AppController() : AppController(Options{}) {}

AppController::AppController(Options options)
    : store_(options.configFile),
      config_(loadConfig(store_, onboardingFlagStored_)),
      catalog_(options.pluginCacheFile, options.catalog),
      graph_(aec_, chain_),
      backend_(graph_, aec_, chain_, [this] { return config_; }),
      engine_(backend_, [] { return nowMs() / 1000.0; }),
      demand_([this](bool inUse) { post([inUse](AppController& c) { c.engine_.onDemandChanged(inUse); }); }),
      wake_([this] { post([](AppController& c) { c.engine_.onSystemWake(); }); }),
      devicesWatcher_(kAudioObjectSystemObject, systemProperty(kAudioHardwarePropertyDevices),
                      [this] {
                          post([](AppController& c) {
                              c.engine_.onMicListChanged();
                              c.rememberInputName();
                              c.notify();   // the list of selectable microphones changed
                          });
                      }),
      defaultOutputWatcher_(kAudioObjectSystemObject, systemProperty(kAudioHardwarePropertyDefaultOutputDevice),
                            [this] { post([](AppController& c) { c.engine_.onDefaultOutputChanged(); }); }),
      serviceWatcher_(kAudioObjectSystemObject, systemProperty(kAudioHardwarePropertyServiceRestarted),
                      [this] { post([](AppController& c) { c.handleServiceRestart(); }); }) {
    // Prepare before restoring, so the factory and the plugins see the real rate and block size.
    // The AEC is prepared too, for the latency estimate while the IO is closed (the backend
    // prepares both again on its first open).
    chain_.prepare(kSampleRate, config_.bufferSize);
    aec_.prepare(kSampleRate, config_.bufferSize);
    restoreChain();

    chain_.onChanged = [this] { post([](AppController& c) { c.changed(); }); };
    chain_.onPluginStateChanged = [this] { post([](AppController& c) { c.scheduleSave(); }); };
    windows_.onClosed = [this](juce::AudioPluginInstance&) { scheduleSave(); };
    // The backend holds the current warning (each open attempt starts without one): notify only.
    backend_.onWarning = [this](mac::WarningKind, juce::String) { post([](AppController& c) { c.notify(); }); };
    backend_.onWarningCleared = [this] { post([](AppController& c) { c.notify(); }); };
    backend_.onDeviceLost = [this] { post([](AppController& c) { c.engine_.onDeviceLost(); }); };
    // Answer to the microphone prompt raised by openIO(): re-evaluate (opens on demand if granted,
    // reports the denial otherwise).
    backend_.onMicrophonePermission = [this](bool) {
        post([](AppController& c) {
            c.engine_.onMicListChanged();
            c.notify();
        });
    };
    // The system audio prompt raised by openIO() was answered: retry at once.
    backend_.onSystemAudioPermission = [this] {
        post([](AppController& c) {
            c.engine_.onMicListChanged();
            c.notify();
        });
    };
    engine_.onStateChanged = [this](EngineState) { post([](AppController& c) { c.notify(); }); };

    if (config_.launchAtLogin && !mac::launchAtLoginEnabled() && !mac::launchAtLoginRequiresApproval()) {
        juce::String error;
        if (!mac::setLaunchAtLogin(true, error)) startupWarnings_.push_back(error);
    }

    engine_.setConfig(config_);
    rememberInputName();
    // Stated soon, not only at quit: the next launch tells a first launch (the guide opens by itself)
    // from a guide closed early (only the panel's reminder row), see ui::launchGuide.
    if (!onboardingFlagStored_) scheduleSave();
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

juce::String AppController::interruptedRestoreWarning() {
    return juce::String::fromUTF8("L\u2019ultimo avvio si \u00e8 interrotto caricando i plugin: la catena \u00e8 "
                                  "stata caricata senza plugin. Togli quello che d\u00e0 problemi: al prossimo "
                                  "avvio fixomic prover\u00e0 di nuovo a caricarli.");
}

void AppController::restoreChain() {
    // A plugin that crashes while loading would kill every launch (silently, with launch at login).
    // The marker exists only while the plugins load: found at launch, the last one died there.
    const auto marker = store_.file().getSiblingFile(kRestoreMarkerName);
    if (marker.existsAsFile()) {
        // Every slot missing: shown as "Plugin non trovato", removable, its saved description and state
        // kept, so the next launch tries again.
        const PluginFactory none = [](const juce::PluginDescription&, double, int, juce::String& error) {
            error = "not loaded: the last launch was interrupted while loading the plugins";
            return std::unique_ptr<juce::AudioPluginInstance>();
        };
        chain_.restore(config_.chain, none);
        startupWarnings_ = {interruptedRestoreWarning()};
        if (!marker.deleteFile()) juce::Logger::writeToLog("cannot remove " + marker.getFullPathName());
        return;
    }
    if (config_.chain.empty()) return;   // nothing to load, nothing to guard
    // Written before the first plugin loads; if it cannot be written the chain loads unguarded.
    if (!marker.getParentDirectory().createDirectory() || !marker.create())
        juce::Logger::writeToLog("cannot write " + marker.getFullPathName() + ": restoring the chain unguarded");
    startupWarnings_ = chain_.restore(config_.chain, catalog_.factory());
    marker.deleteFile();
}

// ---- callbacks --------------------------------------------------------------------------------

void AppController::post(std::function<void(AppController&)> action) {
    juce::MessageManager::callAsync([weak = juce::WeakReference<AppController>(this), fn = std::move(action)] {
        if (auto* self = weak.get()) fn(*self);
    });
}

void AppController::rememberInputName() {
    // A microphone chosen before names were stored: shown by name once it is unplugged.
    if (const auto learned = ui::inputNameToRemember(inputDevices(), config_.inputDeviceUid, config_.inputDeviceName))
        updateConfig([name = *learned](Config& c) { c.inputDeviceName = name; });
}

void AppController::handleServiceRestart() {
    ++serviceRestarts_;
    // The watchers are left alone: the HAL client registers them again with the new coreaudiod by
    // itself (measured, docs/notes/aggregate.md); removing and adding them here raced with that and
    // once left this process with a device list that no longer changed.
    engine_.onServiceRestarted();   // the IO died with the old coreaudiod: closed, re-evaluated
    notify();                       // the device list is being rebuilt
}

void AppController::timerCallback() {
    engine_.tick();
    chain_.collectGarbage();
    // The open IO's latency counts the AEC only once it processes (from the first blocks after the
    // open, or never without a reference): a model change for the panel's latency row.
    if (const bool aecActive = backend_.ioOpen() && graph_.aecActive(); aecActive != aecActive_) {
        aecActive_ = aecActive;
        notify();
    }
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
PendingPermission AppController::pendingPermission() const { return backend_.pendingPermission(); }
juce::String AppController::lastWarning() const { return backend_.warning(); }

std::optional<mac::WarningKind> AppController::lastWarningKind() const { return backend_.warningKind(); }

std::optional<mac::PrivacyPane> AppController::lastWarningPane() const {
    const auto kind = backend_.warningKind();
    if (!kind) return std::nullopt;
    return ui::warningPane(*kind);
}

// ---- config -----------------------------------------------------------------------------------

const Config& AppController::config() const { return config_; }
bool AppController::onboardingFlagStored() const { return onboardingFlagStored_; }

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

juce::String AppController::pluginLoadFailedText(const juce::String& pluginName) {
    if (pluginName.isEmpty()) return "Impossibile caricare il plugin.";
    return juce::String::fromUTF8("Impossibile caricare \u00ab") + pluginName + juce::String::fromUTF8("\u00bb.");
}

juce::String AppController::addPlugin(const juce::PluginDescription& description) {
    // The user reads one Italian sentence; why (the plugin format's own message, often English and
    // technical) goes to the log.
    const auto failure = [&](const juce::String& why) {
        juce::Logger::writeToLog("cannot load plugin \"" + description.name + "\" (" + description.pluginFormatName + " "
                                 + description.fileOrIdentifier + "): " + why);
        return pluginLoadFailedText(description.name);
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

juce::String AppController::ioDiagnostics() const {
    juce::String text;
    if (const auto* io = backend_.io()) {
        const auto stats = io->stats();
        text << "io aggregate " << static_cast<int>(io->deviceId()) << ", "
             << juce::String(static_cast<juce::int64>(stats.callbacks)) << " callbacks, "
             << juce::String(static_cast<juce::int64>(stats.frames)) << " frames, tap "
             << (backend_.tapActive() ? "on" : "off");
    } else {
        text << "io closed";
    }
    return text << ", Core Audio restarts handled " << serviceRestarts_;
}

// ---- platform services ------------------------------------------------------------------------

mac::Permission AppController::microphonePermission() const { return mac::microphonePermission(); }
mac::Permission AppController::systemAudioPermission() const { return mac::systemAudioPermission(); }

void AppController::requestMicrophone(std::function<void(bool granted)> done) {
    // The backend answers at once when there is nothing to ask: deferred either way, so the caller
    // never hears back from inside its own call, and never after destruction.
    backend_.requestMicrophone([weak = juce::WeakReference<AppController>(this), done](bool granted) {
        juce::MessageManager::callAsync([weak, done, granted] {
            if (weak.get() != nullptr && done) done(granted);
        });
    });
    post([](AppController& c) { c.notify(); });   // the prompt is up: microphoneRequestPending()
}

void AppController::requestSystemAudio(std::function<void(bool ok)> done) {
    backend_.requestSystemAudio([weak = juce::WeakReference<AppController>(this), done](bool ok) {
        juce::MessageManager::callAsync([weak, done, ok] {
            if (weak.get() != nullptr && done) done(ok);
        });
    });
    post([](AppController& c) { c.notify(); });
}

bool AppController::microphoneRequestPending() const { return backend_.microphoneRequestPending(); }
bool AppController::systemAudioRequestPending() const { return backend_.systemAudioRequestPending(); }
bool AppController::systemAudioAnswered() const { return backend_.systemAudioAnswered(); }
void AppController::openPrivacySettings(mac::PrivacyPane pane) const { mac::openPrivacySettings(pane); }
bool AppController::driverInstalled() const { return mac::driverInstalled(); }
juce::String AppController::bundledDriverVersion() const { return mac::bundledDriverVersion(); }
juce::String AppController::installedDriverVersion() const { return mac::installedDriverVersion(); }

bool AppController::driverUpdateAvailable() const {
    return mac::isDriverUpdate(bundledDriverVersion(), installedDriverVersion());
}

bool AppController::driverOperationInProgress() const { return mac::driverOperationInProgress(); }

void AppController::installDriver(DriverCompletion done) {
    // mac:: completions already run on the main queue; the weak reference keeps them out after
    // destruction.
    mac::installDriverAsync([weak = juce::WeakReference<AppController>(this), done,
                             restarts = serviceRestarts_](bool ok, juce::String error) {
        if (auto* self = weak.get()) self->driverOperationDone(ok, error, restarts, done);
    });
}

void AppController::uninstallDriver(DriverCompletion done) {
    mac::uninstallDriverAsync([weak = juce::WeakReference<AppController>(this), done,
                               restarts = serviceRestarts_](bool ok, juce::String error) {
        if (auto* self = weak.get()) self->driverOperationDone(ok, error, restarts, done);
    });
}

void AppController::driverOperationDone(bool ok, const juce::String& error, int restartsBefore,
                                        const DriverCompletion& done) {
    // A successful operation restarted coreaudiod (an uninstall with nothing installed did not, and
    // then no IO is open). Its notification normally arrived seconds ago: the old daemon dies at
    // the end of the script, the completion waits for fixomic Mic to (dis)appear. If it has not
    // been handled, handle the restart here rather than trust the open IO.
    if (ok && serviceRestarts_ == restartsBefore)
        handleServiceRestart();
    else
        engine_.onMicListChanged();   // DriverMissing <-> Idle / Running
    if (done) done(ok, error);
    notify();
}

bool AppController::launchAtLogin() const { return mac::launchAtLoginEnabled(); }

bool AppController::setLaunchAtLogin(bool enabled, juce::String& error) {
    if (!mac::setLaunchAtLogin(enabled, error)) return false;
    if (config_.launchAtLogin != enabled)
        updateConfig([enabled](Config& c) { c.launchAtLogin = enabled; });
    else
        notify();
    return true;
}

} // namespace fixomic
