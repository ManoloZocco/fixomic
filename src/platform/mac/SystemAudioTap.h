#pragma once

#include <CoreAudio/AudioHardware.h>
#include <juce_core/juce_core.h>

#include <memory>

namespace fixomic::mac {

// Core Audio process tap on the system audio (the mix every process sends to the default output
// device): mono, global, excluding fixomic's own process, unmuted (what the user hears is never
// changed) and private (visible to this process only). The tap is destroyed with the object.
//
// Reading the tap falls under the "System Audio Recording" permission of the responsible process
// (NSAudioCaptureUsageDescription in its Info.plist). Use it through AggregateIO, which puts it in
// a private aggregate device (see docs/notes/aggregate.md).
class SystemAudioTap {
public:
    static std::unique_ptr<SystemAudioTap> create(juce::String& error);
    ~SystemAudioTap();
    SystemAudioTap(const SystemAudioTap&) = delete;
    SystemAudioTap& operator=(const SystemAudioTap&) = delete;

    juce::String uid() const;
    AudioObjectID id() const;

    // From kAudioTapPropertyFormat: rate of the default output device, 1 channel.
    double sampleRate() const;
    int channels() const;

private:
    SystemAudioTap(AudioObjectID id, juce::String uid);

    AudioObjectID id_;
    juce::String uid_;
};

} // namespace fixomic::mac
