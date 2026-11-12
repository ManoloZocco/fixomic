#include "platform/mac/StatusItem.h"

#import <AppKit/AppKit.h>

#include <cmath>

namespace fixomic::mac {

std::optional<juce::Rectangle<int>> statusItemScreenBounds(void* nsStatusItem)
{
    if (nsStatusItem == nullptr)
        return std::nullopt;
    NSStatusItem* item = (__bridge NSStatusItem*) nsStatusItem;
    NSWindow* window = item.button.window;
    NSScreen* mainScreen = NSScreen.screens.firstObject;   // the screen of Cocoa's origin
    if (window == nil || mainScreen == nil)
        return std::nullopt;
    const NSRect frame = window.frame;   // bottom-left origin
    const double top = NSMaxY(mainScreen.frame) - NSMaxY(frame);
    return juce::Rectangle<int>(static_cast<int>(std::lround(frame.origin.x)), static_cast<int>(std::lround(top)),
                                static_cast<int>(std::lround(frame.size.width)),
                                static_cast<int>(std::lround(frame.size.height)));
}

} // namespace fixomic::mac
