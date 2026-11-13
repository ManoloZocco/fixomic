#pragma once

#include "ui/PanelParts.h"
#include "ui/SetupModel.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace fixomic::ui {

// "Impostazioni…": an in-place view that takes the panel's place (never a window, no modal alert).
// A header with back (‹) and "Impostazioni", then, in this order:
//   Microfono: the input devices (the configured one checked; "<nome> — non collegato" while it is
//     not connected, never replaced by another one), and "Canale" for a device with more than one
//     input channel;
//   Frequenza di campionamento: "48 kHz", information only (the driver runs at 48 kHz), with the
//     UnsupportedSampleRate text in red when the chosen microphone does not offer it;
//   Buffer: 64 / 128 / 256 samples, each with its duration;
//   Avvio all'accesso: a switch; why it could not be changed, inline;
//   Driver: its status; "Installa driver" or "Aggiorna driver (vX → vY)", and "Disinstalla driver",
//     confirmed inline since it removes a system component. While an operation runs the buttons are
//     disabled and "Operazione in corso…" shows; then its result, until the view is reset (a result
//     that arrived while the view was closed survives one reset: it shows at the next opening);
//   Permessi: the microphone and system audio states; "Consenti il microfono" while macOS has not asked
//     for it (System Settings does not list fixomic yet), "Apri Impostazioni di Sistema" for a denied
//     microphone and for system audio not granted ("Non noto" is unknown, not denied);
//   "Rivedi la configurazione iniziale".
//
// Data in through setData(), intents out through the callbacks (MainPanel wires them to the
// AppController). Choices from the lists are reported once their click handler has returned. Its
// bounds reach Theme::kFlatTextInset past the content edges, as PluginPicker's; it sets its own
// height (heightForWidth()). Escape and back call onClose. Message thread only.
class SettingsPanel : public juce::Component {
public:
    struct Data {
        std::vector<DeviceChoice> devices;
        int channel = 0;               // config().inputChannel, 0-based
        int bufferSize = 128;
        bool launchAtLogin = false;    // the login item's system state
        DriverFacts driver;
        mac::Permission microphone = mac::Permission::NotDetermined;
        bool microphoneRequestPending = false;   // the microphone prompt is up
        mac::Permission systemAudio = mac::Permission::NotDetermined;
        std::optional<mac::WarningKind> warning;
        bool operator==(const Data&) const = default;
    };

    SettingsPanel();
    ~SettingsPanel() override;

    void setData(const Data&);
    const Data& data() const;
    // Back to the start, when the view is opened: no uninstall confirmation, no login error, no driver
    // result unless it arrived while the view was closed (not visible) and has not been seen yet.
    void reset();
    int heightForWidth(int width) const;

    // The operation started through onInstallDriver / onUninstallDriver finished (ok, or the
    // Italian error): its result shows inline.
    void driverOperationFinished(bool ok, const juce::String& error);
    bool driverOperationStarted() const;   // started from this view and not finished yet

    std::function<void(const DeviceChoice&)> onChooseDevice;
    std::function<void(int channel)> onChooseChannel;
    std::function<void(int bufferSize)> onChooseBuffer;
    std::function<juce::String(bool enabled)> onSetLaunchAtLogin;   // "" on success, else why not
    std::function<void()> onInstallDriver;     // install, or update
    std::function<void()> onUninstallDriver;   // after the inline confirmation
    std::function<void(mac::PrivacyPane)> onOpenPane;
    std::function<void()> onRequestMicrophone;   // "Consenti il microfono"
    std::function<void()> onReviewSetup;       // "Rivedi la configurazione iniziale"
    std::function<void()> onClose;             // back, Escape

    // State, for tests and the snapshot.
    bool confirmingUninstall() const;
    juce::String driverMessage() const;        // progress, result or error ("" for none)
    juce::String loginError() const;
    juce::String sampleRateText() const;       // the warning under "48 kHz", "" for none

    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    enum class Operation { None, Install, Update, Uninstall };

    struct Layout;
    Layout layOut(int width) const;
    void update();          // the children from data_ and the view's own state
    void fitHeight();
    void startInstall();
    void startUninstall();
    void setLaunchAtLogin();

    Data data_;
    Operation operation_ = Operation::None;   // started here, running
    Operation finished_ = Operation::None;    // the last one, for its result line
    juce::String driverError_;
    bool resultUnseen_ = false;               // the result arrived while the view was closed
    bool confirming_ = false;
    juce::String loginError_;
    std::vector<int> separators_;

    GlyphButton back_{GlyphButton::Glyph::Back, "Torna al pannello"};
    TextBlock title_{TextStyle::BodyStrong, TextBlock::Ink::Primary};

    TextBlock micTitle_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    ChoiceList devices_{"Microfono"};
    TextBlock noDevices_{TextStyle::Caption, TextBlock::Ink::Secondary};
    TextBlock channelLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    SegmentedControl channels_{"Canale"};

    TextBlock rateLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock rateValue_{TextStyle::Body, TextBlock::Ink::Secondary, juce::Justification::topRight};
    TextBlock rateWarning_{TextStyle::Caption, TextBlock::Ink::Problem};
    TextBlock bufferLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    SegmentedControl buffers_{"Buffer"};

    TextBlock loginLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    juce::ToggleButton loginSwitch_;
    TextBlock loginErrorText_{TextStyle::Caption, TextBlock::Ink::Problem};

    TextBlock driverTitle_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    TextBlock driverStatus_{TextStyle::Body, TextBlock::Ink::Secondary, juce::Justification::topRight};
    juce::TextButton install_, uninstall_;
    TextBlock confirmText_{TextStyle::Caption, TextBlock::Ink::Primary};
    juce::TextButton confirmUninstall_, cancelUninstall_;
    TextBlock driverMessage_{TextStyle::Caption, TextBlock::Ink::Secondary};

    TextBlock permissionsTitle_{TextStyle::BodyStrong, TextBlock::Ink::Primary};
    TextBlock micPermissionLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock micPermission_{TextStyle::Body, TextBlock::Ink::Secondary, juce::Justification::topRight};
    juce::TextButton micAsk_;
    juce::TextButton micSettings_;
    TextBlock audioPermissionLabel_{TextStyle::Body, TextBlock::Ink::Primary};
    TextBlock audioPermission_{TextStyle::Body, TextBlock::Ink::Secondary, juce::Justification::topRight};
    TextBlock audioNote_{TextStyle::Caption, TextBlock::Ink::Secondary};
    juce::TextButton audioSettings_;

    juce::TextButton review_;
};

} // namespace fixomic::ui
