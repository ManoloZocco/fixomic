#include "platform/mac/AppActivation.h"

#import <AppKit/AppKit.h>

namespace fixomic::mac {

struct AppActivationObserver::Impl {
    id<NSObject> becameActive = nil;
    id<NSObject> resignedActive = nil;
};

AppActivationObserver::AppActivationObserver(std::function<void(bool active)> onChange)
    : impl_(std::make_unique<Impl>())
{
    auto callback = std::make_shared<std::function<void(bool)>>(std::move(onChange));
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    // queue nil: the block runs synchronously on the posting thread (the main thread, for NSApp).
    impl_->becameActive = [center addObserverForName:NSApplicationDidBecomeActiveNotification
                                              object:nil
                                               queue:nil
                                          usingBlock:^(NSNotification*) {
                                            if (*callback)
                                                (*callback)(true);
                                          }];
    impl_->resignedActive = [center addObserverForName:NSApplicationDidResignActiveNotification
                                                object:nil
                                                 queue:nil
                                            usingBlock:^(NSNotification*) {
                                              if (*callback)
                                                  (*callback)(false);
                                            }];
}

AppActivationObserver::~AppActivationObserver()
{
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (impl_->becameActive != nil)
        [center removeObserver:impl_->becameActive];
    if (impl_->resignedActive != nil)
        [center removeObserver:impl_->resignedActive];
}

} // namespace fixomic::mac
