#include "ui/TrayIcon.h"

#include "app/AppController.h"
#include "platform/mac/StatusItem.h"
#include "ui/Theme.h"

#include <cmath>

namespace fixomic::ui {

namespace {

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }

EngineSnapshot snapshotOf(const AppController& app) {
    EngineSnapshot s;
    s.state = app.engineState();
    s.pendingPermission = app.pendingPermission();
    s.lastError = app.lastError();
    s.globalBypass = app.config().globalBypass;
    s.micSelected = app.config().inputDeviceUid.isNotEmpty();
    return s;
}

// Template images are 20 x 20 pt (the size JUCE gives the status item), drawn at 2x: 40 x 40 px.
constexpr int kIconPixels = 40;
constexpr float kStroke = 3.0f;   // 1.5 pt

// Clears `shape` (anti-aliased) out of `image`: knocked-out shapes keep the four icons legible
// where a badge overlaps the microphone.
void knockOut(juce::Image& image, const juce::Path& shape) {
    juce::Image mask(juce::Image::SingleChannel, image.getWidth(), image.getHeight(), true);
    {
        juce::Graphics g(mask);
        g.setColour(juce::Colours::white);
        g.fillPath(shape);
    }
    const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readWrite);
    const juce::Image::BitmapData alpha(mask, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (const auto a = *alpha.getPixelPointer(x, y); a != 0)
                reinterpret_cast<juce::PixelARGB*>(pixels.getPixelPointer(x, y))
                    ->multiplyAlpha(static_cast<float>(255 - a) / 255.0f);
}

juce::Path microphoneCradle() {
    juce::Path path;
    // The U-shaped holder around the capsule, then the stem and the foot.
    path.addCentredArc(20.0f, 16.0f, 10.5f, 10.5f, 0.0f, juce::MathConstants<float>::halfPi,
                       juce::MathConstants<float>::halfPi * 3.0f, true);
    path.startNewSubPath(20.0f, 26.5f);
    path.lineTo(20.0f, 33.5f);
    path.startNewSubPath(14.5f, 34.5f);
    path.lineTo(25.5f, 34.5f);
    return path;
}

const juce::Rectangle<float> kCapsule{14.0f, 3.5f, 12.0f, 20.0f};

} // namespace

juce::Image TrayIcon::iconImage(StatusKind kind) {
    juce::Image image(juce::Image::ARGB, kIconPixels, kIconPixels, true);
    {
        juce::Graphics g(image);
        g.setColour(juce::Colours::black);
        const juce::PathStrokeType stroke(kStroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(microphoneCradle(), stroke);
        if (kind == StatusKind::Active)
            g.fillRoundedRectangle(kCapsule, 6.0f);   // in service
        else
            g.drawRoundedRectangle(kCapsule.reduced(kStroke / 2.0f), 6.0f - kStroke / 2.0f, kStroke);   // hollow
    }
    if (kind == StatusKind::Bypass) {
        // The line passes the microphone without stopping: a bar through the hollow capsule,
        // with a cut of ground on each side so it reads as passing over, not as a strike-out.
        juce::Path bar;
        bar.addRoundedRectangle(5.0f, 11.5f, 30.0f, 4.0f, 2.0f);
        juce::Path gap;
        gap.addRoundedRectangle(3.0f, 9.5f, 34.0f, 8.0f, 4.0f);
        knockOut(image, gap);
        juce::Graphics g(image);
        g.setColour(juce::Colours::black);
        g.fillPath(bar);
    } else if (kind == StatusKind::Problem) {
        // A badge with "!" at the bottom right.
        const juce::Point<float> centre{30.5f, 30.5f};
        juce::Path clearance;
        clearance.addEllipse(juce::Rectangle<float>(19.0f, 19.0f).withCentre(centre));
        knockOut(image, clearance);
        {
            juce::Graphics g(image);
            g.setColour(juce::Colours::black);
            g.fillEllipse(juce::Rectangle<float>(14.0f, 14.0f).withCentre(centre));
        }
        juce::Path mark;
        mark.addRoundedRectangle(centre.x - 1.4f, centre.y - 4.8f, 2.8f, 6.0f, 1.4f);
        mark.addEllipse(juce::Rectangle<float>(2.9f, 2.9f).withCentre({centre.x, centre.y + 3.6f}));
        knockOut(image, mark);
    }
    return image;
}

TrayIcon::TrayIcon(AppController& app) : app_(app), window_(app) {
    window_.panel().onQuit = [this] {
        if (onQuit) onQuit();
    };
    window_.panel().onOpenSettings = [this] {
        if (onOpenSettings) onOpenSettings();
    };
    setIconTooltip("fixomic");
    refreshStatus();
}

TrayIcon::~TrayIcon() {
    stopTimer();
    menuTimer_.stopTimer();
}

void TrayIcon::modelChanged() { refreshStatus(); }

const StatusPresentation& TrayIcon::status() const { return model_.current(); }

void TrayIcon::refreshStatus() {
    const double now = nowSeconds();
    const auto& status = model_.update(snapshotOf(app_), now);
    if (iconShown_ != status.kind) {
        const auto image = iconImage(status.kind);
        setIconImage(image, image);
        iconShown_ = status.kind;
    }
    window_.panel().refresh(status);

    // The DriverMissing hold has a deadline: refresh once more when it passes, if nothing else does.
    if (const auto at = model_.recheckAt())
        startTimer(std::max(1, static_cast<int>(std::ceil((*at - now) * 1000.0))));
    else
        stopTimer();
}

void TrayIcon::timerCallback() {
    stopTimer();
    refreshStatus();
}

MainPanel& TrayIcon::panel() { return window_.panel(); }

void TrayIcon::showPanel() {
    refreshStatus();
    window_.showUnder(mac::statusItemScreenBounds(getNativeHandle()));
}

void TrayIcon::hidePanel() { window_.dismiss(); }

bool TrayIcon::isPanelShowing() const { return window_.isVisible(); }

void TrayIcon::togglePanel() {
    if (isPanelShowing())
        hidePanel();
    else if (!window_.dismissedJustNow())   // the click that took the focus away closes, never reopens
        showPanel();
}

void TrayIcon::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu())
        showMenu();
    else
        togglePanel();
}

void TrayIcon::showMenu() {
    hidePanel();
    // As JUCE's own tray example does: activate, then give the system a moment before the menu.
    juce::Process::makeForegroundProcess();
    menuTimer_.startTimer(kMenuDelayMs);
}

void TrayIcon::openMenu() {
    menuTimer_.stopTimer();
    juce::PopupMenu menu;
    menu.addItem(juce::PopupMenu::Item(status().text).setEnabled(false));
    menu.addSeparator();
    menu.addItem("Esci", [safe = SafePointer<TrayIcon>(this)] {
        if (safe != nullptr && safe->onQuit) safe->onQuit();
    });
    auto options = juce::PopupMenu::Options{};
    if (const auto bounds = mac::statusItemScreenBounds(getNativeHandle())) options = options.withTargetScreenArea(*bounds);
    menuShowing_ = true;
    if (onMenuChanged) onMenuChanged(true);
    menu.showMenuAsync(options, [safe = SafePointer<TrayIcon>(this)](int) {
        if (safe != nullptr) safe->menuClosed();
    });
}

void TrayIcon::menuClosed() {
    menuShowing_ = false;
    if (onMenuChanged) onMenuChanged(false);
}

bool TrayIcon::isMenuShowing() const { return menuShowing_; }

// ---- icon sheet (snapshot) --------------------------------------------------------------------

namespace {

class TrayIconSheet final : public juce::Component {
public:
    TrayIconSheet() { setSize(4 * 88, 76); }

    void paint(juce::Graphics& g) override {
        const auto& p = Theme::palette(*this);
        g.fillAll(p.background);
        const std::pair<StatusKind, const char*> icons[] = {{StatusKind::Active, "Attivo"},
                                                            {StatusKind::Waiting, "In attesa"},
                                                            {StatusKind::Bypass, "Bypass"},
                                                            {StatusKind::Problem, "Problema"}};
        g.setFont(Theme::font(TextStyle::Micro));
        for (int i = 0; i < 4; ++i) {
            const float x = static_cast<float>(i * 88);
            // A strip of menu bar, then the icon at its real size, tinted like a template image.
            g.setColour(p.hover);
            g.fillRoundedRectangle(x + 24.0f, 12.0f, 40.0f, 30.0f, 6.0f);
            g.setColour(p.text);
            g.drawImage(TrayIcon::iconImage(icons[i].first), juce::Rectangle<float>(x + 34.0f, 17.0f, 20.0f, 20.0f),
                        juce::RectanglePlacement::stretchToFit, true);
            g.setColour(p.textSecondary);
            g.drawText(icons[i].second, juce::Rectangle<float>(x, 50.0f, 88.0f, 16.0f), juce::Justification::centred);
        }
    }
};

} // namespace

std::unique_ptr<juce::Component> createTrayIconSheet() { return std::make_unique<TrayIconSheet>(); }

} // namespace fixomic::ui
