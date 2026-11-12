#include "platform/mac/Permissions.h"

#include "platform/mac/SystemAudioTap.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>

namespace fixomic::mac {

Permission microphonePermission()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized: return Permission::Granted;
    case AVAuthorizationStatusNotDetermined: return Permission::NotDetermined;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted: break;
    }
    return Permission::Denied;
}

void requestMicrophone(std::function<void(bool granted)> completion)
{
    // The block copies the std::function; AVFoundation calls it on an arbitrary queue.
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                                 dispatch_async(dispatch_get_main_queue(), ^{
                                     if (completion)
                                         completion(granted == YES);
                                 });
                             }];
}

namespace {

// int TCCAccessPreflight(CFStringRef service, CFDictionaryRef options): 0 granted, 1 denied,
// 2 not determined (SPI, resolved at run time; absent -> nullptr).
using TccPreflight = int (*)(CFStringRef, CFDictionaryRef);

TccPreflight tccPreflight()
{
    static const TccPreflight fn = [] {
        void* handle = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_LAZY | RTLD_LOCAL);
        return handle != nullptr ? reinterpret_cast<TccPreflight>(dlsym(handle, "TCCAccessPreflight")) : nullptr;
    }();
    return fn;
}

// The throwaway IO behind requestSystemAudio(). Background thread.
void raiseSystemAudioPrompt()
{
    juce::String error;
    auto tap = SystemAudioTap::create(error);
    if (!tap)
        return;   // openIO() then reports why when it tries the real tap
    @autoreleasepool {
        NSString* tapUid = [NSString stringWithUTF8String:tap->uid().toRawUTF8()];
        if (tapUid == nil)
            return;
        NSDictionary* description = @{
            @kAudioAggregateDeviceNameKey : @"fixomic permission",
            @kAudioAggregateDeviceUIDKey : [@"fixomic_permission_" stringByAppendingString:[NSUUID UUID].UUIDString],
            @kAudioAggregateDeviceIsPrivateKey : @1,
            @kAudioAggregateDeviceTapAutoStartKey : @0,
            @kAudioAggregateDeviceTapListKey : @[ @{
                @kAudioSubTapUIDKey : tapUid,
                @kAudioSubTapDriftCompensationKey : @1,
            } ],
        };
        AudioObjectID aggregate = kAudioObjectUnknown;
        if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &aggregate) != noErr)
            return;
        AudioDeviceIOProcID proc = nullptr;
        // Blocks here while the prompt is up (measured: 17-22 s until answered).
        if (AudioDeviceCreateIOProcIDWithBlock(&proc, aggregate, nullptr,
                                               ^(const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                                                 AudioBufferList*, const AudioTimeStamp*) {
                                               })
                == noErr
            && proc != nullptr)
            AudioDeviceDestroyIOProcID(aggregate, proc);
        AudioHardwareDestroyAggregateDevice(aggregate);
    }
}

} // namespace

Permission systemAudioPermission()
{
    const auto preflight = tccPreflight();
    if (preflight == nullptr)
        return Permission::NotDetermined;
    switch (preflight(CFSTR("kTCCServiceAudioCapture"), nullptr)) {
    case 0: return Permission::Granted;
    case 1: return Permission::Denied;
    default: return Permission::NotDetermined;
    }
}

void requestSystemAudio(std::function<void()> completion)
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        raiseSystemAudioPrompt();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion)
                completion();
        });
    });
}

} // namespace fixomic::mac
