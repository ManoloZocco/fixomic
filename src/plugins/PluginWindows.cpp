#include "plugins/PluginWindows.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace fixomic {

class PluginWindows::Window final : public juce::DocumentWindow {
public:
    Window(PluginWindows& owner, juce::AudioPluginInstance& plugin, std::optional<juce::Point<int>> position)
        : juce::DocumentWindow(plugin.getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          owner_(&owner), plugin_(plugin) {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(plugin);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        if (position)
            setTopLeftPosition(*position);
        else
            centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    ~Window() override {
        // The editor goes before the window, while the plugin is still alive.
        clearContentComponent();
    }

    void closeButtonPressed() override {
        setVisible(false);
        // Not from inside our own callback: the owner deletes the window later.
        juce::MessageManager::callAsync([owner = owner_, plugin = &plugin_] {
            if (auto* o = owner.get()) o->closedByUser(plugin);
        });
    }

private:
    juce::WeakReference<PluginWindows> owner_;
    juce::AudioPluginInstance& plugin_;
};

PluginWindows::PluginWindows() = default;
PluginWindows::~PluginWindows() { closeAll(); }

void PluginWindows::open(juce::AudioPluginInstance& plugin) {
    auto& window = windows_[&plugin];
    if (window == nullptr) {
        std::optional<juce::Point<int>> position;
        if (const auto it = positions_.find(&plugin); it != positions_.end()) position = it->second;
        window = std::make_unique<Window>(*this, plugin, position);
    }
    window->setVisible(true);
    juce::Process::makeForegroundProcess();   // a menu-bar app is not frontmost by itself
    window->toFront(true);
}

void PluginWindows::close(juce::AudioPluginInstance& plugin) {
    windows_.erase(&plugin);
    positions_.erase(&plugin);
}

void PluginWindows::closeAll() {
    windows_.clear();
    positions_.clear();
}

bool PluginWindows::isOpen(const juce::AudioPluginInstance& plugin) const {
    const auto it = windows_.find(&plugin);
    return it != windows_.end() && it->second->isVisible();
}

void PluginWindows::closedByUser(juce::AudioPluginInstance* plugin) {
    const auto it = windows_.find(plugin);
    if (it == windows_.end()) return;
    positions_[plugin] = it->second->getPosition();
    windows_.erase(it);
    if (onClosed) onClosed(*plugin);
}

} // namespace fixomic
