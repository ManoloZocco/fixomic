#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>
#include <optional>
#include <memory>

namespace fixomic {

// Editor windows of hosted plugins: at most one per instance. open() brings an existing window to
// the front; a plugin without its own editor gets JUCE's generic parameter editor. The position of
// a closed window is remembered (in memory only) and reused when the same instance is reopened.
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

    // The user closed a window (title-bar button). Message thread, after the window is gone.
    std::function<void(juce::AudioPluginInstance&)> onClosed;

private:
    class Window;
    void closedByUser(juce::AudioPluginInstance*);

    std::map<const juce::AudioPluginInstance*, std::unique_ptr<Window>> windows_;
    std::map<const juce::AudioPluginInstance*, juce::Point<int>> positions_;
    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginWindows)
};

} // namespace fixomic
