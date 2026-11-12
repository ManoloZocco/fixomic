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
// "Esci". The icon and the panel follow the model through modelChanged() (call it from
// AppController::onModelChanged); the only timer is a one-shot deadline for the DriverMissing hold.
// Message thread only.
class TrayIcon : public juce::SystemTrayIconComponent, private juce::Timer {
public:
    explicit TrayIcon(AppController&);
    ~TrayIcon() override;

    void modelChanged();
    const StatusPresentation& status() const;

    void showPanel();
    void hidePanel();
    void togglePanel();
    bool isPanelShowing() const;
    MainPanel& panel();

    std::function<void()> onQuit;           // "Esci", from the menu or the panel
    std::function<void()> onOpenSettings;   // the panel's settings entry (10c)

    // The template image of a state: 20 x 20 pt drawn at 2x, black on transparent (macOS tints it).
    static juce::Image iconImage(StatusKind);

    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void refreshStatus();
    void showMenu();

    AppController& app_;
    StatusModel model_;
    std::optional<StatusKind> iconShown_;
    MainPanelWindow window_;
};

// The four icons side by side with their names, for the --snapshot developer command.
std::unique_ptr<juce::Component> createTrayIconSheet();

} // namespace fixomic::ui
