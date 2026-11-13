#pragma once

#include "ui/Onboarding.h"
#include "ui/SettingsPanel.h"
#include "ui/SetupModel.h"

namespace fixomic {
class AppController;
}

namespace fixomic::ui {

class MainPanel;

// The glue between the AppController and the views that take the panel's place (the settings and
// the first-run guide): what they show, and where their intents go. Message thread only.

DriverFacts driverFacts(const AppController&);
SettingsPanel::Data settingsData(const AppController&);
Onboarding::Data onboardingData(const AppController&);

// The microphone chosen in a view: its uid and name, the first channel.
void chooseInput(AppController&, const DeviceChoice&);

// Wires the panel's settings and guide to `app`: their intents become AppController calls; the
// asynchronous completions (driver operations, permission prompts) come back through a SafePointer
// on `panel`. Both must outlive the views' callbacks (MainPanel calls this on itself, and owns them).
void connectPages(MainPanel& panel, AppController& app);

} // namespace fixomic::ui
