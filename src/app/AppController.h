#pragma once

#include "core/Config.h"
#include "core/ConfigStore.h"
#include "core/LatencyBudget.h"
#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "engine/EngineController.h"
#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/DemandMonitor.h"
#include "platform/mac/MacEngineBackend.h"
#include "platform/mac/Permissions.h"
#include "platform/mac/WakeObserver.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginWindows.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace fixomic {

// The running app: owns the config, the plugin catalog and chain, the DSP graph and the on-demand
// engine, and wires the platform events (demand on fixomic Mic, device list, default output,
// wake, device lost, Core Audio restarted) into the engine.
//
// Core Audio restart (kAudioHardwarePropertyServiceRestarted: coreaudiod restarted by a driver
// install / uninstall, or by anything else): the engine closes the dead IO and re-evaluates
// (EngineController::onServiceRestarted). The HAL listeners survive the restart (the HAL client
// registers them with the new daemon itself, measured on macOS 27.2): they are not re-registered.
// A driver operation that succeeded restarted coreaudiod: if its notification has not been handled
// by the time the operation completes, the completion handles the restart itself, so the engine
// never stays Running on the IO of the old daemon.
//
// Startup: config loaded, chain prepared for 48 kHz / config.bufferSize and then restored (plugins
// that fail to load stay as missing slots, see startupWarnings()), engine configured, demand
// monitoring started, an incremental plugin scan started in the background. A 200 ms timer drives
// EngineController::tick(), PluginChain::collectGarbage() and the debounced config save.
//
// Config: config() is the single source of truth, also for the engine backend. It is saved 1 s
// after the last change (settings, chain structure, bypass, plugin state, an editor closed; at most
// 10 s after the first of a stream of changes) and on destruction. A save is skipped when the file
// already holds the same XML; a failed write stays pending, is retried 1 s later and is reported by
// lastSaveError(). Plugin state is captured with
// PluginChain::saveState() when saving. The chain field of config() is owned by the controller:
// it reflects the chain as last saved, and edits to it through updateConfig() are ignored.
//
// Never picks a microphone by itself: with no input selected the engine stays MicMissing.
//
// Launch at login: Config.launchAtLogin is the user's choice, the login item (SMAppService) its
// effect. setLaunchAtLogin() applies and then persists; at startup a choice of "on" whose item is
// missing (e.g. the app was moved) is registered again; an item the user turned off in System
// Settings is left alone (launchAtLogin() reports the system state).
//
// Threading: message thread only. Every platform / engine / chain callback is re-posted with
// juce::MessageManager::callAsync behind a weak reference, so none runs after destruction and none
// re-enters the engine from inside its own callbacks.
class AppController : private juce::Timer {
public:
    struct Options {
        juce::File configFile = ConfigStore::defaultLocation();
        juce::File pluginCacheFile = PluginCatalog::defaultCacheLocation();
    };

    AppController();
    explicit AppController(Options);
    ~AppController() override;   // saves, closes editors, closes the IO, then destroys the plugins
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // ---- engine ------------------------------------------------------------------------------
    EngineState engineState() const;
    juce::String lastError() const;      // why the engine is in Error (or the last open failed)
    juce::String lastWarning() const;    // last backend warning (e.g. no system audio tap), "" if none

    // ---- config ------------------------------------------------------------------------------
    const Config& config() const;
    // Edits a copy, applies it to the engine (EngineController::setConfig) and schedules a save.
    void updateConfig(std::function<void(Config&)>);
    bool saveNow();                      // false if the write failed (retried by the timer)
    juce::String lastSaveError() const;  // Italian, "" after a successful save

    // ---- plugin chain ------------------------------------------------------------------------
    // Instantiates and appends a plugin. Returns "" on success, else a readable (Italian) error.
    juce::String addPlugin(const juce::PluginDescription&);
    void removePlugin(int index);              // closes its editor first
    void movePlugin(int from, int to);
    void setPluginBypassed(int index, bool);
    bool openEditor(int index);                // false for missing slots / out of range
    // The live instance (nullptr for missing slots / out of range). Do not keep the pointer: the
    // plugin may be destroyed after the next chain edit.
    juce::AudioPluginInstance* plugin(int index) const;
    std::vector<PluginChain::SlotInfo> slots() const;
    std::vector<juce::String> startupWarnings() const;   // chain slots that could not be restored

    // ---- catalog -----------------------------------------------------------------------------
    std::vector<juce::PluginDescription> effects() const;
    void scanPlugins();                        // incremental out-of-process scan; onModelChanged when done
    bool isScanning() const;
    PluginCatalog& catalog();

    // ---- meters / diagnostics ----------------------------------------------------------------
    float inputPeakDb() const;
    float outputPeakDb() const;
    float erleDb() const;                      // NAN until the AEC has measured something
    // Of the open IO; when closed, an estimate from the config (no device frames).
    LatencyBreakdown latency() const;
    std::vector<mac::AudioDeviceInfo> inputDevices() const;
    // One line about the open IO (aggregate id, IOProc callbacks, tap) and the Core Audio restarts
    // handled so far; for logs and the --status developer command.
    juce::String ioDiagnostics() const;

    // ---- platform services (permissions, driver, login item) ---------------------------------
    using DriverCompletion = std::function<void(bool ok, juce::String error)>;
    mac::Permission microphonePermission() const;
    mac::Permission systemAudioPermission() const;   // NotDetermined also means "unknown"
    bool driverInstalled() const;                    // installed and fixomic Mic published
    juce::String bundledDriverVersion() const;       // "" when the app carries no driver
    juce::String installedDriverVersion() const;     // "" when not installed
    bool driverUpdateAvailable() const;              // both known, bundled > installed
    bool driverOperationInProgress() const;
    // Show the administrator authorization dialog (asynchronous, off the message thread). The
    // completion runs on the message thread, never after destruction; the engine re-checks the
    // devices first, and onModelChanged follows.
    void installDriver(DriverCompletion);
    void uninstallDriver(DriverCompletion);
    bool launchAtLogin() const;                      // the login item is enabled (system state)
    // Registers / unregisters the login item, then stores the choice in Config.launchAtLogin
    // (only on success). False with an Italian error otherwise.
    bool setLaunchAtLogin(bool enabled, juce::String& error);

    // State, chain, config or catalog changed. Message thread, never from inside a call above.
    std::function<void()> onModelChanged;

    static constexpr double kSampleRate = 48000.0;
    static constexpr int kTimerMs = 200;
    static constexpr double kSaveDebounceMs = 1000.0;
    static constexpr double kSaveMaxDelayMs = 10000.0;
    static constexpr int kFlushWaitMs = 50;   // max wait for the audio thread's VST3 flush on save

private:
    void timerCallback() override;
    void post(std::function<void(AppController&)>);
    void changed();          // schedule a save and notify
    void notify();
    void scheduleSave();
    void driverOperationDone(bool ok, const juce::String& error, int restartsBefore, const DriverCompletion&);
    void handleServiceRestart();

    ConfigStore store_;
    Config config_;
    PluginCatalog catalog_;
    AecStage aec_;
    PluginChain chain_;
    PluginWindows windows_;   // destroyed before the chain: editors go before their plugins
    ProcessingGraph graph_;
    mac::MacEngineBackend backend_;
    EngineController engine_;
    mac::DemandMonitor demand_;
    mac::WakeObserver wake_;
    mac::PropertyWatcher devicesWatcher_;
    mac::PropertyWatcher defaultOutputWatcher_;
    mac::PropertyWatcher serviceWatcher_;   // kAudioHardwarePropertyServiceRestarted
    int serviceRestarts_ = 0;               // handled so far

    std::vector<juce::String> startupWarnings_;
    juce::String lastWarning_;
    juce::String lastSaveError_;
    double firstUnsavedChange_ = -1.0;   // ms; -1 = nothing to save
    double lastChange_ = 0.0;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AppController)
};

} // namespace fixomic
