#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace fixomic::ui {

// A component to render, laid out at its final size, saved as <name>-light.png / <name>-dark.png.
// It must not have a LookAndFeel of its own: the forced theme is set on it and cleared afterwards.
struct SnapshotTarget {
    juce::String name;
    juce::Component* component = nullptr;
};

// Renders every target with Component::createComponentSnapshot at 2x scale, once with the theme
// forced to light and once to dark, into `directory` (created if needed). Draws in-process: no
// Screen Recording permission involved. Returns the files written; on failure stops and sets
// `error`. Message thread.
std::vector<juce::File> writeSnapshots(const juce::File& directory, const std::vector<SnapshotTarget>& targets,
                                       juce::String& error);

} // namespace fixomic::ui
