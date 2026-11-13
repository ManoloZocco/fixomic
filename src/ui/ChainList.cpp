#include "ui/ChainList.h"

#include "ui/ChainModel.h"

#include <algorithm>
#include <utility>

namespace fixomic::ui {

namespace {

constexpr int kInset = Theme::kFlatTextInset;   // bounds past the content edge, each side
constexpr int kHandleWidth = 14;                // the drag handle's column
constexpr int kNameGap = 6;                     // handle to name, name to its hover fill edge
constexpr int kSwitchWidth = 40;                // as MainPanel's switches
constexpr int kSwitchHeight = 24;
constexpr int kAddButtonHeight = 28;
constexpr int kDragThreshold = 4;               // pt before a press becomes a drag
constexpr float kDraggedAlpha = 0.35f;
constexpr int kAutoRepeatMs = 40;               // mouseDrag while still, for the auto-scroll

} // namespace

// ---- Row --------------------------------------------------------------------------------------

// One slot. Paints the handle, the name and the secondary text; the switch and the remove button
// are its children. Presses outside them start a drag (reported to the list) or, released on the
// name without dragging, open the editor. A press whose drag was cancelled (Escape, a chain change)
// does nothing more until the button is released: the row may show another slot by then.
class ChainList::Row : public juce::Component {
public:
    explicit Row(ChainList& owner) : owner_(owner), remove_(GlyphButton::Glyph::Remove) {
        switch_.setMouseClickGrabsKeyboardFocus(false);
        switch_.onClick = [this] { owner_.bypassClicked(index_, !switch_.getToggleState()); };
        remove_.onClick = [this] { owner_.removeClicked(index_); };
        addChildComponent(switch_);
        addAndMakeVisible(remove_);
    }

    void set(int index, const PluginChain::SlotInfo& slot) {
        index_ = index;
        slot_ = slot;
        detail_ = slotDetail(slot);
        switch_.setButtonText("Attiva " + slot.name);
        switch_.setToggleState(!slot.bypassed, juce::dontSendNotification);
        switch_.setVisible(!slot.missing);
        remove_.setButtonText("Rimuovi " + slot.name);
        setTitle(slot.name);
        setDescription(detail_);
        hover_ = Zone::None;
        updateCursor();
        resized();
        repaint();
    }

    // The list stopped this row's drag, or the chain changed under a press that had not become a
    // drag yet: the rest of the press is ignored.
    void cancelPress() {
        dragging_ = false;
        pressed_ = false;
        pressCancelled_ = true;
        updateCursor();
        repaint();
    }

    void resized() override {
        const int y = (getHeight() - GlyphButton::kSize) / 2;
        remove_.setBounds(getWidth() - GlyphButton::kSize, y, GlyphButton::kSize, GlyphButton::kSize);
        switch_.setBounds(remove_.getX() - 4 - kSwitchWidth, (getHeight() - kSwitchHeight) / 2, kSwitchWidth,
                          kSwitchHeight);
    }

    void paint(juce::Graphics& g) override {
        const auto& p = Theme::palette(*this);
        const float cy = static_cast<float>(getHeight()) / 2.0f;

        // Drag handle: two columns of three dots.
        g.setColour(p.textTertiary);
        for (const float x : {3.5f, 8.5f})
            for (const float dy : {-4.5f, 0.0f, 4.5f})
                g.fillEllipse(juce::Rectangle<float>(2.0f, 2.0f).withCentre({x, cy + dy}));

        const auto text = textArea();
        const int detail = detailWidth();
        const auto name = nameArea();
        if (hover_ == Zone::Name && !slot_.missing && owner_.dragFrom_ < 0) {
            g.setColour(pressed_ ? p.separator : p.hover);
            g.fillRoundedRectangle(name.expanded(kNameGap, 0).toFloat().withTrimmedTop(4.0f).withTrimmedBottom(4.0f),
                                   6.0f);
        }
        g.setFont(Theme::font(TextStyle::Body));
        g.setColour(slot_.missing ? p.textTertiary : slot_.bypassed ? p.textSecondary : p.text);
        g.drawText(slot_.name, name, juce::Justification::centredLeft, true);

        if (detail > 0) {
            g.setFont(Theme::font(TextStyle::Caption));
            g.setColour(p.textSecondary);
            g.drawText(detail_, text.withTrimmedLeft(text.getWidth() - detail), juce::Justification::centredRight, true);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHover(zoneAt(e.position)); }
    void mouseEnter(const juce::MouseEvent& e) override { setHover(zoneAt(e.position)); }
    void mouseExit(const juce::MouseEvent&) override { setHover(Zone::None); }

    void mouseDown(const juce::MouseEvent&) override {
        pressed_ = true;
        pressCancelled_ = false;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging_) {
            if (pressCancelled_ || e.getDistanceFromDragStart() < kDragThreshold) return;
            dragging_ = owner_.beginDrag(index_);
            if (!dragging_) return;
            pressed_ = false;
            updateCursor();
        }
        owner_.dragMoved(e.getEventRelativeTo(&owner_).position);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        pressed_ = false;
        if (dragging_) {
            dragging_ = false;
            updateCursor();
            owner_.endDrag(e.getEventRelativeTo(&owner_).position);   // the last call: may edit the chain
            return;
        }
        repaint();
        if (pressCancelled_) return;
        if (!e.mouseWasDraggedSinceMouseDown() && zoneAt(e.position) == Zone::Name && !slot_.missing)
            owner_.openEditorClicked(index_);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        // A group named after the plugin (its children: the switch and the remove button); pressing
        // it opens the editor, as a click on the name does.
        juce::AccessibilityActions actions;
        actions.addAction(juce::AccessibilityActionType::press, [safe = juce::Component::SafePointer<Row>(this)] {
            if (safe != nullptr && !safe->slot_.missing) safe->owner_.openEditorClicked(safe->index_);
        });
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group, std::move(actions));
    }

private:
    enum class Zone { None, Handle, Name };

    // Between the handle and the switch (the remove button when there is no switch).
    juce::Rectangle<int> textArea() const {
        const int right = (switch_.isVisible() ? switch_.getX() : remove_.getX()) - 8;
        return {kHandleWidth + kNameGap, 0, std::max(0, right - kHandleWidth - kNameGap), getHeight()};
    }

    // The secondary text's width at the right of textArea(): at most half of it.
    int detailWidth() const {
        if (detail_.isEmpty()) return 0;
        return std::min(textArea().getWidth() / 2,
                        juce::GlyphArrangement::getStringWidthInt(Theme::font(TextStyle::Caption), detail_) + 2);
    }

    // The name's text, as wide as it is drawn (the hover fill hugs it).
    juce::Rectangle<int> nameArea() const {
        const auto text = textArea();
        const int detail = detailWidth();
        const int available = std::max(0, text.getWidth() - (detail > 0 ? detail + 12 : 0));
        const int nameWidth = std::min(available, juce::GlyphArrangement::getStringWidthInt(
                                                      Theme::font(TextStyle::Body), slot_.name)
                                                      + 1);
        return text.withWidth(nameWidth);
    }

    Zone zoneAt(juce::Point<float> position) const {
        if (position.x < static_cast<float>(kHandleWidth)) return Zone::Handle;
        if (nameArea().expanded(kNameGap, 0).toFloat().contains(position)) return Zone::Name;
        return Zone::None;
    }

    void setHover(Zone zone) {
        if (zone == hover_) return;
        hover_ = zone;
        updateCursor();
        repaint();
    }

    void updateCursor() {
        if (dragging_ || hover_ == Zone::Handle)
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        else if (hover_ == Zone::Name && !slot_.missing)
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    ChainList& owner_;
    int index_ = -1;
    PluginChain::SlotInfo slot_{};
    juce::String detail_;
    juce::ToggleButton switch_;
    GlyphButton remove_;
    Zone hover_ = Zone::None;
    bool pressed_ = false;
    bool dragging_ = false;
    bool pressCancelled_ = false;   // its drag was cancelled: ignore the rest of this press
};

// ---- ChainList --------------------------------------------------------------------------------

ChainList::ChainList() {
    setOpaque(false);
    setTitle("Catena dei plugin");
    emptyText_.setText("Nessun plugin: aggiungine uno per trattare la voce.");
    addAndMakeVisible(emptyText_);

    addButton_.setButtonText("+ Aggiungi plugin");
    addButton_.getProperties().set(Theme::kStyleProperty, Theme::kFlatStyle);
    addButton_.setMouseClickGrabsKeyboardFocus(false);
    addButton_.onClick = [this] {
        if (onAdd) onAdd();
    };
    addAndMakeVisible(addButton_);
    fitHeight();
}

ChainList::~ChainList() { stopDrag(); }

void ChainList::setSlots(std::vector<PluginChain::SlotInfo> slots) {
    // After an edit the rows are refilled even if the chain did not change (an edit that did
    // nothing): a switch flipped by the click goes back to the slot's state.
    const bool afterEdit = std::exchange(awaitingSlots_, false);
    if (slots == slots_ && !afterEdit) return;
    stopDrag();   // the indices it was about may be gone
    // So may the slot under a press that has not become a drag yet (a click on its name included).
    for (auto& row : rows_) row->cancelPress();
    slots_ = std::move(slots);
    // Rows are kept and refilled by index: only the surplus is destroyed.
    while (rows_.size() > slots_.size()) rows_.pop_back();
    while (rows_.size() < slots_.size()) {
        rows_.push_back(std::make_unique<Row>(*this));
        addAndMakeVisible(*rows_.back());
    }
    for (size_t i = 0; i < slots_.size(); ++i) rows_[i]->set(static_cast<int>(i), slots_[i]);
    emptyText_.setVisible(slots_.empty());
    fitHeight();
    resized();
    repaint();
}

const std::vector<PluginChain::SlotInfo>& ChainList::slots() const { return slots_; }

int ChainList::heightForWidth(int width) const {
    const int body = slots_.empty() ? emptyText_.heightForWidth(width - 2 * kInset) + 6
                                    : static_cast<int>(slots_.size()) * kRowHeight + 2;
    return body + kAddButtonHeight;
}

void ChainList::fitHeight() {
    if (getWidth() > 0) setSize(getWidth(), heightForWidth(getWidth()));   // before the first layout: nothing to fit
}

juce::Rectangle<int> ChainList::content() const { return getLocalBounds().reduced(kInset, 0); }

juce::Rectangle<int> ChainList::rowBounds(int index) const {
    return content().withY(index * kRowHeight).withHeight(kRowHeight);
}

juce::Component* ChainList::row(int index) const {
    return index >= 0 && index < static_cast<int>(rows_.size()) ? rows_[static_cast<size_t>(index)].get() : nullptr;
}

juce::Button& ChainList::addButton() { return addButton_; }

void ChainList::resized() {
    for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->setBounds(rowBounds(static_cast<int>(i)));
    int y = 0;
    if (slots_.empty()) {
        const int h = emptyText_.heightForWidth(content().getWidth());
        emptyText_.setBounds(content().withHeight(h));
        y = h + 6;
    } else {
        y = static_cast<int>(slots_.size()) * kRowHeight + 2;
    }
    const int width = juce::GlyphArrangement::getStringWidthInt(Theme::font(TextStyle::Body), addButton_.getButtonText())
                      + 2 * kInset;
    addButton_.setBounds(0, y, width, kAddButtonHeight);
}

void ChainList::paintOverChildren(juce::Graphics& g) {
    const auto gap = dropGap();
    const auto from = draggedRow();
    if (!gap || !from || !moveTarget(*from, *gap, static_cast<int>(slots_.size()))) return;
    const auto& p = Theme::palette(*this);
    const auto area = content().toFloat();
    // The boundary between two rows, kept inside the list so the ring is never clipped.
    const float y = juce::jlimit(4.0f, static_cast<float>(slots_.size() * kRowHeight) - 4.0f,
                                 static_cast<float>(*gap * kRowHeight));
    const float ringX = area.getX() + 6.0f;
    g.setColour(p.text);
    g.drawEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre({ringX, y}), 2.0f);
    g.fillRoundedRectangle(juce::Rectangle<float>(ringX + 3.5f, y - 1.0f, area.getRight() - ringX - 3.5f, 2.0f), 1.0f);
}

std::optional<int> ChainList::draggedRow() const {
    return dragFrom_ >= 0 ? std::optional<int>(dragFrom_) : std::nullopt;
}

std::optional<int> ChainList::dropGap() const {
    return dragFrom_ >= 0 && dragGap_ >= 0 ? std::optional<int>(dragGap_) : std::nullopt;
}

bool ChainList::inDropZone(juce::Point<float> position) const {
    // The list, widened to the panel's edges and by a row above and below it.
    const auto zone = getLocalBounds().toFloat().expanded(static_cast<float>(kInset), static_cast<float>(kRowHeight));
    return zone.contains(position);
}

bool ChainList::beginDrag(int index) {
    if (awaitingSlots_ || index < 0 || index >= static_cast<int>(rows_.size())) return false;
    dragFrom_ = index;
    dragGap_ = index;
    rows_[static_cast<size_t>(index)]->setAlpha(kDraggedAlpha);
    juce::Desktop::getInstance().beginDragAutoRepeat(kAutoRepeatMs);
    repaint();
    return true;
}

void ChainList::dragMoved(juce::Point<float> position) {
    if (dragFrom_ < 0) return;
    const int gap = inDropZone(position)
                        ? insertionGap(position.y, static_cast<float>(kRowHeight), static_cast<int>(slots_.size()))
                        : -1;   // released here, the drag is cancelled: no drop line
    if (gap != dragGap_) {
        dragGap_ = gap;
        repaint();
    }
    if (auto* viewport = findParentComponentOfClass<juce::Viewport>()) {
        const auto inViewport = viewport->getLocalPoint(this, position.toInt());
        viewport->autoScroll(inViewport.x, inViewport.y, 24, 12);
    }
}

void ChainList::endDrag(juce::Point<float> position) {
    if (dragFrom_ < 0) return;
    const int from = dragFrom_;
    const int count = static_cast<int>(slots_.size());
    const auto to = inDropZone(position)
                        ? moveTarget(from, insertionGap(position.y, static_cast<float>(kRowHeight), count), count)
                        : std::nullopt;   // released away from the list: cancelled
    stopDrag();
    if (!to) return;
    awaitingSlots_ = true;
    if (onMove) onMove(from, *to);
}

bool ChainList::cancelDrag() {
    if (dragFrom_ < 0) return false;
    stopDrag();
    return true;
}

void ChainList::stopDrag() {
    if (dragFrom_ < 0) return;
    if (dragFrom_ < static_cast<int>(rows_.size())) {
        auto& dragged = *rows_[static_cast<size_t>(dragFrom_)];
        dragged.setAlpha(1.0f);
        dragged.cancelPress();
    }
    dragFrom_ = dragGap_ = -1;
    juce::Desktop::getInstance().beginDragAutoRepeat(0);
    repaint();
}

void ChainList::bypassClicked(int index, bool bypassed) {
    if (awaitingSlots_ || dragFrom_ >= 0) {
        // Ignored: the switch goes back to the slot's state.
        if (index >= 0 && index < static_cast<int>(rows_.size()))
            rows_[static_cast<size_t>(index)]->set(index, slots_[static_cast<size_t>(index)]);
        return;
    }
    awaitingSlots_ = true;
    if (onBypassChanged) onBypassChanged(index, bypassed);
}

void ChainList::removeClicked(int index) {
    if (awaitingSlots_ || dragFrom_ >= 0) return;
    awaitingSlots_ = true;
    if (onRemove) onRemove(index);
}

void ChainList::openEditorClicked(int index) {
    if (awaitingSlots_ || dragFrom_ >= 0) return;
    if (onOpenEditor) onOpenEditor(index);
}

} // namespace fixomic::ui
