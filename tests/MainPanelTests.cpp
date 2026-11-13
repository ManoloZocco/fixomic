#include <catch2/catch_test_macros.hpp>

#include "app/AppController.h"
#include "ui/ChainModel.h"
#include "ui/MainPanel.h"
#include "ui/SetupModel.h"
#include "core/Config.h"
#include "core/ConfigStore.h"

#include <CoreFoundation/CoreFoundation.h>

#include <functional>
#include <vector>

using fixomic::AppController;
using namespace fixomic::ui;

// The real panel window on screen, over a real AppController that runs on temp files: no input
// selected (the engine stays MicMissing and never opens audio), a scan of an empty folder. Keys go
// through the window's peer, as typed keys do; plugins are Apple's stock Audio Units.

namespace {

juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("fixomic-panel-test", "", false);
    TempDir() { REQUIRE(dir.createDirectory()); }
    ~TempDir() { dir.deleteRecursively(); }
};

AppController::Options appOptions(const juce::File& dir) {
    AppController::Options o;
    o.configFile = dir.getChildFile("config.xml");
    o.pluginCacheFile = dir.getChildFile("plugins.xml");
    o.catalog.workerExecutable = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                     .getSiblingFile("fixomic_test_scan_worker");
    o.catalog.scanAudioUnits = false;
    const auto noPlugins = dir.getChildFile("vst3");
    REQUIRE(noPlugins.createDirectory());
    o.catalog.vst3SearchPath = juce::FileSearchPath(noPlugins.getFullPathName());
    return o;
}

// Services the main run loop (JUCE's message queue on macOS) for about `ms` milliseconds.
void pumpMessages(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
}

// Services it until `done` (true) or `ms` passed (false): asynchronous steps can queue behind the
// panel's own refreshes.
bool pumpUntil(const std::function<bool()>& done, int ms = 2000) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (!done() && juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    return done();
}

juce::PluginDescription appleEffect(const char* name, const char* code) {
    juce::PluginDescription d;
    d.name = name;
    d.manufacturerName = "Apple";
    d.pluginFormatName = "AudioUnit";
    d.category = "Effect";
    d.fileOrIdentifier = juce::String("AudioUnit:Effects/aufx,") + code + ",appl";
    return d;
}

template <typename T>
T* findChild(juce::Component& root, const std::function<bool(T&)>& matches) {
    for (auto* child : root.getChildren()) {
        if (auto* typed = dynamic_cast<T*>(child); typed != nullptr && matches(*typed)) return typed;
        if (auto* found = findChild<T>(*child, matches)) return found;
    }
    return nullptr;
}

juce::MouseEvent mouseAt(juce::Component& target, juce::Point<float> position, juce::Point<float> downAt,
                         bool dragged) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
                            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &target, &target, now, downAt, now, 1, dragged);
}

struct LogCapture : juce::Logger {
    juce::StringArray lines;
    LogCapture() { juce::Logger::setCurrentLogger(this); }
    ~LogCapture() override { juce::Logger::setCurrentLogger(nullptr); }
    void logMessage(const juce::String& message) override { lines.add(message); }
};

struct PanelOnScreen {
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir tmp;
    AppController app{appOptions(tmp.dir)};
    MainPanelWindow window{app};

    PanelOnScreen() {
        app.onModelChanged = [this] { refresh(); };   // as the app does, through the tray icon
        window.addToDesktop();
        window.setTopLeftPosition(60, 120);
        window.setVisible(true);
        window.grabKeyboardFocus();
        REQUIRE(window.hasKeyboardFocus(false));
    }

    MainPanel& panel() { return window.panel(); }
    void refresh() { panel().refresh(StatusPresentation{}); }
    void press(int keyCode) {
        REQUIRE(window.getPeer() != nullptr);
        window.getPeer()->handleKeyPress(juce::KeyPress(keyCode));
    }

    juce::ToggleButton& bypassSwitch() {
        auto* button =
            findChild<juce::ToggleButton>(window, [](auto& b) { return b.getButtonText() == "Bypass globale"; });
        REQUIRE(button != nullptr);
        return *button;
    }
    juce::TextEditor& searchField() {
        auto* editor = findChild<juce::TextEditor>(window, [](auto& e) { return e.getTitle() == "Cerca plugin"; });
        REQUIRE(editor != nullptr);
        return *editor;
    }
    juce::Button& backButton() {
        auto* button =
            findChild<juce::Button>(window, [](auto& b) { return b.getButtonText() == "Torna alla catena"; });
        REQUIRE(button != nullptr);
        return *button;
    }

    // After the picker closed: the keyboard focus is the window's, and Return toggles nothing.
    void requireFocusBackOnTheWindow() {
        REQUIRE_FALSE(panel().isPickerOpen());
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        REQUIRE(focused != &bypassSwitch());
        REQUIRE(dynamic_cast<juce::Button*>(focused) == nullptr);
        REQUIRE(focused == &window);
        press(juce::KeyPress::returnKey);
        pumpMessages(100);   // a focused button clicks asynchronously
        REQUIRE_FALSE(app.config().globalBypass);
        REQUIRE_FALSE(bypassSwitch().getToggleState());
        REQUIRE(window.isVisible());
    }
};

} // namespace

TEST_CASE("Closing the plugin picker gives the keyboard focus back to the window", "[ui][panel][gui]") {
    PanelOnScreen s;
    s.panel().showPicker();
    REQUIRE(s.panel().isPickerOpen());
    REQUIRE(s.searchField().hasKeyboardFocus(false));

    SECTION("Escape") {
        s.press(juce::KeyPress::escapeKey);
        s.requireFocusBackOnTheWindow();
    }
    SECTION("back") {
        s.backButton().triggerClick();
        pumpMessages(100);
        s.requireFocusBackOnTheWindow();
    }
    SECTION("a plugin chosen and added") {
        s.panel().picker().setEffects({appleEffect("AUDelay", "dely")});
        s.panel().picker().choose(0);
        REQUIRE(s.app.slots().size() == 1);
        s.requireFocusBackOnTheWindow();
    }
}

TEST_CASE("Escape during a drag cancels the drag; the next one closes the panel", "[ui][panel][gui]") {
    PanelOnScreen s;
    REQUIRE(s.app.addPlugin(appleEffect("AUDelay", "dely")).isEmpty());
    REQUIRE(s.app.addPlugin(appleEffect("AUBandpass", "bpas")).isEmpty());
    s.refresh();
    auto& list = s.panel().chainList();
    REQUIRE(list.slots().size() == 2);

    auto& row = *list.row(0);
    const juce::Point<float> down{30.0f, 16.0f};
    row.mouseDown(mouseAt(row, down, down, false));
    row.mouseDrag(mouseAt(row, {30.0f, 30.0f}, down, true));
    row.mouseDrag(mouseAt(row, {30.0f, 60.0f}, down, true));   // over the gap under the second row
    REQUIRE(list.draggedRow() == 0);

    s.press(juce::KeyPress::escapeKey);
    REQUIRE_FALSE(list.draggedRow().has_value());
    REQUIRE(s.window.isVisible());
    row.mouseDrag(mouseAt(row, {30.0f, 62.0f}, down, true));
    row.mouseUp(mouseAt(row, {30.0f, 62.0f}, down, true));
    pumpMessages(50);
    const auto slots = s.app.slots();
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].name == "AUDelay");
    REQUIRE(slots[1].name == "AUBandpass");

    s.press(juce::KeyPress::escapeKey);
    REQUIRE_FALSE(s.window.isVisible());
}

TEST_CASE("A plugin that cannot be loaded is reported in Italian; why goes to the log", "[ui][panel][gui]") {
    REQUIRE(AppController::pluginLoadFailedText("Nectar 4 Gate") == utf8("Impossibile caricare «Nectar 4 Gate»."));
    REQUIRE(AppController::pluginLoadFailedText({}) == "Impossibile caricare il plugin.");

    PanelOnScreen s;
    LogCapture log;
    s.panel().showPicker();
    s.panel().picker().setEffects({appleEffect("Bogus FX", "zzzz")});
    s.panel().picker().choose(0);

    REQUIRE(s.app.slots().empty());
    REQUIRE(s.panel().isPickerOpen());
    REQUIRE(s.panel().picker().showsError());
    REQUIRE(s.panel().picker().statusText() == utf8("Impossibile caricare «Bogus FX»."));
    // The format's own message (English, technical) is logged, not shown.
    REQUIRE(log.lines.size() == 1);
    REQUIRE(log.lines[0].contains("Bogus FX"));
    REQUIRE(log.lines[0].length() > juce::String("Bogus FX").length() + 20);
}

TEST_CASE("Impostazioni opens the settings in the panel; back and Escape return to it", "[ui][panel][gui]") {
    PanelOnScreen s;
    auto* settingsButton = findChild<juce::Button>(s.window, [](auto& b) { return b.getButtonText() == utf8("Impostazioni…"); });
    REQUIRE(settingsButton != nullptr);
    settingsButton->onClick();
    REQUIRE(s.panel().page() == MainPanel::Page::Settings);
    REQUIRE(s.panel().settings().isVisible());
    REQUIRE_FALSE(s.bypassSwitch().isVisible());   // the view takes the panel's place
    REQUIRE(s.panel().getHeight() == s.panel().preferredHeight(MainPanel::kWidth));

    SECTION("Escape: back to the panel, the focus on the window, the panel still open") {
        s.press(juce::KeyPress::escapeKey);
        REQUIRE(s.panel().page() == MainPanel::Page::Main);
        REQUIRE(s.bypassSwitch().isVisible());
        REQUIRE(s.window.isVisible());
        REQUIRE(juce::Component::getCurrentlyFocusedComponent() == &s.window);
        s.press(juce::KeyPress::escapeKey);   // then the panel itself
        REQUIRE_FALSE(s.window.isVisible());
    }
    SECTION("back") {
        auto* back = findChild<juce::Button>(s.window, [](auto& b) { return b.getButtonText() == "Torna al pannello"; });
        REQUIRE(back != nullptr);
        back->onClick();
        REQUIRE(s.panel().page() == MainPanel::Page::Main);
    }
    SECTION("dismissed: it reopens on the panel") {
        s.window.dismiss();
        REQUIRE(s.panel().page() == MainPanel::Page::Main);
    }
}

TEST_CASE("The settings apply a buffer size and a microphone through the AppController", "[ui][panel][gui]") {
    PanelOnScreen s;
    s.panel().showSettings();
    auto& settings = s.panel().settings();
    REQUIRE(settings.data().bufferSize == 128);

    // A new buffer size applies at once: the latency estimate follows (both directions).
    auto* b256 = findChild<juce::Button>(settings, [](auto& b) { return b.getButtonText() == utf8("256 · 5,3 ms"); });
    REQUIRE(b256 != nullptr);
    b256->onClick();
    REQUIRE(pumpUntil([&] { return s.app.config().bufferSize == 256; }));
    REQUIRE(s.app.config().bufferSize == 256);
    REQUIRE(s.app.latency().bufferFrames == 2 * 256);
    REQUIRE(settings.data().bufferSize == 256);   // the view re-read it
    REQUIRE(b256->getToggleState());

    // A microphone: its uid and name, the first channel. (Not a real device: the engine stays
    // MicMissing and opens nothing.)
    s.app.updateConfig([](fixomic::Config& c) { c.inputChannel = 1; });
    DeviceChoice choice;
    choice.uid = "no-such-mic";
    choice.name = "Microfono finto";
    settings.onChooseDevice(choice);
    REQUIRE(s.app.config().inputDeviceUid == "no-such-mic");
    REQUIRE(s.app.config().inputDeviceName == "Microfono finto");
    REQUIRE(s.app.config().inputChannel == 0);
    // It is not connected: listed first, checked, as non collegato.
    REQUIRE_FALSE(settings.data().devices.empty());
    REQUIRE(settings.data().devices[0].text == utf8("Microfono finto — non collegato"));
    REQUIRE(settings.data().devices[0].selected);
}

TEST_CASE("The first-run guide: Fine marks it done, closing it early does not", "[ui][panel][gui]") {
    PanelOnScreen s;   // a new config: the guide is not done
    REQUIRE_FALSE(s.app.config().onboardingDone);
    REQUIRE(s.panel().showsSetupReminder());
    auto* complete = findChild<juce::Button>(s.window, [](auto& b) { return b.getButtonText() == "Completa la configurazione"; });
    REQUIRE(complete != nullptr);
    complete->onClick();
    REQUIRE(s.panel().page() == MainPanel::Page::Onboarding);
    REQUIRE(s.panel().holdsOpen());

    s.press(juce::KeyPress::escapeKey);   // closed early
    REQUIRE(s.panel().page() == MainPanel::Page::Main);
    REQUIRE_FALSE(s.app.config().onboardingDone);
    REQUIRE(s.panel().showsSetupReminder());
    REQUIRE_FALSE(s.panel().holdsOpen());

    // From the settings, and back to them.
    s.panel().showSettings();
    auto* review = findChild<juce::Button>(s.window, [](auto& b) { return b.getButtonText() == "Rivedi la configurazione iniziale"; });
    REQUIRE(review != nullptr);
    review->onClick();
    REQUIRE(s.panel().page() == MainPanel::Page::Onboarding);
    auto& guide = s.panel().onboarding();
    while (guide.currentStep() < kSetupStepCount - 1) guide.skip();   // whatever this Mac has done already
    REQUIRE(guide.primaryButton().getButtonText() == "Fine");
    guide.primaryButton().onClick();
    REQUIRE(s.app.config().onboardingDone);
    REQUIRE(s.panel().page() == MainPanel::Page::Settings);
    s.panel().closePage();
    REQUIRE_FALSE(s.panel().showsSetupReminder());
    REQUIRE(s.app.saveNow());
    REQUIRE(fixomic::fromXmlString(s.tmp.dir.getChildFile("config.xml").loadFileAsString())->onboardingDone);
}

// Focus loss itself is reported by JUCE only to a foreground app (not to this test process): the
// window's response to it is driven directly.
TEST_CASE("The panel stays up for the guide when the app loses the focus", "[ui][panel][gui]") {
    PanelOnScreen s;
    SECTION("the guide: held open (a prompt, the administrator dialog, System Settings)") {
        s.panel().showOnboarding();
        s.window.activeStatusChanged(false);
        REQUIRE(s.window.isVisible());
        REQUIRE(s.panel().page() == MainPanel::Page::Onboarding);
        s.window.activeStatusChanged(true);   // back: re-read, still open
        REQUIRE(s.window.isVisible());
    }
    SECTION("the settings and the panel: dismissed, as menu-bar extras are; reopened on the panel") {
        s.panel().showSettings();
        REQUIRE_FALSE(s.panel().holdsOpen());   // no driver operation started from them
        s.window.activeStatusChanged(false);
        REQUIRE_FALSE(s.window.isVisible());
        REQUIRE(s.panel().page() == MainPanel::Page::Main);
    }
}

TEST_CASE("AppController says whether the loaded config stated the onboarding flag, and states it soon", "[ui][app]") {
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir tmp;
    const auto file = tmp.dir.getChildFile("config.xml");
    const auto atLaunch = [](const AppController& app) {
        return launchGuide(app.onboardingFlagStored(), app.config().onboardingDone);
    };

    SECTION("no config file: the guide opens; the flag is saved soon, so the next launch shows only the reminder") {
        {
            AppController app{appOptions(tmp.dir)};
            REQUIRE_FALSE(app.onboardingFlagStored());
            REQUIRE(atLaunch(app) == LaunchGuide::Open);
            REQUIRE(pumpUntil([&] { return file.existsAsFile(); }, 3000));   // without waiting for the quit
        }
        REQUIRE(file.loadFileAsString().contains("onboardingDone=\"0\""));
        AppController next{appOptions(tmp.dir)};
        REQUIRE(next.onboardingFlagStored());
        REQUIRE(atLaunch(next) == LaunchGuide::Reminder);
    }
    SECTION("a config from before the flag, already set up: nothing opens, and it is stated from now on") {
        REQUIRE(file.replaceWithText("<FixomicConfig version=\"1\" inputDeviceUid=\"no-such-mic\"/>"));
        {
            AppController app{appOptions(tmp.dir)};
            REQUIRE_FALSE(app.onboardingFlagStored());
            REQUIRE(app.config().onboardingDone);
            REQUIRE(atLaunch(app) == LaunchGuide::None);
            REQUIRE(pumpUntil([&] { return file.loadFileAsString().contains("onboardingDone=\"1\""); }, 3000));
        }
        AppController next{appOptions(tmp.dir)};
        REQUIRE(next.onboardingFlagStored());
        REQUIRE(atLaunch(next) == LaunchGuide::None);
    }
}

// A plugin that crashes while the chain is restored would kill every launch (silently, at login).
// The controller marks the restore in the config folder; a marker left by a launch that died there
// makes the next launch restore the chain without loading any plugin.
TEST_CASE("AppController restores the chain without plugins after a launch that died loading them", "[ui][app]") {
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir tmp;
    const auto file = tmp.dir.getChildFile("config.xml");
    const auto marker = tmp.dir.getChildFile(AppController::kRestoreMarkerName);
    {
        fixomic::Config c;
        c.inputDeviceUid = "no-such-mic";
        c.onboardingDone = true;
        fixomic::PluginSlot slot;
        slot.descriptionXml = appleEffect("AUDelay", "dely").createXml()->toString();
        c.chain.push_back(slot);
        REQUIRE(fixomic::ConfigStore(file).save(c));
    }
    const juce::String interrupted = utf8("L’ultimo avvio si è interrotto caricando i plugin: la catena "
                                          "è stata caricata senza plugin.");

    SECTION("a normal launch loads the plugins and leaves no marker") {
        AppController app{appOptions(tmp.dir)};
        REQUIRE(app.slots().size() == 1);
        REQUIRE_FALSE(app.slots()[0].missing);
        REQUIRE(app.startupWarnings().empty());
        REQUIRE_FALSE(marker.exists());
    }
    SECTION("the marker exists while the plugins load (a crash there is detected), and is gone after") {
        int loads = 0;
        bool markerSeenWhileLoading = false;
        auto options = appOptions(tmp.dir);
        options.restoreFactory = [&](const juce::PluginDescription&, double, int,
                                     juce::String& detail) -> std::unique_ptr<juce::AudioPluginInstance> {
            ++loads;
            markerSeenWhileLoading = marker.existsAsFile();
            detail = "test factory";
            return nullptr;
        };
        AppController app{options};
        REQUIRE(loads == 1);
        REQUIRE(markerSeenWhileLoading);
        REQUIRE_FALSE(marker.exists());
        REQUIRE(app.startupWarnings().size() == 1);
        REQUIRE(app.startupWarnings()[0]
                == utf8("AUDelay: non \u00e8 stato possibile caricarlo (test factory)"));
    }
    SECTION("after an interrupted launch: every slot missing (removable), one warning, plugins back next time") {
        REQUIRE(marker.create());
        {
            AppController app{appOptions(tmp.dir)};
            const auto slots = app.slots();
            REQUIRE(slots.size() == 1);
            REQUIRE(slots[0].missing);
            REQUIRE(slots[0].name == "AUDelay");
            REQUIRE(slotDetail(slots[0]) == "Plugin non trovato");
            REQUIRE(app.startupWarnings().size() == 1);
            REQUIRE(app.startupWarnings()[0].startsWith(interrupted));
            REQUIRE_FALSE(marker.exists());   // this launch got through
            app.removePlugin(0);              // the user can take it out
            REQUIRE(app.slots().empty());
            app.saveNow();
        }
        AppController next{appOptions(tmp.dir)};
        REQUIRE(next.slots().empty());   // removed for good
        REQUIRE(next.startupWarnings().empty());
    }
    SECTION("after an interrupted launch the slots keep what was saved: the next launch loads them again") {
        REQUIRE(marker.create());
        { AppController app{appOptions(tmp.dir)}; }   // quits without touching the chain (saves it)
        AppController next{appOptions(tmp.dir)};
        REQUIRE(next.slots().size() == 1);
        REQUIRE_FALSE(next.slots()[0].missing);
        REQUIRE(next.startupWarnings().empty());
    }
}
