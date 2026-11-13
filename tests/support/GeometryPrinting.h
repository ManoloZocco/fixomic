#pragma once

#include <catch2/catch_tostring.hpp>
#include <juce_graphics/juce_graphics.h>

#include <string>

// Failure messages show JUCE points ("(x, y)") and rectangles ("[x y width height]").
namespace Catch {

template <>
struct StringMaker<juce::Point<int>> {
    static std::string convert(juce::Point<int> p) { return "(" + p.toString().toStdString() + ")"; }
};

template <>
struct StringMaker<juce::Rectangle<int>> {
    static std::string convert(juce::Rectangle<int> r) { return "[" + r.toString().toStdString() + "]"; }
};

} // namespace Catch
