#pragma once

// Small Core Audio property helpers shared by the src/platform/mac/*.mm files. Internal: not
// meant for code outside src/platform/mac. Never call from an IOProc (they allocate).

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <juce_core/juce_core.h>

#include <cstring>
#include <optional>
#include <vector>

namespace fixomic::mac::util {

inline AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                          AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                          AudioObjectPropertyElement element = kAudioObjectPropertyElementMain)
{
    return {selector, scope, element};
}

template <typename T>
std::optional<T> getScalar(AudioObjectID object, AudioObjectPropertySelector selector,
                           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const auto addr = address(selector, scope);
    T value{};
    UInt32 size = sizeof(T);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr || size != sizeof(T))
        return std::nullopt;
    return value;
}

template <typename T>
OSStatus setScalar(AudioObjectID object, AudioObjectPropertySelector selector, const T& value,
                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const auto addr = address(selector, scope);
    return AudioObjectSetPropertyData(object, &addr, 0, nullptr, sizeof(T), &value);
}

inline juce::String toString(CFStringRef string)
{
    if (string == nullptr)
        return {};
    return juce::String::fromCFString(string);
}

inline juce::String getString(AudioObjectID object, AudioObjectPropertySelector selector)
{
    const auto addr = address(selector);
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};
    juce::String result = toString(value);
    CFRelease(value);
    return result;
}

// Variable-size array property (object lists, ranges, ...).
template <typename T>
std::vector<T> getArray(AudioObjectID object, AudioObjectPropertySelector selector,
                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const auto addr = address(selector, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &size) != noErr || size < sizeof(T))
        return {};
    std::vector<T> values(size / sizeof(T));
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, values.data()) != noErr)
        return {};
    values.resize(size / sizeof(T));
    return values;
}

// Total channels over all streams of `scope` (kAudioDevicePropertyStreamConfiguration).
inline int channelCount(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const auto addr = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    std::vector<std::byte> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr)
        return 0;
    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return channels;
}

// Nominal rates the device offers, ascending (continuous ranges expanded to the standard rates
// they contain). Defined in CoreAudioDevices.mm.
std::vector<double> nominalRates(AudioObjectID device);

// "'nope' (1852797029)" for four-char codes, the number otherwise.
inline juce::String statusString(OSStatus status)
{
    char chars[4];
    const UInt32 be = CFSwapInt32HostToBig(static_cast<UInt32>(status));
    std::memcpy(chars, &be, 4);
    for (char c : chars)
        if (c < 32 || c > 126)
            return juce::String(static_cast<int>(status));
    return "'" + juce::String(chars, 4) + "' (" + juce::String(static_cast<int>(status)) + ")";
}

} // namespace fixomic::mac::util
