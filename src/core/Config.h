#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace fixomic {

struct PluginSlot {
    juce::String descriptionXml;   // juce::PluginDescription::createXml()
    juce::MemoryBlock state;       // getStateInformation()
    bool bypassed = false;
};

struct Config {
    int version = 1;
    juce::String inputDeviceUid;   // "" = none selected
    int inputChannel = 0;          // 0-based
    int bufferSize = 128;          // 64 | 128 | 256
    bool aecEnabled = true;
    bool globalBypass = false;
    bool launchAtLogin = false;
    std::vector<PluginSlot> chain;
};

juce::String toXmlString(const Config&);

// Returns nullopt when the text is not valid XML or is not a fixomic config document.
std::optional<Config> fromXmlString(const juce::String&);

} // namespace fixomic
