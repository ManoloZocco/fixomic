#pragma once

// Names and UIDs of the two devices published by the fixomic HAL driver (driver/, BlackHole
// built with fixomic macros). "fixomic Mic" is the visible input-only device that apps such as
// Discord read; "fixomic Bridge" is the hidden output-only device the app writes to. Both share
// one ring buffer inside the driver.
namespace fixomic::devices {
inline constexpr const char* kMicName = "fixomic Mic";
inline constexpr const char* kMicUid = "fixomic_UID";
inline constexpr const char* kBridgeName = "fixomic Bridge";
inline constexpr const char* kBridgeUid = "fixomic_2_UID";
inline constexpr double kSampleRate = 48000.0;
}
