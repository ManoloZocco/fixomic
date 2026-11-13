#pragma once

#include <functional>
#include <memory>

namespace fixomic::mac {

// Calls back when this app becomes or stops being the active (foreground) app:
// NSApplicationDidBecomeActiveNotification, NSApplicationDidResignActiveNotification. AppKit posts
// them on the main thread and the callback runs there, inside the post (no queue), so none runs
// after the destructor. JUCE has no public hook for this; its own focus tracking only follows the
// key window. Construct and destroy on the main thread.
class AppActivationObserver {
public:
    explicit AppActivationObserver(std::function<void(bool active)> onChange);
    ~AppActivationObserver();
    AppActivationObserver(const AppActivationObserver&) = delete;
    AppActivationObserver& operator=(const AppActivationObserver&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic::mac
