#pragma once

#include <functional>
#include <memory>

namespace fixomic::mac {

// Calls back on the main queue whenever the Mac wakes from sleep (NSWorkspaceDidWakeNotification).
// Construct and destroy on the main thread; no callback runs after the destructor.
class WakeObserver {
public:
    explicit WakeObserver(std::function<void()> onWake);
    ~WakeObserver();
    WakeObserver(const WakeObserver&) = delete;
    WakeObserver& operator=(const WakeObserver&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic::mac
