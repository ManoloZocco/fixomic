#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdio>

#include "app/AppController.h"
#include "plugins/PluginCatalog.h"

// Command line (besides the plugin scan worker, `--scan-worker:<pipe>`, launched by PluginCatalog):
//
//   --set-input <uid>           selects the microphone (fixomic never picks one by itself; the UI of
//                               Task 10 replaces this), e.g. --set-input BuiltInHeadphoneInputDevice
//
// Developer commands, also forwarded to the running instance when the binary is launched again
// (JUCE passes a second launch's command line to anotherInstanceStarted()); results go to the
// running instance's stderr:
//
//   --status                    engine state, chain, latency, scan state
//   --list-effects              the catalog's effects
//   --add-plugin <name> [fmt]   adds the first effect named <name> (format AudioUnit / VST3)
//   --remove-plugin <i> | --open-editor <i> | --bypass <i> <0|1>
//   --set-param <i> <p> <v>     sets parameter p (0..1) of slot i, as if moved in its editor
//   --get-param <i> <p>         prints it
//   --rescan                    incremental plugin scan

namespace {

using fixomic::AppController;
using fixomic::EngineState;

juce::String statusText(const AppController& app) {
    switch (app.engineState()) {
    case EngineState::Idle: return "In attesa: nessuna app usa fixomic Mic";
    case EngineState::Starting: return "Avvio in corso...";
    case EngineState::Running: return "Attivo";
    case EngineState::MicMissing:
        return app.config().inputDeviceUid.isEmpty() ? "Nessun microfono selezionato" : "Microfono non disponibile";
    case EngineState::DriverMissing: return "Driver fixomic non installato";
    case EngineState::UnsupportedSampleRate: return "Il microfono non supporta 48 kHz";
    case EngineState::Error: return "Errore: " + app.lastError();
    }
    return {};
}

void log(const juce::String& text) {
    std::fprintf(stderr, "fixomic: [%.3f] %s\n", static_cast<double>(juce::Time::currentTimeMillis()) / 1000.0,
                 text.toRawUTF8());
}

void printStatus(AppController& app) {
    log(juce::String("state ") + fixomic::toString(app.engineState()) + " (" + statusText(app) + ")"
        + (app.lastWarning().isNotEmpty() ? ", warning: " + app.lastWarning() : juce::String())
        + ", input " + app.config().inputDeviceUid + ", latency "
        + juce::String(app.latency().totalMs(), 2) + " ms, scanning " + (app.isScanning() ? "yes" : "no")
        + ", " + juce::String(app.effects().size()) + " effects, " + juce::String(app.catalog().blacklisted().size())
        + " blacklisted");
    const auto slots = app.slots();
    for (size_t i = 0; i < slots.size(); ++i)
        log("  slot " + juce::String(static_cast<int>(i)) + ": " + slots[i].name + (slots[i].missing ? " [missing]" : "")
            + (slots[i].bypassed ? " [bypassed]" : "") + ", latency " + juce::String(slots[i].latencySamples));
    for (const auto& warning : app.startupWarnings()) log("  startup warning: " + warning);
}

// Applies the options in `commandLine`; returns false if one was not understood.
bool runCommands(AppController& app, const juce::String& commandLine) {
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
            printStatus(app);
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
        } else {
            log("unknown option " + arg);
            return false;
        }
    }
    return true;
}

// Menu-bar icon: a simple filled circle drawn as a template image (macOS tints it
// for light/dark menu bars).
juce::Image makeTrayIcon() {
    constexpr int size = 44; // 22 pt @2x
    juce::Image image(juce::Image::ARGB, size, size, true);
    juce::Graphics g(image);
    g.setColour(juce::Colours::black);
    g.fillEllipse(juce::Rectangle<float>(size, size).reduced(8.0f));
    return image;
}

// Minimal tray until the real UI (Task 10): status line and "Esci".
class TrayIcon final : public juce::SystemTrayIconComponent {
public:
    explicit TrayIcon(AppController& app) : app_(app) {
        setIconImage(makeTrayIcon(), makeTrayIcon());
        setIconTooltip("fixomic");
    }

    void mouseDown(const juce::MouseEvent&) override {
        juce::PopupMenu menu;
        menu.addItem(juce::PopupMenu::Item(statusText(app_)).setEnabled(false));
        menu.addSeparator();
        menu.addItem("Esci", [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
        menu.showMenuAsync(juce::PopupMenu::Options{});
    }

private:
    AppController& app_;
};

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
        app_ = std::make_unique<AppController>();
        app_->onModelChanged = [this] {
            if (app_->engineState() != lastState_) {
                lastState_ = app_->engineState();
                log(juce::String("engine state -> ") + fixomic::toString(lastState_)
                    + (lastState_ == EngineState::Error ? ": " + app_->lastError() : juce::String()));
            }
        };
        for (const auto& warning : app_->startupWarnings()) log("startup warning: " + warning);
        runCommands(*app_, commandLine);
        tray_ = std::make_unique<TrayIcon>(*app_);
    }

    void anotherInstanceStarted(const juce::String& commandLine) override {
        if (app_ != nullptr) runCommands(*app_, commandLine);
    }

    void shutdown() override {
        tray_.reset();
        app_.reset();   // saves the config, closes the IO, unloads the plugins
        scanWorker_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<fixomic::PluginScanWorker> scanWorker_;
    std::unique_ptr<AppController> app_;
    std::unique_ptr<TrayIcon> tray_;
    EngineState lastState_ = EngineState::Idle;
};

} // namespace

START_JUCE_APPLICATION(FixomicApplication)
