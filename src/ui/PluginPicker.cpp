#include "ui/PluginPicker.h"

#include "ui/ChainModel.h"

#include <algorithm>
#include <cmath>

namespace fixomic::ui {

namespace {

constexpr int kInset = Theme::kFlatTextInset;   // bounds past the content edge, each side
constexpr int kHeaderHeight = 24;
constexpr int kSearchHeight = 28;
constexpr int kGap = 8;
constexpr int kStatusGap = 6;
constexpr int kStatusMinHeight = 16;            // one caption line, kept even when empty
constexpr int kStatusMaxLines = 3;              // an error longer than this is elided
constexpr int kSearchLeftIndent = 26;           // room for the magnifier

juce::String identifierOf(const juce::PluginDescription& effect) { return effect.createIdentifierString(); }

bool sameEffects(const std::vector<juce::PluginDescription>& a, const std::vector<juce::PluginDescription>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
        return identifierOf(x) == identifierOf(y) && x.name == y.name && x.manufacturerName == y.manufacturerName;
    });
}

} // namespace

PluginPicker::PluginPicker() {
    setOpaque(false);
    setTitle("Aggiungi plugin");

    back_.onClick = [this] {
        if (onClose) onClose();
    };
    title_.setText("Aggiungi plugin");

    rescan_.setButtonText("Cerca nuovi plugin");
    rescan_.getProperties().set(Theme::kStyleProperty, Theme::kFlatStyle);
    rescan_.setMouseClickGrabsKeyboardFocus(false);
    rescan_.onClick = [this] {
        if (onRescan) onRescan();
    };

    search_.setTitle("Cerca plugin");
    search_.setMultiLine(false);
    search_.setJustification(juce::Justification::centredLeft);
    search_.setIndents(kSearchLeftIndent, 0);
    search_.setSelectAllWhenFocused(true);
    search_.onTextChange = [this] { searchChanged(); };
    search_.addKeyListener(this);

    list_.setModel(this);
    list_.setTitle("Plugin installati");
    list_.setRowHeight(kRowHeight);
    list_.setOutlineThickness(0);
    list_.setMouseMoveSelectsRows(true);
    list_.setWantsKeyboardFocus(false);   // typing stays in the search field; its keys drive the list
    list_.getViewport()->setScrollBarsShown(false, false, true, false);   // trackpad, no bar

    status_.setMaxLines(kStatusMaxLines);

    for (juce::Component* c : std::initializer_list<juce::Component*>{&back_, &title_, &rescan_, &search_, &list_,
                                                                      &status_})
        addAndMakeVisible(c);
    addChildComponent(message_);

    applyColours();
    refilter({});
    fitHeight();
}

PluginPicker::~PluginPicker() {
    search_.removeKeyListener(this);
    list_.setModel(nullptr);
}

// ---- data -------------------------------------------------------------------------------------

void PluginPicker::setEffects(std::vector<juce::PluginDescription> effects) {
    if (sameEffects(effects, effects_)) return;
    const int selected = list_.getSelectedRow();
    const auto keep = selected >= 0 && selected < static_cast<int>(listed_.size())
                          ? identifierOf(effects_[static_cast<size_t>(listed_[static_cast<size_t>(selected)])])
                          : juce::String();
    effects_ = std::move(effects);
    refilter(keep);
}

void PluginPicker::setScanning(bool scanning) {
    rescan_.setEnabled(!scanning);
    if (scanning == scanning_) return;
    scanning_ = scanning;
    updateMessages();
}

void PluginPicker::setSearchText(const juce::String& text) {
    search_.setText(text, false);
    searchChanged();
}

void PluginPicker::reset() {
    choosing_ = false;
    error_.clear();
    search_.setText({}, false);
    refilter({});
}

void PluginPicker::focusSearch() {
    if (isShowing()) search_.grabKeyboardFocus();
}

void PluginPicker::searchChanged() {
    error_.clear();   // a new search: the last failure is old news
    refilter({});
}

void PluginPicker::refilter(const juce::String& selectIdentifier) {
    listed_ = filterEffects(effects_, search_.getText());
    list_.updateContent();
    int select = listed_.empty() ? -1 : 0;
    for (size_t i = 0; i < listed_.size() && selectIdentifier.isNotEmpty(); ++i)
        if (identifierOf(effects_[static_cast<size_t>(listed_[i])]) == selectIdentifier) select = static_cast<int>(i);
    if (select >= 0)
        list_.selectRow(select);
    else
        list_.deselectAllRows();
    if (select <= 0) list_.getViewport()->setViewPosition(0, 0);
    list_.repaint();
    updateMessages();
}

void PluginPicker::updateMessages() {
    const auto messages = pickerMessages(static_cast<int>(effects_.size()), static_cast<int>(listed_.size()), scanning_);
    message_.setText(messages.list);
    message_.setVisible(messages.list.isNotEmpty());
    status_.setText(error_.isNotEmpty() ? error_ : messages.status);
    status_.setInk(error_.isNotEmpty() ? TextBlock::Ink::Problem : TextBlock::Ink::Secondary);
    fitHeight();
    resized();
    repaint();
}

std::vector<juce::PluginDescription> PluginPicker::listedEffects() const {
    std::vector<juce::PluginDescription> out;
    for (const int i : listed_) out.push_back(effects_[static_cast<size_t>(i)]);
    return out;
}

int PluginPicker::selectedIndex() const { return list_.getSelectedRow(); }

juce::String PluginPicker::listMessage() const { return message_.isVisible() ? message_.text() : juce::String(); }

juce::String PluginPicker::statusText() const { return status_.text(); }

bool PluginPicker::showsError() const { return error_.isNotEmpty(); }

// ---- choosing ---------------------------------------------------------------------------------

void PluginPicker::choose(int index) {
    choosing_ = false;
    if (index < 0 || index >= static_cast<int>(listed_.size())) return;
    chooseEffect(effects_[static_cast<size_t>(listed_[static_cast<size_t>(index)])]);
}

void PluginPicker::chooseEffect(const juce::PluginDescription& effect) {
    choosing_ = false;
    if (!onChoose) return;
    // The owner closes (and resets) the picker on success: nothing here may refer to the old list.
    error_ = onChoose(effect);
    updateMessages();
}

void PluginPicker::chooseSoon(int index) {
    if (choosing_ || index < 0 || index >= static_cast<int>(listed_.size())) return;
    choosing_ = true;
    // Out of the list's (or the editor's) event handler: adding a plugin may take a while and
    // closes this view.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PluginPicker>(this),
                                     effect = effects_[static_cast<size_t>(listed_[static_cast<size_t>(index)])]] {
        if (safe != nullptr && safe->choosing_) safe->chooseEffect(effect);
    });
}

// ---- ListBoxModel -----------------------------------------------------------------------------

int PluginPicker::getNumRows() { return static_cast<int>(listed_.size()); }

void PluginPicker::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= static_cast<int>(listed_.size())) return;
    const auto& effect = effects_[static_cast<size_t>(listed_[static_cast<size_t>(row)])];
    const auto& p = Theme::palette(*this);
    if (selected) {
        g.setColour(p.hover);
        g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height))
                                   .reduced(0.0f, 1.0f),
                               6.0f);
    }
    auto text = juce::Rectangle<int>(width, height).reduced(kInset, 0);
    const auto detail = effectDetail(effect);
    const auto detailFont = Theme::font(TextStyle::Caption);
    const int detailWidth = std::min(text.getWidth() * 45 / 100,
                                     juce::GlyphArrangement::getStringWidthInt(detailFont, detail) + 2);
    g.setFont(detailFont);
    g.setColour(p.textSecondary);
    g.drawText(detail, text.removeFromRight(detailWidth), juce::Justification::centredRight, true);
    text.removeFromRight(12);
    g.setFont(Theme::font(TextStyle::Body));
    g.setColour(p.text);
    g.drawText(effect.name, text, juce::Justification::centredLeft, true);
}

void PluginPicker::listBoxItemClicked(int row, const juce::MouseEvent&) { chooseSoon(row); }

void PluginPicker::returnKeyPressed(int lastRowSelected) { chooseSoon(lastRowSelected); }

juce::String PluginPicker::getNameForRow(int row) {
    if (row < 0 || row >= static_cast<int>(listed_.size())) return {};
    const auto& effect = effects_[static_cast<size_t>(listed_[static_cast<size_t>(row)])];
    return effect.name + ", " + effectDetail(effect);
}

// ---- keys -------------------------------------------------------------------------------------

bool PluginPicker::keyPressed(const juce::KeyPress& key, juce::Component*) {
    const int rows = static_cast<int>(listed_.size());
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
        if (rows > 0) {
            const int step = key == juce::KeyPress::upKey ? -1 : 1;
            list_.selectRow(juce::jlimit(0, rows - 1, list_.getSelectedRow() + step));
        }
        return true;
    }
    if (key == juce::KeyPress::returnKey) {
        chooseSoon(list_.getSelectedRow());
        return true;
    }
    if (key == juce::KeyPress::escapeKey) {
        if (onClose) onClose();
        return true;
    }
    return false;
}

bool PluginPicker::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (onClose) onClose();
        return true;
    }
    return false;
}

// ---- layout and paint -------------------------------------------------------------------------

int PluginPicker::listHeight() const { return static_cast<int>(std::lround(kVisibleRows * kRowHeight)); }

int PluginPicker::heightForWidth(int width) const {
    const int status = std::max(kStatusMinHeight, status_.heightForWidth(width - 2 * kInset));
    return kHeaderHeight + kGap + kSearchHeight + kGap + listHeight() + kStatusGap + status;
}

void PluginPicker::fitHeight() {
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));   // before the first layout: nothing to fit
}

void PluginPicker::resized() {
    const int width = getWidth();
    const int content = width - 2 * kInset;
    // Header: the chevron's stroke starts at the content edge, the rescan button's text ends at it.
    back_.setBounds(kInset - 8, 1, GlyphButton::kSize, GlyphButton::kSize);
    const int titleX = back_.getRight() + 2;
    const int rescanWidth =
        juce::GlyphArrangement::getStringWidthInt(Theme::font(TextStyle::Body), rescan_.getButtonText()) + 2 * kInset;
    rescan_.setBounds(width - rescanWidth, 0, rescanWidth, kHeaderHeight);
    title_.setBounds(titleX, 3, std::max(0, rescan_.getX() - 8 - titleX), kHeaderHeight - 3);

    int y = kHeaderHeight + kGap;
    search_.setBounds(kInset, y, content, kSearchHeight);
    y += kSearchHeight + kGap;
    list_.setBounds(0, y, width, listHeight());
    message_.setBounds(kInset, y + 24, content, message_.heightForWidth(content));
    y += listHeight() + kStatusGap;
    status_.setBounds(kInset, y, content, std::max(kStatusMinHeight, status_.heightForWidth(content)));
}

void PluginPicker::paintOverChildren(juce::Graphics& g) {
    // The search field's magnifier.
    const auto& p = Theme::palette(*this);
    const auto field = search_.getBounds().toFloat();
    const juce::Point<float> centre{field.getX() + 13.0f, field.getCentreY() - 1.0f};
    g.setColour(p.textTertiary);
    g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(centre), 1.5f);
    g.drawLine(centre.x + 3.3f, centre.y + 3.3f, centre.x + 6.5f, centre.y + 6.5f, 1.5f);
}

void PluginPicker::lookAndFeelChanged() { applyColours(); }

void PluginPicker::applyColours() {
    const auto& p = Theme::palette(*this);
    search_.applyFontToAllText(Theme::font(TextStyle::Body));
    search_.applyColourToAllText(p.text);
    search_.setTextToShowWhenEmpty("Cerca per nome o produttore", p.textTertiary);
    list_.repaint();
}

} // namespace fixomic::ui
