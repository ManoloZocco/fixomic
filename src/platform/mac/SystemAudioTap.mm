#include "platform/mac/SystemAudioTap.h"

#include "platform/mac/CoreAudioUtil.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

namespace fixomic::mac {

namespace {

AudioObjectID ownProcessObject()
{
    const auto addr = util::address(kAudioHardwarePropertyTranslatePIDToProcessObject);
    const pid_t pid = getpid();
    AudioObjectID process = kAudioObjectUnknown;
    UInt32 size = sizeof(process);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(pid), &pid, &size, &process) != noErr)
        return kAudioObjectUnknown;
    return process;
}

std::optional<AudioStreamBasicDescription> tapFormat(AudioObjectID tap)
{
    return util::getScalar<AudioStreamBasicDescription>(tap, kAudioTapPropertyFormat);
}

} // namespace

std::unique_ptr<SystemAudioTap> SystemAudioTap::create(juce::String& error)
{
    @autoreleasepool {
        const AudioObjectID self = ownProcessObject();
        if (self == kAudioObjectUnknown) {
            error = "system audio tap: cannot translate fixomic's pid to a Core Audio process object";
            return nullptr;
        }
        CATapDescription* description = [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:@[ @(self) ]];
        description.name = @"fixomic system audio";
        description.muteBehavior = CATapUnmuted;
        description.privateTap = YES;

        AudioObjectID tap = kAudioObjectUnknown;
        const OSStatus status = AudioHardwareCreateProcessTap(description, &tap);
        if (status != noErr || tap == kAudioObjectUnknown) {
            error = "system audio tap: AudioHardwareCreateProcessTap failed: " + util::statusString(status);
            return nullptr;
        }
        juce::String uid = util::getString(tap, kAudioTapPropertyUID);
        if (uid.isEmpty())
            uid = util::toString((__bridge CFStringRef)description.UUID.UUIDString);
        return std::unique_ptr<SystemAudioTap>(new SystemAudioTap(tap, uid));
    }
}

SystemAudioTap::SystemAudioTap(AudioObjectID id, juce::String uid) : id_(id), uid_(std::move(uid)) {}

SystemAudioTap::~SystemAudioTap()
{
    AudioHardwareDestroyProcessTap(id_);
}

juce::String SystemAudioTap::uid() const
{
    return uid_;
}

AudioObjectID SystemAudioTap::id() const
{
    return id_;
}

double SystemAudioTap::sampleRate() const
{
    const auto format = tapFormat(id_);
    return format ? format->mSampleRate : 0.0;
}

int SystemAudioTap::channels() const
{
    const auto format = tapFormat(id_);
    return format ? static_cast<int>(format->mChannelsPerFrame) : 0;
}

} // namespace fixomic::mac
