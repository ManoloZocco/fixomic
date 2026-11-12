#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/PluginWindows.h"
#include "support/TestPlugin.h"

using fixomic::PluginWindows;
using fixomic::test::TestPlugin;

namespace {

juce::DocumentWindow* windowFor(const juce::AudioPluginInstance& plugin) {
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(juce::TopLevelWindow::getTopLevelWindow(i)))
            if (w->getName() == plugin.getName() && w->isVisible()) return w;
    return nullptr;
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
