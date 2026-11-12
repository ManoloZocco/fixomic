#include "platform/mac/WakeObserver.h"

#import <AppKit/AppKit.h>

namespace fixomic::mac {

struct WakeObserver::Impl {
    id<NSObject> token = nil;
};

WakeObserver::WakeObserver(std::function<void()> onWake) : impl_(std::make_unique<Impl>())
{
    auto callback = std::make_shared<std::function<void()>>(std::move(onWake));
    impl_->token = [NSWorkspace.sharedWorkspace.notificationCenter
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification*) {
                  if (*callback)
                      (*callback)();
                }];
}

WakeObserver::~WakeObserver()
{
    if (impl_->token != nil)
        [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:impl_->token];
}

} // namespace fixomic::mac
