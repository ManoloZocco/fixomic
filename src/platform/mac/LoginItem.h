#pragma once

#include <juce_core/juce_core.h>

namespace fixomic::mac {

// "Open at login" through SMAppService.mainAppService (macOS 13+): the app itself, at the path it
// runs from, registered as a login item (Impostazioni di Sistema -> Generali -> Elementi login ed estensioni).
// Main thread.

// The service is enabled (registered and approved).
bool launchAtLoginEnabled();
// The user turned the item off in System Settings: registered, waiting for approval there.
bool launchAtLoginRequiresApproval();
// Registers / unregisters. False with an Italian `error` on failure, and also when the system
// keeps the item waiting for approval (the error then says where to approve it). Unregistering an
// item that is not registered succeeds.
bool setLaunchAtLogin(bool enabled, juce::String& error);

} // namespace fixomic::mac
