#pragma once

#include <juce_graphics/juce_graphics.h>

#include <optional>

namespace fixomic::mac {

// Where the menu-bar icon is: the screen bounds of an NSStatusItem's button (as returned by
// juce::SystemTrayIconComponent::getNativeHandle()), in JUCE desktop coordinates (points, origin at
// the top left of the main screen). nullopt when there is no item or it is not on screen yet.
// Main thread.
std::optional<juce::Rectangle<int>> statusItemScreenBounds(void* nsStatusItem);

} // namespace fixomic::mac
