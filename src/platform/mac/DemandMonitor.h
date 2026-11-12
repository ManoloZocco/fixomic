#pragma once

#include <functional>
#include <memory>

namespace fixomic::mac {

// Tells whether some process (any client, this one included) is reading fixomic Mic: polls
// kAudioDevicePropertyDeviceIsRunningSomewhere of the device every kPollMilliseconds (the property
// validated in Task 5; IsRunning stays 0 for other processes' IO). The UID is resolved on every
// poll, so a reinstalled driver is picked up; no fixomic Mic counts as not in use.
//
// The callback runs on the main queue, only when the value changes (the initial value is "not in
// use": start() polls at once and reports `true` right away if a reader is already there).
// Threading: construct, start(), stop() and destroy on the main thread; no callback runs after
// stop() or the destructor.
class DemandMonitor {
public:
    explicit DemandMonitor(std::function<void(bool inUse)>);
    ~DemandMonitor();
    DemandMonitor(const DemandMonitor&) = delete;
    DemandMonitor& operator=(const DemandMonitor&) = delete;

    void start();   // no-op when already started
    void stop();    // no-op when stopped; does not report a final `false`

    bool inUse() const;   // last value reported (false before the first poll)

    static constexpr int kPollMilliseconds = 200;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic::mac
