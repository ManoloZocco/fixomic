#include <catch2/catch_test_macros.hpp>

#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/FixomicDevices.h"

#include <algorithm>

using fixomic::mac::AudioDeviceInfo;
using fixomic::mac::filterSelectableInputs;

namespace {

AudioDeviceInfo device(AudioObjectID id, const char* uid, const char* name, int inputs)
{
    return AudioDeviceInfo{id, uid, name, inputs, {44100.0, 48000.0}};
}

std::vector<juce::String> uids(const std::vector<AudioDeviceInfo>& devices)
{
    std::vector<juce::String> result;
    for (const auto& d : devices)
        result.push_back(d.uid);
    return result;
}

} // namespace

TEST_CASE("filterSelectableInputs drops fixomic devices and devices without inputs", "[devices]")
{
    const std::vector<AudioDeviceInfo> all{
        device(10, "BuiltInHeadphoneInputDevice", "Microfono esterno", 1),
        device(11, fixomic::devices::kMicUid, fixomic::devices::kMicName, 2),
        device(12, "BuiltInSpeakerDevice", "Altoparlanti MacBook Air", 0),
        device(13, "BuiltInMicrophoneDevice", "Microfono MacBook Air", 1),
        device(14, fixomic::devices::kBridgeUid, fixomic::devices::kBridgeName, 0),
        device(15, "AppleUSBAudioEngine:HyperX:1", "HyperX Cloud", 2),
    };

    const auto selectable = filterSelectableInputs(all);

    REQUIRE(uids(selectable)
            == std::vector<juce::String>{"BuiltInHeadphoneInputDevice", "BuiltInMicrophoneDevice",
                                         "AppleUSBAudioEngine:HyperX:1"});
    REQUIRE(selectable[0].id == 10);
    REQUIRE(selectable[1].name == "Microfono MacBook Air");
    REQUIRE(selectable[2].inputChannels == 2);
    REQUIRE(selectable[2].sampleRates == std::vector<double>{44100.0, 48000.0});
}

TEST_CASE("filterSelectableInputs drops fixomic's own private aggregate devices", "[devices]")
{
    // A process sees its own private aggregates in the device list; ours are fixomic_-prefixed.
    const std::vector<AudioDeviceInfo> all{
        device(20, "fixomic_aggregate_6A0F5E1C", "fixomic Aggregate", 3),
        device(21, "BuiltInMicrophoneDevice", "Microfono MacBook Air", 1),
    };
    REQUIRE(uids(filterSelectableInputs(all)) == std::vector<juce::String>{"BuiltInMicrophoneDevice"});
}

TEST_CASE("filterSelectableInputs keeps a device whose UID only contains fixomic", "[devices]")
{
    const std::vector<AudioDeviceInfo> all{device(30, "USB:my-fixomic_mic", "Someone's mic", 1)};
    REQUIRE(filterSelectableInputs(all).size() == 1);
}

TEST_CASE("filterSelectableInputs on an empty list", "[devices]")
{
    REQUIRE(filterSelectableInputs({}).empty());
}

TEST_CASE("listInputDevices never lists fixomic devices", "[devices][hardware]")
{
    const auto inputs = fixomic::mac::listInputDevices();
    REQUIRE_FALSE(inputs.empty());
    for (const auto& d : inputs) {
        REQUIRE_FALSE(d.uid.startsWith("fixomic_"));
        REQUIRE(d.inputChannels > 0);
    }
}

TEST_CASE("findDeviceByUid resolves the hidden fixomic Bridge", "[devices][hardware]")
{
    REQUIRE(fixomic::mac::findDeviceByUid(fixomic::devices::kBridgeUid).has_value());
    REQUIRE_FALSE(fixomic::mac::findDeviceByUid("fixomic_no_such_device").has_value());
}
