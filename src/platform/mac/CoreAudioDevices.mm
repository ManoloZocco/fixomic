#include "platform/mac/CoreAudioDevices.h"

#include "platform/mac/CoreAudioUtil.h"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>

namespace fixomic::mac {

namespace {

constexpr const char* kFixomicUidPrefix = "fixomic_";

} // namespace

std::vector<double> util::nominalRates(AudioObjectID device)
{
    static constexpr double kStandardRates[] = {8000.0,  11025.0, 12000.0, 16000.0,  22050.0,  24000.0, 32000.0,
                                                44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
    std::vector<double> rates;
    for (const AudioValueRange& range :
         util::getArray<AudioValueRange>(device, kAudioDevicePropertyAvailableNominalSampleRates)) {
        if (range.mMaximum - range.mMinimum < 1.0) {
            rates.push_back(range.mMinimum);
            continue;
        }
        for (double rate : kStandardRates)
            if (rate >= range.mMinimum && rate <= range.mMaximum)
                rates.push_back(rate);
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

std::vector<AudioDeviceInfo> filterSelectableInputs(std::vector<AudioDeviceInfo> all)
{
    all.erase(std::remove_if(all.begin(), all.end(),
                             [](const AudioDeviceInfo& d) {
                                 return d.uid.startsWith(kFixomicUidPrefix) || d.inputChannels <= 0;
                             }),
              all.end());
    return all;
}

std::vector<AudioDeviceInfo> listInputDevices()
{
    std::vector<AudioDeviceInfo> all;
    for (AudioObjectID id : util::getArray<AudioObjectID>(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
        all.push_back(AudioDeviceInfo{id, util::getString(id, kAudioDevicePropertyDeviceUID),
                                      util::getString(id, kAudioObjectPropertyName),
                                      util::channelCount(id, kAudioObjectPropertyScopeInput), util::nominalRates(id)});
    }
    return filterSelectableInputs(std::move(all));
}

std::optional<AudioObjectID> findDeviceByUid(const juce::String& uid)
{
    const auto addr = util::address(kAudioHardwarePropertyTranslateUIDToDevice);
    CFStringRef uidString = uid.toCFString();
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const OSStatus status =
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(uidString), &uidString, &size, &device);
    CFRelease(uidString);
    if (status != noErr || device == kAudioObjectUnknown)
        return std::nullopt;
    return device;
}

namespace {

// The HAL delivers a dying object's last notifications with dispatch_sync on the listener's queue
// while it holds the object's lock, and removing a listener waits for that lock. With the main
// queue as listener queue, removing a listener on the main thread while Core Audio tears the object
// down deadlocks (seen when the IO was closed after `killall coreaudiod`: the app froze). So the HAL
// calls the listener blocks on this serial queue, and they only forward to the main queue.
dispatch_queue_t listenerQueue()
{
    static dispatch_queue_t queue = dispatch_queue_create("fixomic.coreaudio.listeners", DISPATCH_QUEUE_SERIAL);
    return queue;
}

} // namespace

// The blocks hold the shared state, so a call already forwarded to the main queue still finds valid
// memory after the watcher is gone; `alive`, checked on the main queue, makes it a no-op then.
struct PropertyWatcher::Impl {
    struct State {
        std::atomic<bool> alive{true};
        std::function<void()> callback;
    };

    AudioObjectID object = kAudioObjectUnknown;
    AudioObjectPropertyAddress address{};
    std::shared_ptr<State> state = std::make_shared<State>();
    AudioObjectPropertyListenerBlock block = nil;
    bool listening = false;
};

PropertyWatcher::PropertyWatcher(AudioObjectID object, AudioObjectPropertyAddress address,
                                 std::function<void()> callback)
    : impl_(std::make_unique<Impl>())
{
    impl_->object = object;
    impl_->address = address;
    impl_->state->callback = std::move(callback);
    std::shared_ptr<Impl::State> state = impl_->state;
    impl_->block = ^(UInt32, const AudioObjectPropertyAddress*) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load() && state->callback)
            state->callback();
      });
    };
    impl_->listening =
        AudioObjectAddPropertyListenerBlock(object, &impl_->address, listenerQueue(), impl_->block) == noErr;
}

PropertyWatcher::~PropertyWatcher()
{
    impl_->state->alive.store(false);
    if (impl_->listening)
        AudioObjectRemovePropertyListenerBlock(impl_->object, &impl_->address, listenerQueue(), impl_->block);
}

bool PropertyWatcher::isListening() const
{
    return impl_->listening;
}

} // namespace fixomic::mac
