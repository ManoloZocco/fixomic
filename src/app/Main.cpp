#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdio>

#include "app/AppController.h"
#include "platform/mac/StatusItem.h"
#include "plugins/PluginCatalog.h"
#include "ui/ChainModel.h"
#include "ui/MainPanel.h"
#include "ui/Snapshot.h"
#include "ui/Theme.h"
#include "ui/TrayIcon.h"

// Command line: only the plugin scan worker, `--scan-worker:<pipe>`, launched by PluginCatalog.
//
// Developer commands exist only when FIXOMIC_DEV_COMMANDS is 1 (CMake: every configuration except
// Release). At launch they run once the event loop runs. They are also forwarded to the running
// instance when the binary is launched again (JUCE passes a second launch's command line to
// anotherInstanceStarted()); results go to the running instance's stderr:
//
//   --set-input <uid>           selects the microphone (the Settings panel replaces it), e.g.
//                               --set-input BuiltInHeadphoneInputDevice
//   --status                    engine state, chain, latency, scan state, open IO, meters
//   --list-effects              the catalog's effects
//   --add-plugin <name> [fmt]   adds the first effect named <name> (format AudioUnit / VST3)
//   --remove-plugin <i> | --open-editor <i> | --bypass <i> <0|1>
//   --set-param <i> <p> <v>     sets parameter p (0..1) of slot i, as if moved in its editor
//   --get-param <i> <p>         prints it
//   --rescan                    incremental plugin scan
//   --platform-status           permissions, driver (installed / versions), launch at login
//   --install-driver            installs the bundled driver (administrator dialog), result logged
//   --uninstall-driver          removes the installed driver (administrator dialog), result logged
//   --launch-at-login <0|1>     registers / unregisters the login item
//   --show-panel                opens the menu-bar panel, as a click on the icon does
//   --tray-menu                 opens the icon's right-click menu and logs whether it is still
//                               open 1 s later
//   --snapshot <dir>            renders into <dir>, at 2x, light and dark: the panel with the current
//                               state (panel-*.png), the four icons (tray-*.png), and the panel with
//                               demo chain data, never the user's: a chain of four plugins, one
//                               bypassed and one missing (chain-*.png), no plugins (chain-empty-*.png),
//                               the plugin picker with a search typed (picker-*.png); their latency row
//                               counts the demo chain's plugins. A relative <dir>
//                               is resolved against the running instance's working directory (/ when
//                               launched with `open`): pass an absolute one

#ifndef FIXOMIC_DEV_COMMANDS
#define FIXOMIC_DEV_COMMANDS 0
#endif

namespace {

using fixomic::AppController;
using fixomic::EngineState;
namespace ui = fixomic::ui;

void log(const juce::String& text) {
    std::fprintf(stderr, "fixomic: [%.3f] %s\n", static_cast<double>(juce::Time::currentTimeMillis()) / 1000.0,
                 text.toRawUTF8());
}

// juce::Logger messages (e.g. why a plugin could not be loaded) in the same log, same format.
struct AppLogger final : juce::Logger {
    void logMessage(const juce::String& message) override { log(message); }
};

#if FIXOMIC_DEV_COMMANDS

juce::String boundsText(std::optional<juce::Rectangle<int>> bounds) {
    return bounds ? bounds->toString() : juce::String("unknown");
}

void printStatus(AppController& app, ui::TrayIcon& tray) {
    log(juce::String("state ") + fixomic::toString(app.engineState()) + " (" + tray.status().text + ")"
        + (app.lastWarning().isNotEmpty() ? ", warning: " + app.lastWarning() : juce::String())
        + ", input " + app.config().inputDeviceUid + ", latency "
        + juce::String(app.latency().totalMs(), 2) + " ms, scanning " + (app.isScanning() ? "yes" : "no")
        + ", " + juce::String(app.effects().size()) + " effects, " + juce::String(app.catalog().blacklisted().size())
        + " blacklisted");
    const auto* panelWindow = tray.panel().getTopLevelComponent();
    log("  icon at " + boundsText(fixomic::mac::statusItemScreenBounds(tray.getNativeHandle())) + ", panel "
        + (tray.isPanelShowing() ? "shown at " + panelWindow->getScreenBounds().toString() : juce::String("hidden"))
        + ", menu " + (tray.isMenuShowing() ? "open" : "closed"));
    const auto slots = app.slots();
    for (size_t i = 0; i < slots.size(); ++i)
        log("  slot " + juce::String(static_cast<int>(i)) + ": " + slots[i].name + (slots[i].missing ? " [missing]" : "")
            + (slots[i].bypassed ? " [bypassed]" : "") + ", latency " + juce::String(slots[i].latencySamples));
    for (const auto& warning : app.startupWarnings()) log("  startup warning: " + warning);
    log("  " + app.ioDiagnostics() + ", input peak " + juce::String(app.inputPeakDb(), 1) + " dB, output peak "
        + juce::String(app.outputPeakDb(), 1) + " dB, ERLE " + ui::formatErle(app.erleDb()));
}

const char* permissionText(fixomic::mac::Permission permission) {
    switch (permission) {
    case fixomic::mac::Permission::Granted: return "granted";
    case fixomic::mac::Permission::Denied: return "denied";
    case fixomic::mac::Permission::NotDetermined: return "not determined";
    }
    return "?";
}

void printPlatformStatus(AppController& app) {
    log(juce::String("microphone ") + permissionText(app.microphonePermission()) + ", system audio "
        + permissionText(app.systemAudioPermission()) + ", driver "
        + (app.driverInstalled() ? "installed" : "not installed") + " (installed '" + app.installedDriverVersion()
        + "', bundled '" + app.bundledDriverVersion() + "'" + (app.driverUpdateAvailable() ? ", update available" : "")
        + (app.driverOperationInProgress() ? ", operation in progress" : "") + "), launch at login "
        + (app.launchAtLogin() ? "enabled" : "not enabled") + " (config " + (app.config().launchAtLogin ? "on" : "off")
        + ")");
}

// Demo data for the chain section's snapshots: four plugins, one bypassed, one missing.
std::vector<fixomic::PluginChain::SlotInfo> demoChain() {
    return {{"Nectar 4 Gate", false, false, 48},
            {"AUNBandEQ", true, false, 0},
            {"De-esser", false, true, 0},
            {"AUPeakLimiter", false, false, 0}};
}

// Demo catalog for the picker's snapshot: the effects macOS ships, and one third-party plugin.
std::vector<juce::PluginDescription> demoEffects() {
    std::vector<juce::PluginDescription> effects;
    const auto add = [&](const char* name, const char* manufacturer, const char* format) {
        juce::PluginDescription d;
        d.name = name;
        d.manufacturerName = manufacturer;
        d.pluginFormatName = format;
        d.fileOrIdentifier = juce::String(format) + "/" + name;
        effects.push_back(d);
    };
    for (const auto* name : {"AUBandpass", "AUDelay", "AUDynamicsProcessor", "AUGraphicEQ", "AUHighShelfFilter",
                             "AUMatrixReverb", "AUMultibandCompressor", "AUNBandEQ", "AUParametricEQ", "AUPeakLimiter"})
        add(name, "Apple", "AudioUnit");
    add("Nectar 4 Gate", "iZotope", "VST3");
    return effects;
}

// The latency of `chain` with the app's buffer, AEC and devices: the latency row of a demo panel
// agrees with the chain it shows.
fixomic::LatencyBreakdown demoLatency(AppController& app, const std::vector<fixomic::PluginChain::SlotInfo>& chain) {
    auto latency = app.latency();
    latency.pluginFrames = app.config().globalBypass ? 0 : ui::chainLatencySamples(chain);
    return latency;
}

// Renders the UI: the panel with the current state, the icons, and the chain section with demo data.
void writeSnapshots(AppController& app, const ui::TrayIcon& tray, const juce::String& path) {
    const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    const auto fit = [](ui::MainPanel& panel) {
        panel.setSize(ui::MainPanel::kWidth, panel.preferredHeight(ui::MainPanel::kWidth));
    };
    ui::MainPanel panel(app), chain(app), emptyChain(app), picker(app);
    for (auto* p : {&panel, &chain, &emptyChain, &picker}) {
        p->refresh(tray.status());
        fit(*p);
    }
    chain.showChain(demoChain(), demoLatency(app, demoChain()));
    emptyChain.showChain({}, demoLatency(app, {}));
    picker.showPicker();
    picker.showChain(demoChain(), demoLatency(app, demoChain()));   // the chain the picker adds to
    picker.picker().setEffects(demoEffects());
    picker.picker().setScanning(false);
    picker.picker().setSearchText("eq");
    for (auto* p : {&chain, &emptyChain, &picker}) fit(*p);
    const auto icons = ui::createTrayIconSheet();

    const std::vector<ui::SnapshotTarget> targets{{"panel", &panel},
                                                  {"tray", icons.get()},
                                                  {"chain", &chain},
                                                  {"chain-empty", &emptyChain},
                                                  {"picker", &picker}};
    juce::String error;
    for (const auto& file : ui::writeSnapshots(directory, targets, error)) log("snapshot " + file.getFullPathName());
    if (error.isNotEmpty()) log("snapshot failed: " + error);
}

// Applies the options in `commandLine`; returns false if one was not understood.
bool runCommands(AppController& app, ui::TrayIcon& tray, const juce::String& commandLine) {
    juce::StringArray args;
    args.addTokens(commandLine, true);
    args.removeEmptyStrings();
    for (auto& arg : args) arg = arg.unquoted();
    const auto intArg = [&](int i) { return i < args.size() ? args[i].getIntValue() : -1; };

    for (int i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--set-input" && i + 1 < args.size()) {
            const auto uid = args[++i];
            app.updateConfig([&](fixomic::Config& c) { c.inputDeviceUid = uid; });
        } else if (arg == "--status") {
            printStatus(app, tray);
        } else if (arg == "--list-effects") {
            for (const auto& d : app.effects()) log(d.pluginFormatName + "  " + d.name + "  " + d.fileOrIdentifier);
        } else if (arg == "--add-plugin" && i + 1 < args.size()) {
            const auto name = args[++i];
            juce::String format;
            if (i + 1 < args.size() && !args[i + 1].startsWith("--")) format = args[++i];
            const auto effects = app.effects();
            const auto it = std::find_if(effects.begin(), effects.end(), [&](const juce::PluginDescription& d) {
                return d.name == name && (format.isEmpty() || d.pluginFormatName == format);
            });
            if (it == effects.end())
                log("no effect named " + name + " " + format);
            else if (const auto error = app.addPlugin(*it); error.isNotEmpty())
                log(error);
            else
                log("added " + it->name + " (" + it->pluginFormatName + ")");
        } else if (arg == "--remove-plugin") {
            app.removePlugin(intArg(++i));
        } else if (arg == "--open-editor") {
            if (!app.openEditor(intArg(++i))) log("no editor for slot " + args[i]);
        } else if (arg == "--bypass") {
            const int index = intArg(++i);
            app.setPluginBypassed(index, intArg(++i) != 0);
        } else if ((arg == "--set-param" && i + 3 < args.size()) || (arg == "--get-param" && i + 2 < args.size())) {
            const int index = intArg(++i), param = intArg(++i);
            auto* plugin = app.plugin(index);
            auto* parameter = plugin != nullptr ? plugin->getParameters()[param] : nullptr;
            if (arg == "--set-param") {
                const float value = args[++i].getFloatValue();
                if (parameter != nullptr) parameter->setValueNotifyingHost(value);
            }
            if (parameter == nullptr)
                log("no parameter " + juce::String(param) + " in slot " + juce::String(index));
            else
                log("slot " + juce::String(index) + " " + plugin->getName() + " param " + juce::String(param) + " '"
                    + parameter->getName(64) + "' = " + juce::String(parameter->getValue(), 4) + " ("
                    + parameter->getCurrentValueAsText() + ")");
        } else if (arg == "--rescan") {
            app.scanPlugins();
        } else if (arg == "--platform-status") {
            printPlatformStatus(app);
        } else if (arg == "--install-driver" || arg == "--uninstall-driver") {
            const bool install = arg == "--install-driver";
            const double start = juce::Time::getMillisecondCounterHiRes();
            auto done = [install, start](bool ok, juce::String error) {
                log(juce::String(install ? "install-driver" : "uninstall-driver") + (ok ? " ok" : " failed: " + error)
                    + " (" + juce::String((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) + " s)");
            };
            log(juce::String(install ? "installing" : "uninstalling") + " the driver...");
            if (install)
                app.installDriver(done);
            else
                app.uninstallDriver(done);
        } else if (arg == "--launch-at-login" && i + 1 < args.size()) {
            juce::String error;
            const bool enable = intArg(++i) != 0;
            if (app.setLaunchAtLogin(enable, error))
                log(juce::String("launch at login ") + (enable ? "on" : "off") + ", system state "
                    + (app.launchAtLogin() ? "enabled" : "not enabled"));
            else
                log("launch at login: " + error);
        } else if (arg == "--tray-menu") {
            // The right-click menu, then whether it is still open a second later (an inactive
            // LSUIElement app loses a JUCE menu on its first timer tick).
            tray.showMenu();
            log("tray menu requested");
            juce::Timer::callAfterDelay(1000, [safe = juce::Component::SafePointer<ui::TrayIcon>(&tray)] {
                if (safe == nullptr) return;
                log(juce::String("tray menu 1 s later: ") + (safe->isMenuShowing() ? "open" : "closed")
                    + ", app active " + (juce::Process::isForegroundProcess() ? "yes" : "no") + ", modal components "
                    + juce::String(juce::ModalComponentManager::getInstance()->getNumModalComponents()));
            });
        } else if (arg == "--show-panel") {
            tray.showPanel();
            log("panel shown");
        } else if (arg == "--snapshot" && i + 1 < args.size()) {
            writeSnapshots(app, tray, args[++i]);
        } else {
            log("unknown option " + arg);
            return false;
        }
    }
    return true;
}

#endif // FIXOMIC_DEV_COMMANDS

class FixomicApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // One app instance; scan workers (this binary relaunched by PluginCatalog) run alongside it.
    bool moreThanOneInstanceAllowed() override {
        return fixomic::PluginScanWorker::isWorkerCommandLine(getCommandLineParameters());
    }

    void initialise(const juce::String& commandLine) override {
        if (fixomic::PluginScanWorker::isWorkerCommandLine(commandLine)) {
            scanWorker_ = fixomic::PluginScanWorker::connect(commandLine);
            if (scanWorker_ == nullptr) quit();
            return;
        }
        juce::Logger::setCurrentLogger(&logger_);
        theme_ = std::make_unique<ui::Theme>();   // follows the system appearance
        juce::LookAndFeel::setDefaultLookAndFeel(theme_.get());

        app_ = std::make_unique<AppController>();
        tray_ = std::make_unique<ui::TrayIcon>(*app_);
        tray_->onQuit = [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); };
#if FIXOMIC_DEV_COMMANDS
        tray_->onMenuChanged = [](bool open) { log(open ? "tray menu open" : "tray menu closed"); };
#endif
        // tray_->onOpenSettings: the Settings panel (Task 10c).

        // The one model handler: the log, then the menu-bar icon and its panel.
        app_->onModelChanged = [this] {
            logStateChange();
            if (tray_ != nullptr) tray_->modelChanged();
        };
        for (const auto& warning : app_->startupWarnings()) log("startup warning: " + warning);
#if FIXOMIC_DEV_COMMANDS
        // Once the event loop runs: initialise() is called before the app finished launching, when
        // macOS ignores activation (--show-panel and --tray-menu activate the app).
        juce::MessageManager::callAsync([this, commandLine] {
            if (app_ != nullptr && tray_ != nullptr) runCommands(*app_, *tray_, commandLine);
        });
#endif
    }

    void anotherInstanceStarted([[maybe_unused]] const juce::String& commandLine) override {
#if FIXOMIC_DEV_COMMANDS
        if (app_ != nullptr && tray_ != nullptr) runCommands(*app_, *tray_, commandLine);
#endif
    }

    void shutdown() override {
        tray_.reset();
        app_.reset();   // saves the config, closes the IO, unloads the plugins
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        theme_.reset();
        juce::Logger::setCurrentLogger(nullptr);
        scanWorker_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    void logStateChange() {
        if (app_->engineState() == lastState_) return;
        lastState_ = app_->engineState();
        log(juce::String("engine state -> ") + fixomic::toString(lastState_)
            + (lastState_ == EngineState::Error ? ": " + app_->lastError() : juce::String()));
    }

    AppLogger logger_;
    std::unique_ptr<fixomic::PluginScanWorker> scanWorker_;
    std::unique_ptr<ui::Theme> theme_;
    std::unique_ptr<AppController> app_;
    std::unique_ptr<ui::TrayIcon> tray_;
    EngineState lastState_ = EngineState::Idle;
};

} // namespace

START_JUCE_APPLICATION(FixomicApplication)
