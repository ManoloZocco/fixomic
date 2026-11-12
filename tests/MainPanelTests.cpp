#include <catch2/catch_test_macros.hpp>

#include "app/AppController.h"
#include "ui/MainPanel.h"

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

TEST_CASE("Closing the plugin picker gives the keyboard focus back to the window", "[ui][panel]") {
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

TEST_CASE("Escape during a drag cancels the drag; the next one closes the panel", "[ui][panel]") {
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

TEST_CASE("A plugin that cannot be loaded is reported in Italian; why goes to the log", "[ui][panel]") {
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
