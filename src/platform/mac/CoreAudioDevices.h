#pragma once

// Core Audio device enumeration and property listening. Plain C++ interface (usable from .cpp);
// implemented in CoreAudioDevices.mm.

#include <CoreAudio/AudioHardware.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace fixomic::mac {

struct AudioDeviceInfo {
    AudioObjectID id;
    juce::String uid, name;
    int inputChannels;
    std::vector<double> sampleRates;  // nominal rates the device offers, ascending
};

// Devices the user may pick as microphone: drops every device whose UID starts with "fixomic_"
// (fixomic Mic, fixomic Bridge and fixomic's own private aggregates, which this process sees in
// its own device list) and every device without input channels. Keeps the order.
std::vector<AudioDeviceInfo> filterSelectableInputs(std::vector<AudioDeviceInfo> all);

// = filterSelectableInputs(every device in kAudioHardwarePropertyDevices).
std::vector<AudioDeviceInfo> listInputDevices();

// kAudioHardwarePropertyTranslateUIDToDevice: also finds hidden devices (fixomic Bridge).
std::optional<AudioObjectID> findDeviceByUid(const juce::String& uid);

// RAII wrapper around AudioObjectAddPropertyListenerBlock. The callback runs on the main queue
// and never after the watcher has been destroyed (destroy it on the main thread). The HAL calls the
// listener on a private serial queue that forwards to the main queue, so destroying a watcher never
// waits for the main queue: safe while Core Audio tears the object down (see CoreAudioDevices.mm).
class PropertyWatcher {
public:
    PropertyWatcher(AudioObjectID object, AudioObjectPropertyAddress address, std::function<void()> callback);
    ~PropertyWatcher();
    PropertyWatcher(const PropertyWatcher&) = delete;
    PropertyWatcher& operator=(const PropertyWatcher&) = delete;

    // False if the HAL refused the listener (e.g. the object is gone).
    bool isListening() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fixomic::mac
