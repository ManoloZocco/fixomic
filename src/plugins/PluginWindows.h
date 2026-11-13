#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace fixomic {

// Editor windows of hosted plugins: at most one per instance. open() brings an existing window to
// the front, where it is; a plugin without its own editor gets JUCE's generic parameter editor. A new
// window goes where `placement` says (the menu-bar panel's window puts it beside the panel), else at
// the position it was closed at (remembered in memory only, per instance), else centred.
//
// Windows must be closed before their plugin is destroyed: call close() before removing a plugin
// from the chain, closeAll() before replacing or destroying the chain.
//
// Threading: message thread only.
class PluginWindows {
public:
    PluginWindows();
    ~PluginWindows();   // closes every window
    PluginWindows(const PluginWindows&) = delete;
    PluginWindows& operator=(const PluginWindows&) = delete;

    void open(juce::AudioPluginInstance&);
    void close(juce::AudioPluginInstance&);   // no-op if not open; forgets the position
    void closeAll();                          // forgets every position
    bool isOpen(const juce::AudioPluginInstance&) const;

    // Where a new window opens: given the size of its frame (title bar included) and the frame of the
    // window opened last that is still open (none: nullopt), the top-left of its frame, in screen
    // coordinates; nullopt (or no hook) for the default, the remembered position or centred. Not
    // asked for a window that is already open.
    using Placement = std::function<std::optional<juce::Point<int>>(juce::Point<int> frameSize,
                                                                    std::optional<juce::Rectangle<int>> previousFrame)>;
    Placement placement;

    // The user is closing a window (title-bar button). Called before it hides, so that another of
    // the app's windows can take the keyboard focus first (the app stays the active one); it must not
    // open or close windows. Not called for close() and closeAll().
    std::function<void()> onClosing;

    // The user closed a window (title-bar button). Message thread, after the window is gone.
    std::function<void(juce::AudioPluginInstance&)> onClosed;

private:
    class Window;
    void closing();
    void closedByUser(juce::AudioPluginInstance*);
    std::optional<juce::Rectangle<int>> lastOpenedFrame() const;   // of the windows on screen

    std::map<const juce::AudioPluginInstance*, std::unique_ptr<Window>> windows_;
    std::map<const juce::AudioPluginInstance*, juce::Point<int>> positions_;
    std::uint64_t opened_ = 0;   // windows opened so far: their order
    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginWindows)
};

} // namespace fixomic
