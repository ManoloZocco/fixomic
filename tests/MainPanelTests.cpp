#include <catch2/catch_test_macros.hpp>

#include "app/AppController.h"
#include "ui/ChainModel.h"
#include "ui/MainPanel.h"
#include "ui/SetupModel.h"
#include "core/Config.h"
#include "core/ConfigStore.h"
#include "support/GeometryPrinting.h"
#include "support/TestPlugin.h"

#include <CoreFoundation/CoreFoundation.h>

#include <functional>
#include <vector>

using fixomic::AppController;
using fixomic::test::TestPlugin;
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

// A chain of two TestPlugins ("fixomic editor A", "fixomic editor B"), restored at startup: their
// generic editors open in this process, at a known size.
AppController::Options withTestPlugins(const juce::File& dir) {
    fixomic::Config c;
    c.onboardingDone = true;
    for (const char* name : {"fixomic editor A", "fixomic editor B"}) {
        fixomic::PluginSlot slot;
        slot.descriptionXml = TestPlugin(name).getPluginDescription().createXml()->toString();
        c.chain.push_back(slot);
    }
    REQUIRE(fixomic::ConfigStore(dir.getChildFile("config.xml")).save(c));
    auto o = appOptions(dir);
    o.restoreFactory = [](const juce::PluginDescription& d, double, int,
                          juce::String&) -> std::unique_ptr<juce::AudioPluginInstance> {
        return std::make_unique<TestPlugin>(d.name);
    };
    return o;
}

// The visible editor window of the plugin named `name`, if any.
juce::DocumentWindow* editorWindow(const juce::String& name) {
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(juce::TopLevelWindow::getTopLevelWindow(i)))
            if (w->getName() == name && w->isVisible()) return w;
    return nullptr;
}

// A window's frame on screen: its bounds plus the native title bar, if it has one.
juce::Rectangle<int> frameOf(juce::Component& window) {
    auto* peer = window.getPeer();
    REQUIRE(peer != nullptr);
    const auto border = peer->getFrameSizeIfPresent();
    REQUIRE(static_cast<bool>(border));
    return (*border).addedTo(window.getScreenBounds());
}

// A click on a chain row's name: the chain list's open-editor path.
void clickName(ChainList& list, int index) {
    auto& row = *list.row(index);
    const juce::Point<float> at{30.0f, ChainList::kRowHeight / 2.0f};
    row.mouseDown(mouseAt(row, at, at, false));
    row.mouseUp(mouseAt(row, at, at, false));
}

struct PanelOnScreen {
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir tmp;
    AppController app;
    MainPanelWindow window{app};

    explicit PanelOnScreen(AppController::Options (*options)(const juce::File&) = appOptions) : app(options(tmp.dir)) {
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
        s.window.activeStatusChanged(false, false);
        REQUIRE(s.window.isVisible());
        REQUIRE(s.panel().page() == MainPanel::Page::Onboarding);
        s.window.activeStatusChanged(true, true);   // back: re-read, still open
        REQUIRE(s.window.isVisible());
    }
    SECTION("the settings and the panel: dismissed, as menu-bar extras are; reopened on the panel") {
        s.panel().showSettings();
        REQUIRE_FALSE(s.panel().holdsOpen());   // no driver operation started from them
        s.window.activeStatusChanged(false, false);
        REQUIRE_FALSE(s.window.isVisible());
        REQUIRE(s.panel().page() == MainPanel::Page::Main);
    }
    SECTION("the focus went to another fixomic window (a plugin editor), the app still active: kept open") {
        s.window.activeStatusChanged(false, true);
        REQUIRE(s.window.isVisible());
    }
}

// The editor windows are real ones on screen, the TestPlugins' generic editors. Focus changes are
// reported by JUCE only to a foreground app (not to this test process): the window's response to
// them is driven directly, as the app would see them.
TEST_CASE("A plugin editor opens beside the panel, which stays open with the chain", "[ui][panel][gui]") {
    PanelOnScreen s{withTestPlugins};
    // At the right of the main display, as under a menu-bar icon: the editor fits at its left.
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    REQUIRE(display != nullptr);
    const auto userArea = display->userBounds.getSmallestIntegerContainer();
    s.window.setTopLeftPosition(userArea.getRight() - MainPanel::kWidth - 40, userArea.getY() + 6);
    auto& list = s.panel().chainList();
    REQUIRE(list.slots().size() == 2);
    REQUIRE(list.slots()[0].name == "fixomic editor A");
    const auto panel = s.window.getScreenBounds();

    clickName(list, 0);
    pumpMessages(200);
    auto* editor = editorWindow("fixomic editor A");
    REQUIRE(editor != nullptr);
    REQUIRE(s.window.isVisible());
    REQUIRE(s.window.getScreenBounds() == panel);
    REQUIRE_FALSE(s.window.hasKeyboardFocus(true));   // the editor took it

    // At the panel's left, 8 pt apart, the tops (the editor's title bar) aligned.
    const auto frame = frameOf(*editor);
    CHECK_FALSE(frame.intersects(panel));
    CHECK(frame.getRight() + 8 == panel.getX());
    CHECK(frame.getY() == panel.getY());

    SECTION("the editor has the focus, fixomic is still the foreground app: the panel stays") {
        s.window.activeStatusChanged(false, true);   // as the app sees the editor take the focus
        REQUIRE(s.window.isVisible());
    }
    SECTION("a second editor: 24 pt below the first, still beside the panel; the first one stays put") {
        clickName(list, 1);
        pumpMessages(200);
        auto* second = editorWindow("fixomic editor B");
        REQUIRE(second != nullptr);
        const auto secondFrame = frameOf(*second);
        REQUIRE(secondFrame.getRight() + 8 == panel.getX());
        REQUIRE(secondFrame.getY() == frame.getY() + 24);
        REQUIRE(frameOf(*editor) == frame);
        clickName(list, 0);   // already open: to the front, not moved
        pumpMessages(100);
        REQUIRE(frameOf(*editor) == frame);
        REQUIRE(s.window.isVisible());
    }
    SECTION("reordered while the editor is open: the slot moves, its editor stays open where it is") {
        auto& row = *list.row(0);
        const juce::Point<float> down{30.0f, 16.0f};
        row.mouseDown(mouseAt(row, down, down, false));
        row.mouseDrag(mouseAt(row, {30.0f, 30.0f}, down, true));
        row.mouseDrag(mouseAt(row, {30.0f, 62.0f}, down, true));   // under the second row
        row.mouseUp(mouseAt(row, {30.0f, 62.0f}, down, true));
        REQUIRE(pumpUntil([&] { return list.slots()[0].name == "fixomic editor B"; }));
        REQUIRE(s.app.slots()[1].name == "fixomic editor A");
        REQUIRE(editorWindow("fixomic editor A") == editor);
        REQUIRE(frameOf(*editor) == frame);
        REQUIRE(s.window.isVisible());
    }
    SECTION("its slot removed: the editor closes, the panel stays") {
        auto* remove = findChild<juce::Button>(*list.row(0), [](auto& b) { return b.getButtonText() == "Rimuovi fixomic editor A"; });
        REQUIRE(remove != nullptr);
        remove->onClick();
        REQUIRE(editorWindow("fixomic editor A") == nullptr);
        REQUIRE(pumpUntil([&] { return list.slots().size() == 1; }));
        REQUIRE(s.window.isVisible());
    }
    SECTION("closed with its close button: the panel stays open with the keyboard focus, Escape closes it") {
        editor->getPeer()->handleUserClosingWindow();   // what the title bar's close button does
        pumpMessages(100);
        REQUIRE(editorWindow("fixomic editor A") == nullptr);
        REQUIRE(s.window.isVisible());
        REQUIRE(juce::Component::getCurrentlyFocusedComponent() == &s.window);
        s.press(juce::KeyPress::escapeKey);
        REQUIRE_FALSE(s.window.isVisible());
    }
    SECTION("fixomic stops being the foreground app (the app-activation seam): the panel goes, the editor stays") {
        s.window.appActiveChanged(false);
        REQUIRE_FALSE(s.window.isVisible());
        REQUIRE(editorWindow("fixomic editor A") == editor);
    }
    SECTION("... unless the panel holds itself open: another process's prompt is up") {
        s.panel().showOnboarding();
        s.window.appActiveChanged(false);
        REQUIRE(s.window.isVisible());
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
