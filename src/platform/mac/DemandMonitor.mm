#include "platform/mac/DemandMonitor.h"

#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/CoreAudioUtil.h"
#include "platform/mac/FixomicDevices.h"

#include <dispatch/dispatch.h>

namespace fixomic::mac {

namespace {

bool micRunningSomewhere()
{
    const auto mic = findDeviceByUid(devices::kMicUid);
    return mic && util::getScalar<UInt32>(*mic, kAudioDevicePropertyDeviceIsRunningSomewhere).value_or(0) != 0;
}

} // namespace

struct DemandMonitor::Impl {
    // Shared with the timer handler, which may still be queued when the monitor goes away.
    struct State {
        bool alive = true;
        bool inUse = false;
        std::function<void(bool)> callback;
    };
    std::function<void(bool)> callback;
    std::shared_ptr<State> state;
    dispatch_source_t timer = nil;
};

DemandMonitor::DemandMonitor(std::function<void(bool inUse)> callback) : impl_(std::make_unique<Impl>())
{
    impl_->callback = std::move(callback);
}

DemandMonitor::~DemandMonitor()
{
    stop();
}

void DemandMonitor::start()
{
    if (impl_->timer != nil)
        return;
    auto state = std::make_shared<Impl::State>();
    state->callback = impl_->callback;
    impl_->state = state;
    impl_->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(impl_->timer, ^{
        if (!state->alive)
            return;
        const bool inUse = micRunningSomewhere();
        if (inUse == state->inUse)
            return;
        state->inUse = inUse;
        const auto callback = state->callback;   // the callback may stop or destroy the monitor
        if (callback)
            callback(inUse);
    });
    const auto period = static_cast<uint64_t>(kPollMilliseconds) * NSEC_PER_MSEC;
    dispatch_source_set_timer(impl_->timer, dispatch_time(DISPATCH_TIME_NOW, 0), period, period / 10);
    dispatch_resume(impl_->timer);
}

void DemandMonitor::stop()
{
    if (impl_->timer == nil)
        return;
    impl_->state->alive = false;
    dispatch_source_cancel(impl_->timer);
    impl_->timer = nil;
}

bool DemandMonitor::inUse() const
{
    return impl_->state != nullptr && impl_->state->inUse;
}

} // namespace fixomic::mac
