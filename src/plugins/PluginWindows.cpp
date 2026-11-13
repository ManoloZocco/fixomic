#include "plugins/PluginWindows.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace fixomic {

class PluginWindows::Window final : public juce::DocumentWindow {
public:
    Window(PluginWindows& owner, juce::AudioPluginInstance& plugin, std::uint64_t order)
        : juce::DocumentWindow(plugin.getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          owner_(&owner), plugin_(plugin), order_(order) {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(plugin);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
    }

    ~Window() override {
        // The editor goes before the window, while the plugin is still alive.
        clearContentComponent();
    }

    std::uint64_t order() const { return order_; }

    // On screen, title bar included.
    juce::Rectangle<int> frame() const { return frameBorder().addedTo(getScreenBounds()); }
    void setFrameTopLeft(juce::Point<int> topLeft) {
        const auto border = frameBorder();
        setTopLeftPosition(topLeft.x + border.getLeft(), topLeft.y + border.getTop());
    }

    void closeButtonPressed() override {
        // Another window may take the focus while this one is still up.
        if (auto* o = owner_.get()) o->closing();
        setVisible(false);
        // Not from inside our own callback: the owner deletes the window later.
        juce::MessageManager::callAsync([owner = owner_, plugin = &plugin_] {
            if (auto* o = owner.get()) o->closedByUser(plugin);
        });
    }

private:
    // The native title bar (and any border) around the content.
    juce::BorderSize<int> frameBorder() const {
        if (auto* peer = getPeer())
            if (const auto border = peer->getFrameSizeIfPresent()) return *border;
        return {};
    }

    juce::WeakReference<PluginWindows> owner_;
    juce::AudioPluginInstance& plugin_;
    const std::uint64_t order_;
};

PluginWindows::PluginWindows() = default;
PluginWindows::~PluginWindows() { closeAll(); }

void PluginWindows::open(juce::AudioPluginInstance& plugin) {
    auto it = windows_.find(&plugin);
    if (it == windows_.end()) {
        const auto previous = lastOpenedFrame();
        auto window = std::make_unique<Window>(*this, plugin, ++opened_);
        const auto frame = window->frame();
        std::optional<juce::Point<int>> placed;
        if (placement) placed = placement({frame.getWidth(), frame.getHeight()}, previous);
        if (placed)
            window->setFrameTopLeft(*placed);
        else if (const auto remembered = positions_.find(&plugin); remembered != positions_.end())
            window->setTopLeftPosition(remembered->second);
        else
            window->centreWithSize(window->getWidth(), window->getHeight());
        it = windows_.emplace(&plugin, std::move(window)).first;
    }
    auto& window = *it->second;
    window.setVisible(true);
    juce::Process::makeForegroundProcess();   // a menu-bar app is not frontmost by itself
    window.toFront(true);
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

std::optional<juce::Rectangle<int>> PluginWindows::lastOpenedFrame() const {
    const Window* last = nullptr;
    for (const auto& entry : windows_)
        if (const auto* window = entry.second.get();
            window->isVisible() && (last == nullptr || window->order() > last->order()))
            last = window;
    return last != nullptr ? std::optional<juce::Rectangle<int>>(last->frame()) : std::nullopt;
}

void PluginWindows::closing() {
    if (onClosing) onClosing();
}

void PluginWindows::closedByUser(juce::AudioPluginInstance* plugin) {
    const auto it = windows_.find(plugin);
    if (it == windows_.end()) return;
    positions_[plugin] = it->second->getPosition();
    windows_.erase(it);
    if (onClosed) onClosed(*plugin);
}

} // namespace fixomic
