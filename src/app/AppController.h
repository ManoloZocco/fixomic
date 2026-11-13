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
#include <optional>
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
// that fail to load stay as missing slots, see startupWarnings()). While the plugins load, an empty
// file kRestoreMarkerName sits next to the config file; if a launch finds it, the previous one died
// loading them (a crashing plugin), so this one restores the chain with every slot missing ("Plugin
// non trovato", removable, the saved description and state kept for the next launch) and reports
// it with interruptedRestoreWarning() as its startup warning. Then the engine is configured, demand
// monitoring started, an incremental plugin scan started in the background. A 200 ms timer drives
// EngineController::tick(), PluginChain::collectGarbage() and the debounced config save, and
// notifies when the AEC starts or stops processing (it changes latency()).
//
// Config: config() is the single source of truth, also for the engine backend. It is saved 1 s
// after the last change (settings, chain structure, bypass, plugin state, an editor closed; at most
// 10 s after the first of a stream of changes) and on destruction; a config that did not state
// onboardingDone (no file, or one written before the flag) also 1 s after startup, so it states it
// from then on. A save is skipped when the file
// already holds the same XML; a failed write stays pending, is retried 1 s later and is reported by
// lastSaveError(). Plugin state is captured with
// PluginChain::saveState() when saving. The chain field of config() is owned by the controller:
// it reflects the chain as last saved, and edits to it through updateConfig() are ignored.
//
// Never picks a microphone by itself: with no input selected the engine stays MicMissing. The
// configured one's name is stored with it (Config.inputDeviceName, shown while it is not connected);
// a config written before names were stored learns it the first time the device is connected.
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
        PluginCatalog::Options catalog;   // scan worker and locations (tests point them elsewhere)
    };

    AppController();
    explicit AppController(Options);
    ~AppController() override;   // saves, closes editors, closes the IO, then destroys the plugins
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // ---- engine ------------------------------------------------------------------------------
    EngineState engineState() const;
    juce::String lastError() const;      // why the engine is in Error (or the last open failed)
    // The permission prompt the last open attempt is waiting for: with it, Error means "waiting for
    // the user", not a failure. None after a successful open or any other failure.
    PendingPermission pendingPermission() const;
    // The backend's current warning (e.g. no system audio tap), "" if none. About the last open
    // attempt: a new attempt starts without one (MacEngineBackend::warning()).
    juce::String lastWarning() const;
    std::optional<mac::WarningKind> lastWarningKind() const;   // what it is about; nullopt without one
    // The System Settings pane that fixes lastWarning(): the system audio pane for a denied or silent
    // tap (ui::warningPane); nullopt for a tap that could not be created (opening a pane cannot fix
    // it, and the prompt may not have been shown) and without a warning.
    std::optional<mac::PrivacyPane> lastWarningPane() const;

    // ---- config ------------------------------------------------------------------------------
    const Config& config() const;
    // The config file stated onboardingDone when it was loaded at startup. False for no file, a
    // corrupt one, or one written before the flag existed (Config's rule then decided its value).
    bool onboardingFlagStored() const;
    // Edits a copy, applies it to the engine (EngineController::setConfig) and schedules a save.
    void updateConfig(std::function<void(Config&)>);
    bool saveNow();                      // false if the write failed (retried by the timer)
    juce::String lastSaveError() const;  // Italian, "" after a successful save

    // ---- plugin chain ------------------------------------------------------------------------
    // Instantiates and appends a plugin. Returns "" on success, else pluginLoadFailedText() of its
    // name, for the UI; why it failed (the plugin format's message) is written to juce::Logger.
    juce::String addPlugin(const juce::PluginDescription&);
    // "Impossibile caricare «Nectar 4 Gate»." ("… il plugin." without a name).
    static juce::String pluginLoadFailedText(const juce::String& pluginName);
    // The startup warning after a launch that died loading the chain's plugins.
    static juce::String interruptedRestoreWarning();
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
    // The prompts for the first-run guide, shared with the engine's own (MacEngineBackend): one
    // prompt, whoever asks first. `done` runs later on the message thread (never inside the call,
    // never after destruction): granted / ok = answered (the system audio outcome stays unknown, see
    // Permissions.h). A notification follows each call (the pending state changed).
    void requestMicrophone(std::function<void(bool granted)> done);
    void requestSystemAudio(std::function<void(bool ok)> done);
    bool microphoneRequestPending() const;
    bool systemAudioRequestPending() const;
    bool systemAudioAnswered() const;                // a System Audio Recording prompt was answered
    void openPrivacySettings(mac::PrivacyPane) const;
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
    // One handler: the app composes its consumers (log, menu-bar icon and panel) in it.
    std::function<void()> onModelChanged;

    static constexpr double kSampleRate = 48000.0;
    static constexpr int kTimerMs = 200;
    static constexpr double kSaveDebounceMs = 1000.0;
    static constexpr double kSaveMaxDelayMs = 10000.0;
    static constexpr int kFlushWaitMs = 50;   // max wait for the audio thread's VST3 flush on save
    // Next to the config file while the chain's plugins load at startup (see the class comment).
    static constexpr const char* kRestoreMarkerName = "restoring-plugins";

private:
    void timerCallback() override;
    void post(std::function<void(AppController&)>);
    void changed();          // schedule a save and notify
    void notify();
    void scheduleSave();
    void driverOperationDone(bool ok, const juce::String& error, int restartsBefore, const DriverCompletion&);
    void handleServiceRestart();
    void rememberInputName();   // stores the configured microphone's name when it is not stored yet
    void restoreChain();        // the chain from config_, guarded by the restore marker

    ConfigStore store_;
    bool onboardingFlagStored_ = false;   // declared before config_: set while config_ loads
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
    bool aecActive_ = false;                // as last notified (see timerCallback)

    std::vector<juce::String> startupWarnings_;
    juce::String lastSaveError_;
    double firstUnsavedChange_ = -1.0;   // ms; -1 = nothing to save
    double lastChange_ = 0.0;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AppController)
};

} // namespace fixomic
