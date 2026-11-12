#include "platform/mac/Permissions.h"

#import <AVFoundation/AVFoundation.h>

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

} // namespace fixomic::mac
