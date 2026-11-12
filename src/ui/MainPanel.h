#pragma once

#include "ui/ChainList.h"
#include "ui/PanelParts.h"
#include "ui/PluginPicker.h"
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
// switch and the route strip; notices (backend warning, plugins not restored); input / output
// levels; echo cancellation switch + echo removed; the chain section; total latency with its
// breakdown; settings entry (10c) and quit.
//
// The chain section is "Catena" over the ChainList, or the PluginPicker in its place while a plugin
// is being added ("+ Aggiungi plugin"; back, Escape or a plugin added return to the list). Chain
// edits go to the AppController, and the section re-reads the chain right after each one
// (asynchronously, never from inside the row that asked).
//
// Refresh: refresh() re-reads the model on AppController::onModelChanged; a 30 Hz timer updates
// only the meters (and the echo-removed value, the AEC's meter), and only while setLive(true).
// Message thread only.
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

    std::function<void()> onOpenSettings;           // 10c; until then the button does nothing visible
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

    AppController& app_;
    StatusPresentation status_;
    int lastPreferredHeight_ = 0;
    std::vector<int> separators_;   // y of each hairline, from the last layout

    TextBlock statusText_{TextStyle::Title, TextBlock::Ink::Primary};
    TextBlock bypassLabel_{TextStyle::Caption, TextBlock::Ink::Secondary, juce::Justification::centredRight};
    juce::ToggleButton bypassSwitch_;
    RouteStripView route_;

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
};

// Where the panel window goes: centred under `anchor` (the menu-bar icon; under `mouse` when
// unknown), kept 8 pt inside `userArea` horizontally, 6 pt below the menu bar, at `preferredHeight`
// unless that would run past the bottom of `userArea` (8 pt margin): then clamped, and the panel
// scrolls inside the window.
juce::Rectangle<int> panelWindowBounds(std::optional<juce::Rectangle<int>> anchor, juce::Point<int> mouse,
                                       juce::Rectangle<int> userArea, int width, int preferredHeight);

// The borderless, rounded, shadowed window the panel lives in, under the menu-bar icon, like the
// system's menu-bar extras: it hides itself on Esc (an Esc first cancels a drag in the chain list,
// then closes the plugin picker, if either is going on) and when it stops being the active window
// (a click elsewhere); it reopens on the chain list.
// Never taller than the display (panelWindowBounds): a longer panel (a long chain) scrolls.
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

    static constexpr double kReopenGuardMs = 300.0;

    int getDesktopWindowStyleFlags() const override;
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override;

private:
    void activeWindowStatusChanged() override;
    void place();
    void fitPanel();   // the panel at its preferred height

    MainPanel panel_;
    juce::Viewport viewport_;
    std::optional<juce::Rectangle<int>> anchor_;
    double dismissedAt_ = -1.0e9;
};

} // namespace fixomic::ui
