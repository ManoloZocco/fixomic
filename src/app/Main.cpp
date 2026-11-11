#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace {

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

class TrayIcon final : public juce::SystemTrayIconComponent {
public:
    TrayIcon() {
        setIconImage(makeTrayIcon(), makeTrayIcon());
        setIconTooltip("fixomic");
    }

    void mouseDown(const juce::MouseEvent&) override {
        juce::PopupMenu menu;
        menu.addItem("Esci", [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
        menu.showMenuAsync(juce::PopupMenu::Options{});
    }
};

class FixomicApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override { tray_ = std::make_unique<TrayIcon>(); }
    void shutdown() override { tray_.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<TrayIcon> tray_;
};

} // namespace

START_JUCE_APPLICATION(FixomicApplication)
