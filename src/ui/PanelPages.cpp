#include "ui/PanelPages.h"

#include "app/AppController.h"
#include "ui/MainPanel.h"

namespace fixomic::ui {

DriverFacts driverFacts(const AppController& app) {
    return {app.driverInstalled(), app.installedDriverVersion(), app.bundledDriverVersion(),
            app.driverUpdateAvailable(), app.driverOperationInProgress()};
}

SettingsPanel::Data settingsData(const AppController& app) {
    const auto& config = app.config();
    SettingsPanel::Data data;
    data.devices = deviceChoices(app.inputDevices(), config.inputDeviceUid, config.inputDeviceName);
    data.channel = config.inputChannel;
    data.bufferSize = config.bufferSize;
    data.launchAtLogin = app.launchAtLogin();
    data.driver = driverFacts(app);
    data.microphone = app.microphonePermission();
    data.microphoneRequestPending = app.microphoneRequestPending();
    data.systemAudio = app.systemAudioPermission();
    data.warning = app.lastWarningKind();
    return data;
}

Onboarding::Data onboardingData(const AppController& app) {
    const auto& config = app.config();
    Onboarding::Data data;
    data.devices = deviceChoices(app.inputDevices(), config.inputDeviceUid, config.inputDeviceName);
    data.driver = driverFacts(app);
    auto& f = data.facts;
    f.microphone = app.microphonePermission();
    f.microphoneRequestPending = app.microphoneRequestPending();
    f.microphoneChosen = config.inputDeviceUid.isNotEmpty();
    f.systemAudio = app.systemAudioPermission();
    f.systemAudioRequestPending = app.systemAudioRequestPending();
    f.systemAudioAnswered = app.systemAudioAnswered();
    f.warning = app.lastWarningKind();
    f.driverInstalled = data.driver.installed;
    f.driverOperationInProgress = data.driver.operationInProgress;
    f.driverBundled = data.driver.bundledVersion.isNotEmpty();
    return data;
}

void chooseInput(AppController& app, const DeviceChoice& device) {
    app.updateConfig([uid = device.uid, name = device.name](Config& c) {
        c.inputDeviceUid = uid;
        c.inputDeviceName = name;
        c.inputChannel = 0;
    });
}

void connectPages(MainPanel& panel, AppController& app) {
    auto& settings = panel.settings();
    auto& onboarding = panel.onboarding();
    const auto safe = juce::Component::SafePointer<MainPanel>(&panel);

    settings.onClose = [&panel] { panel.closePage(); };
    settings.onChooseDevice = [&app](const DeviceChoice& device) { chooseInput(app, device); };
    settings.onChooseChannel = [&app](int channel) {
        app.updateConfig([channel](Config& c) { c.inputChannel = channel; });
    };
    // Applied at once: the engine reopens the IO, and the AEC and the plugins are prepared for the
    // new size while it is closed (MacEngineBackend::openIO).
    settings.onChooseBuffer = [&app](int size) { app.updateConfig([size](Config& c) { c.bufferSize = size; }); };
    settings.onSetLaunchAtLogin = [&panel, &app](bool enabled) {
        juce::String error;
        app.setLaunchAtLogin(enabled, error);
        panel.refreshPage();   // the switch shows the system state
        return error;
    };
    settings.onInstallDriver = [&panel, &app, safe] {
        app.installDriver([safe](bool ok, juce::String error) {
            if (safe != nullptr) safe->settings().driverOperationFinished(ok, error);
        });
        panel.refreshPage();   // in progress
    };
    settings.onUninstallDriver = [&panel, &app, safe] {
        app.uninstallDriver([safe](bool ok, juce::String error) {
            if (safe != nullptr) safe->settings().driverOperationFinished(ok, error);
        });
        panel.refreshPage();
    };
    settings.onOpenPane = [&app](mac::PrivacyPane pane) { app.openPrivacySettings(pane); };
    // The prompt the guide raises (shared with the engine's): the answer notifies, and the view re-reads.
    settings.onRequestMicrophone = [&app] { app.requestMicrophone([](bool) {}); };
    settings.onReviewSetup = [&panel] { panel.showOnboarding(); };

    onboarding.onClose = [&panel] { panel.closePage(); };   // closed early: onboardingDone stays false
    onboarding.onRequestMicrophone = [&app] {
        // The answer re-evaluates the engine and notifies; the prompt's pending state shows at once.
        app.requestMicrophone([](bool) {});
    };
    onboarding.onRequestSystemAudio = [&app, safe] {
        app.requestSystemAudio([safe](bool ok) {
            if (safe != nullptr) safe->onboarding().systemAudioRequestFinished(ok);
        });
    };
    onboarding.onChooseDevice = [&app](const DeviceChoice& device) { chooseInput(app, device); };
    onboarding.onOpenPane = [&app](mac::PrivacyPane pane) { app.openPrivacySettings(pane); };
    onboarding.onInstallDriver = [&panel, &app, safe] {
        app.installDriver([safe](bool ok, juce::String error) {
            if (safe != nullptr) safe->onboarding().driverOperationFinished(ok, error);
        });
        panel.refreshPage();
    };
    onboarding.onFinish = [&panel, &app] {
        app.updateConfig([](Config& c) { c.onboardingDone = true; });
        panel.closePage();
    };
}

} // namespace fixomic::ui
