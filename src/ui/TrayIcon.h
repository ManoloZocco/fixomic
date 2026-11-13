#pragma once

#include "ui/MainPanel.h"
#include "ui/StatusModel.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

namespace fixomic {
class AppController;
}

namespace fixomic::ui {

// fixomic's menu-bar extra: the status icon in one of four states (active, waiting, bypass,
// problem), the panel it opens, and the StatusModel both of them present.
//
// Left click toggles the panel; right click (or control-click) shows a menu with the state and
// "Esci" (see showMenu()). The icon and the panel follow the model through modelChanged() (call it
// from AppController::onModelChanged). Two one-shot timers, no polling: the deadline of the
// DriverMissing hold and the short delay before the menu opens. Message thread only.
class TrayIcon : public juce::SystemTrayIconComponent, private juce::Timer {
public:
    explicit TrayIcon(AppController&);
    ~TrayIcon() override;

    void modelChanged();
    const StatusPresentation& status() const;

    void showPanel();
    // The panel opened on the first-run guide (at the first launch, until it is done) or on the
    // settings.
    void showOnboarding();
    void showSettings();
    void hidePanel();
    void togglePanel();
    bool isPanelShowing() const;
    MainPanel& panel();

    // The right-click menu: the state and "Esci". The app is made active first and the menu opens
    // kMenuDelayMs later: a JUCE PopupMenu dismisses itself on its first timer tick when the app is
    // not the active one (an LSUIElement app is not, when its status item is clicked).
    void showMenu();
    bool isMenuShowing() const;   // opened and not dismissed yet
    static constexpr int kMenuDelayMs = 50;

    std::function<void()> onQuit;              // "Esci", from the menu or the panel
    std::function<void(bool open)> onMenuChanged;   // the menu opened / closed (diagnostics)

    // The template image of a state: 20 x 20 pt drawn at 2x, black on transparent (macOS tints it).
    static juce::Image iconImage(StatusKind);

    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void refreshStatus();
    void openMenu();
    void menuClosed();

    AppController& app_;
    StatusModel model_;
    std::optional<StatusKind> iconShown_;
    bool menuShowing_ = false;
    juce::TimedCallback menuTimer_{[this] { openMenu(); }};   // one-shot: kMenuDelayMs after showMenu()
    MainPanelWindow window_;
};

// The four icons side by side with their names, for the --snapshot developer command.
std::unique_ptr<juce::Component> createTrayIconSheet();

} // namespace fixomic::ui
