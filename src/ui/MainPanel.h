#pragma once

#include "platform/mac/AppActivation.h"
#include "ui/ChainList.h"
#include "ui/Onboarding.h"
#include "ui/PanelParts.h"
#include "ui/PluginPicker.h"
#include "ui/SettingsPanel.h"
#include "ui/StatusModel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace fixomic {
class AppController;
}

namespace fixomic::ui {

// The menu-bar panel, ~360 pt wide, rows in the spec's order: state sentence + global bypass
// switch and the route strip; notices ("Completa la configurazione" while the first-run guide is not
// done, the backend warning, plugins not restored); input / output levels; echo cancellation switch
// + echo removed; the chain section; total latency with its breakdown; "Impostazioni…" and quit.
//
// The chain section is "Catena" over the ChainList, or the PluginPicker in its place while a plugin
// is being added ("+ Aggiungi plugin"; back, Escape or a plugin added return to the list). Chain
// edits go to the AppController, and the section re-reads the chain right after each one
// (asynchronously, never from inside the row that asked).
//
// Two views take the whole panel's place, each with its own back control: the SettingsPanel
// ("Impostazioni…") and the Onboarding (on first launch, from the reminder row, from the settings'
// "Rivedi la configurazione iniziale"). Their intents go to the AppController (PanelPages); "Fine" sets
// Config.onboardingDone, closing the guide early leaves it false.
//
// Refresh: refresh() re-reads the model on AppController::onModelChanged (and the open view's data);
// a 30 Hz timer updates only the meters (and the echo-removed value, the AEC's meter), and only while
// setLive(true). Message thread only.
class MainPanel : public juce::Component, private juce::Timer {
public:
    static constexpr int kWidth = 360;
    static constexpr int kMeterHz = 30;

    explicit MainPanel(AppController&);
    ~MainPanel() override;

    // Shows `status` (from the tray icon's StatusModel) and re-reads config, warnings, chain and
    // latency.
    void refresh(const StatusPresentation& status);
    void setLive(bool);        // meters running: only while the panel is on screen
    bool isLive() const;
    void updateMeters();       // one read of the meters
    int preferredHeight(int width) const;

    // The plugin picker in place of the chain list (it takes the keyboard focus when on screen),
    // and back. closePicker() returns whether it was open.
    void showPicker();
    bool closePicker();
    bool isPickerOpen() const;

    // The chain section's views. The snapshot command fills them with demo data.
    ChainList& chainList();
    PluginPicker& picker();
    // Shows `slots` in the chain list and the route, and `latency` in the latency row, instead of
    // the AppController's (the snapshot command's demo chain); the next refresh() shows the app's.
    void showChain(std::vector<PluginChain::SlotInfo> slots, const LatencyBreakdown& latency);

    // The views in the panel's place. closePage() goes one step back (the guide to where it was
    // opened from, the settings to the panel) and returns whether one was open.
    enum class Page { Main, Settings, Onboarding };
    void showSettings();
    void showOnboarding();
    bool closePage();
    Page page() const;
    SettingsPanel& settings();
    Onboarding& onboarding();
    // Re-reads the open view's data (devices, permissions, driver, login item): on every refresh(),
    // and when the window becomes active again (a prompt or System Settings may have changed them).
    void refreshPage();
    // Stay up when the app loses focus (to a system prompt, the administrator dialog, System Settings
    // opened from the panel): while the guide is open, or, on the settings, while a driver operation
    // started there runs or the microphone prompt is up.
    bool holdsOpen() const;
    bool showsSetupReminder() const;   // the "Completa la configurazione" row

    std::function<void()> onQuit;
    std::function<void()> onPreferredHeightChanged; // the window follows preferredHeight()

    void paint(juce::Graphics&) override;
    void resized() override;
    void childBoundsChanged(juce::Component*) override;

private:
    struct Layout;
    Layout layOut(int width) const;
    void timerCallback() override;
    void rebuildNotices(const std::vector<Notice>&);
    void heightMayHaveChanged();
    void refreshChain();       // the chain (and the catalog, while picking) from the AppController
    void refreshChainSoon();   // the same, once the current event handler has returned
    void scrollIntoView(const juce::Component&);
    void updateRoute();                          // from status_ and the chain list's slots
    void showLatency(const LatencyBreakdown&);
    void setPage(Page);
    void updateVisibility();                     // the page's components, the picker or the list
    void releaseFocus(juce::Component& leaving); // to the window, before `leaving` hides

    AppController& app_;
    StatusPresentation status_;
    int lastPreferredHeight_ = 0;
    std::vector<int> separators_;   // y of each hairline, from the last layout

    TextBlock statusText_{TextStyle::Title, TextBlock::Ink::Primary};
    TextBlock bypassLabel_{TextStyle::Caption, TextBlock::Ink::Secondary, juce::Justification::centredRight};
    juce::ToggleButton bypassSwitch_;
    RouteStripView route_;

    std::unique_ptr<NoticeRow> setupRow_;   // "Completa la configurazione"
    bool setupReminder_ = false;
    std::vector<Notice> notices_;
    std::vector<std::unique_ptr<NoticeRow>> noticeRows_;

    TextBlock inputLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock outputLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    LevelMeter inputMeter_, outputMeter_;
    MeterScale meterScale_;

    TextBlock aecLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    juce::ToggleButton aecSwitch_;
    TextBlock erleLabel_{TextStyle::Caption, TextBlock::Ink::Secondary};
    TextBlock erleValue_{TextStyle::Caption, TextBlock::Ink::Primary, juce::Justification::topRight};

    TextBlock chainTitle_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    ChainList chainList_;
    PluginPicker picker_;
    bool pickerOpen_ = false;

    TextBlock latencyLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock latencyValue_{TextStyle::BodyStrong, TextBlock::Ink::Primary, juce::Justification::topRight};
    TextBlock latencyDetail_{TextStyle::Caption, TextBlock::Ink::Secondary};

    juce::TextButton settingsButton_;
    juce::TextButton quitButton_;

    SettingsPanel settings_;
    Onboarding onboarding_;
    Page page_ = Page::Main;
    Page onboardingBack_ = Page::Main;   // where the guide's back control goes
};

// Where the panel window goes: centred under `anchor` (the menu-bar icon; under `mouse` when
// unknown), kept 8 pt inside `userArea` horizontally, 6 pt below the menu bar, at `preferredHeight`
// unless that would run past the bottom of `userArea` (8 pt margin): then clamped, and the panel
// scrolls inside the window.
juce::Rectangle<int> panelWindowBounds(std::optional<juce::Rectangle<int>> anchor, juce::Point<int> mouse,
                                       juce::Rectangle<int> userArea, int width, int preferredHeight);

// Where a plugin editor window opens while the panel window is on screen at `panel`: the top-left of
// the editor's frame (title bar included) of `frameSize`, beside the panel, inside the user area of
// the panel's display (of `userAreas`, one per display, the one holding most of the panel; at least
// one). At the panel's left, 8 pt apart; if it does not fit there, at its right; if it fits on neither
// side, at the user area's left edge (over the panel). Its top is the panel's; a further editor's
// (`previousTop`: the top of the editor opened before it, still open) 24 pt below that one's. Always
// clamped vertically inside the user area (its title bar at the top when it is taller).
juce::Point<int> editorBesidePanel(juce::Rectangle<int> panel, const std::vector<juce::Rectangle<int>>& userAreas,
                                   juce::Point<int> frameSize, std::optional<int> previousTop);

// Whether the panel window, on screen, stays open when it stops being the active window or fixomic
// stops being the foreground app: while fixomic is still the foreground app (the focus went to one of
// its plugin editors, or came back from one), and while the panel holds itself open
// (MainPanel::holdsOpen(): the administrator dialog and the microphone prompt belong to other
// processes, so the app is not the foreground one then).
bool panelStaysOpen(bool appActive, bool heldOpen);

// The borderless, rounded, shadowed window the panel lives in, under the menu-bar icon, like the
// system's menu-bar extras. It hides itself on Esc (an Esc first cancels a drag in the chain list,
// then closes the plugin picker, then the settings or the guide, if any is going on) and when fixomic
// stops being the foreground app (a click on another app or on the desktop, even while a plugin
// editor has the focus), unless the panel holds it open (panelStaysOpen); the focus going to one of
// fixomic's plugin editors and back leaves it open. It reopens on the chain list. Becoming active
// again re-reads the open view's data. Never taller than the display (panelWindowBounds): a longer
// panel (a long chain) scrolls.
//
// Plugin editors (through the AppController's editor hooks; one panel window per AppController): one
// opened while the panel is on screen goes beside it (editorBesidePanel), one opened while it is not
// goes where PluginWindows puts it by default. Closing an editor with its title-bar button while the
// panel is on screen gives the panel the keyboard focus back, before the editor hides.
//
// The app leaving the foreground while an editor, not the panel, is the key window does not reach
// activeWindowStatusChanged() (the panel is no longer active by then): an AppActivationObserver
// reports it to appActiveChanged().
class MainPanelWindow : public juce::TopLevelWindow {
public:
    explicit MainPanelWindow(AppController&);
    ~MainPanelWindow() override;

    MainPanel& panel();
    // Shows the panel centred under `anchor` (the menu-bar icon's screen bounds; under the mouse
    // when unknown), activates the app and takes the keyboard focus.
    void showUnder(std::optional<juce::Rectangle<int>> anchor);
    void dismiss();
    // A click on the icon that made the panel lose focus arrives right after the dismissal: it
    // must close the panel, not reopen it.
    bool dismissedJustNow() const;
    // The window became active (re-read the open view's data) or stopped being it, with fixomic still
    // the foreground app or not (hide, unless panelStaysOpen). Called by activeWindowStatusChanged().
    void activeStatusChanged(bool windowActive, bool appActive);
    // fixomic became or stopped being the foreground app (hide, unless panelStaysOpen). Called by the
    // AppActivationObserver.
    void appActiveChanged(bool active);

    static constexpr double kReopenGuardMs = 300.0;

    int getDesktopWindowStyleFlags() const override;
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override;

private:
    void activeWindowStatusChanged() override;
    void place();
    void fitPanel();   // the panel at its preferred height
    // The editor hooks: where a new editor opens (nullopt while the panel is not on screen), and the
    // user closing one.
    std::optional<juce::Point<int>> editorPosition(juce::Point<int> frameSize,
                                                   std::optional<juce::Rectangle<int>> previousFrame) const;
    void editorClosing();

    AppController& app_;
    MainPanel panel_;
    juce::Viewport viewport_;
    std::optional<juce::Rectangle<int>> anchor_;
    double dismissedAt_ = -1.0e9;
    mac::AppActivationObserver activation_{[this](bool active) { appActiveChanged(active); }};   // last: goes first
};

} // namespace fixomic::ui
