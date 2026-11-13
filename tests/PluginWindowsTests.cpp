#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/PluginWindows.h"
#include "support/GeometryPrinting.h"
#include "support/TestPlugin.h"

#include <CoreFoundation/CoreFoundation.h>

#include <optional>
#include <utility>
#include <vector>

using fixomic::PluginWindows;
using fixomic::test::TestPlugin;

namespace {

juce::DocumentWindow* windowFor(const juce::AudioPluginInstance& plugin) {
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(juce::TopLevelWindow::getTopLevelWindow(i)))
            if (w->getName() == plugin.getName() && w->isVisible()) return w;
    return nullptr;
}

// A window's frame on screen: its bounds plus the native title bar.
juce::Rectangle<int> frameOf(juce::Component& window) {
    auto* peer = window.getPeer();
    REQUIRE(peer != nullptr);
    const auto border = peer->getFrameSizeIfPresent();
    REQUIRE(static_cast<bool>(border));
    return (*border).addedTo(window.getScreenBounds());
}

// Services the main run loop until `done` (true) or about `ms` milliseconds passed (false).
bool pumpUntil(const std::function<bool()>& done, int ms = 2000) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (!done() && juce::Time::getMillisecondCounter() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    return done();
}

} // namespace

TEST_CASE("PluginWindows opens one window per instance and closes it before the plugin goes", "[windows]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TestPlugin a("fixomic window test A"), b("fixomic window test B");
    {
        PluginWindows windows;
        windows.open(a);
        windows.open(a);   // brought to front, not duplicated
        windows.open(b);
        REQUIRE(windows.isOpen(a));
        REQUIRE(windows.isOpen(b));
        auto* window = windowFor(a);
        REQUIRE(window != nullptr);
        // TestPlugin has no editor of its own: the generic parameter editor stands in.
        REQUIRE(dynamic_cast<juce::GenericAudioProcessorEditor*>(window->getContentComponent()) != nullptr);

        windows.close(a);
        REQUIRE_FALSE(windows.isOpen(a));
        REQUIRE(windowFor(a) == nullptr);
        REQUIRE(windows.isOpen(b));
    }   // destructor closes b
    REQUIRE(windowFor(b) == nullptr);
}

TEST_CASE("PluginWindows asks where a new window goes, and tells when the user is closing one", "[windows]") {
    juce::ScopedJuceInitialiser_GUI juce;
    TestPlugin a("fixomic window test C"), b("fixomic window test D");
    PluginWindows windows;
    std::vector<std::pair<juce::Point<int>, std::optional<juce::Rectangle<int>>>> asked;
    windows.placement = [&](juce::Point<int> size,
                            std::optional<juce::Rectangle<int>> previous) -> std::optional<juce::Point<int>> {
        asked.emplace_back(size, previous);
        return juce::Point<int>(100 + 30 * static_cast<int>(asked.size()), 200);
    };

    windows.open(a);
    REQUIRE(asked.size() == 1);
    REQUIRE_FALSE(asked[0].second.has_value());   // no other window open
    REQUIRE(windowFor(a) != nullptr);
    const auto first = frameOf(*windowFor(a));
    // The hook places the frame, title bar included, and was given the frame's size.
    REQUIRE(first.getPosition() == juce::Point<int>(130, 200));
    REQUIRE(first.getWidth() == asked[0].first.x);
    REQUIRE(first.getHeight() == asked[0].first.y);
    REQUIRE(first.getHeight() > windowFor(a)->getHeight());

    windows.open(a);   // already open: to the front, not asked again, not moved
    REQUIRE(asked.size() == 1);
    REQUIRE(frameOf(*windowFor(a)) == first);

    windows.open(b);
    REQUIRE(asked.size() == 2);
    REQUIRE(asked[1].second == std::optional<juce::Rectangle<int>>(first));   // the one opened before it
    REQUIRE(frameOf(*windowFor(b)).getPosition() == juce::Point<int>(160, 200));

    SECTION("closed with the title-bar button: onClosing while it is still up, onClosed once it is gone") {
        int closing = 0, closed = 0;
        bool upWhileClosing = false;
        windows.onClosing = [&] {
            ++closing;
            upWhileClosing = windowFor(b) != nullptr;
        };
        windows.onClosed = [&](juce::AudioPluginInstance& plugin) {
            REQUIRE(&plugin == &b);
            ++closed;
        };
        windowFor(b)->getPeer()->handleUserClosingWindow();
        REQUIRE(closing == 1);
        REQUIRE(upWhileClosing);
        REQUIRE(windowFor(b) == nullptr);   // hidden at once
        REQUIRE(pumpUntil([&] { return closed == 1; }));
        REQUIRE(closing == 1);
        REQUIRE_FALSE(windows.isOpen(b));
        REQUIRE(windows.isOpen(a));

        // The hook declines: today's default, here the position b was closed at.
        windows.placement = [](juce::Point<int>, std::optional<juce::Rectangle<int>>) {
            return std::optional<juce::Point<int>>();
        };
        windows.open(b);
        REQUIRE(frameOf(*windowFor(b)).getPosition() == juce::Point<int>(160, 200));
    }
    SECTION("closed by the app (a slot removed): no onClosing") {
        int closing = 0;
        windows.onClosing = [&] { ++closing; };
        windows.close(b);
        REQUIRE(windowFor(b) == nullptr);
        REQUIRE(closing == 0);
    }
}
