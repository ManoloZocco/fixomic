#pragma once

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

// A peak level bar on the -60..0 dBFS scale, ruled at -40 / -20 / -10 dB, drawn in the ink of the
// state line (the voice's colour). Empty while not live.
class LevelMeter : public juce::Component {
public:
    void setLevel(float peakDb);
    void setInk(StatusKind kind, bool live);
    void paint(juce::Graphics&) override;

    static constexpr float kTicksDb[] = {-40.0f, -20.0f, -10.0f};

private:
    float fraction_ = 0.0f;
    bool clipping_ = false;
    StatusKind kind_ = StatusKind::Waiting;
    bool live_ = false;
};

// The meter scale's numerals, aligned with LevelMeter's rules.
class MeterScale : public juce::Component {
public:
    void paint(juce::Graphics&) override;
};

// The signature: the voice's route Microfono -> Eco -> Catena -> fixomic Mic as a transit line in
// the state's ink; filled stations process, hollow ones are skipped, a barred one is closed.
class RouteStripView : public juce::Component {
public:
    RouteStripView();
    void setRoute(const RouteStrip&);
    void paint(juce::Graphics&) override;

    static constexpr int kHeight = 34;

private:
    RouteStrip route_;
};

// A warning, tinted, with an optional button to the System Settings pane that fixes it.
class NoticeRow : public juce::Component {
public:
    explicit NoticeRow(const Notice&, std::function<void(mac::PrivacyPane)> openPane);
    int heightForWidth(int width) const;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    Notice notice_;
    TextBlock text_{TextStyle::Caption, TextBlock::Ink::Primary};
    juce::TextButton openButton_;
};

// The menu-bar panel, ~360 pt wide, rows in the spec's order: state sentence + global bypass
// switch and the route strip; notices (backend warning, plugins not restored); input / output
// levels; echo cancellation switch + echo removed; the chain section (10b fills it); total latency
// with its breakdown; settings entry (10c) and quit.
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

    // 10b: the chain editor. Takes the place of the empty-state line under "Catena", laid out at
    // the view's own height (set its size; the panel re-lays out when it changes).
    void setChainView(std::unique_ptr<juce::Component>);
    juce::Component* chainView() const;

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
    TextBlock chainPlaceholder_{TextStyle::Caption, TextBlock::Ink::Secondary};   // summary / empty state
    std::unique_ptr<juce::Component> chainView_;

    TextBlock latencyLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock latencyValue_{TextStyle::BodyStrong, TextBlock::Ink::Primary, juce::Justification::topRight};
    TextBlock latencyDetail_{TextStyle::Caption, TextBlock::Ink::Secondary};

    juce::TextButton settingsButton_;
    juce::TextButton quitButton_;
};

// The borderless, rounded, shadowed window the panel lives in, under the menu-bar icon, like the
// system's menu-bar extras: it hides itself on Esc and when it stops being the active window (a
// click elsewhere).
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

    MainPanel panel_;
    std::optional<juce::Rectangle<int>> anchor_;
    double dismissedAt_ = -1.0e9;
};

} // namespace fixomic::ui
