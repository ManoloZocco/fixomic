#include "dsp/PluginChain.h"

#include <algorithm>
#include <stdexcept>

namespace fixomic {

struct PluginChain::Slot {
    // Live slot. The listener stays registered until the slot is destroyed in collectGarbage().
    Slot(std::unique_ptr<juce::AudioPluginInstance> p, juce::AudioProcessorListener& l, bool isMono)
        : plugin(std::move(p)), listener(&l), mono(isMono), name(plugin->getName()) {
        plugin->addListener(listener);
    }

    // Missing slot: keeps what was saved so saveState() can return it verbatim.
    Slot(juce::String missingName, PluginSlot original) : name(std::move(missingName)), saved(std::move(original)) {}

    ~Slot() {
        if (plugin != nullptr) plugin->removeListener(listener);
    }

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    const std::unique_ptr<juce::AudioPluginInstance> plugin; // nullptr = missing
    juce::AudioProcessorListener* const listener = nullptr;
    const bool mono = false;                                 // processes channel 0 only
    const juce::String name;
    const PluginSlot saved;                                  // missing slots only
    std::atomic<bool> bypassed{false};                       // shared by every snapshot holding the slot
};

struct PluginChain::Snapshot {
    std::vector<std::shared_ptr<Slot>> slots;
};

namespace {

// Main buses set to `set`, every other bus disabled. False if the plugin refuses the layout.
bool trySetMainLayout(juce::AudioPluginInstance& plugin, const juce::AudioChannelSet& set) {
    if (plugin.getBusCount(true) < 1 || plugin.getBusCount(false) < 1) return false;
    auto layout = plugin.getBusesLayout();
    for (auto& bus : layout.inputBuses) bus = juce::AudioChannelSet::disabled();
    for (auto& bus : layout.outputBuses) bus = juce::AudioChannelSet::disabled();
    layout.inputBuses.getReference(0) = set;
    layout.outputBuses.getReference(0) = set;
    return plugin.setBusesLayout(layout);
}

} // namespace

PluginChain::PluginChain() : current_(new Snapshot) {}

PluginChain::~PluginChain() {
    retired_.clear();
    delete current_.exchange(nullptr);
}

const PluginChain::Snapshot& PluginChain::snapshot() const noexcept { return *current_.load(); }

void PluginChain::publish(std::unique_ptr<Snapshot> next) {
    retired_.reserve(retired_.size() + 1); // so nothing can throw once the swap has happened
    retired_.emplace_back(current_.exchange(next.release()));
}

void PluginChain::notifyChanged() const {
    if (onChanged) onChanged();
}

void PluginChain::preparePlugin(juce::AudioPluginInstance& plugin) const {
    const int blockSize = maxBlockSize_.load();
    if (blockSize <= 0) return;
    plugin.setRateAndBufferSizeDetails(sampleRate_, blockSize);
    plugin.prepareToPlay(sampleRate_, blockSize);
}

void PluginChain::prepare(double sampleRate, int maxBlockSize) {
    const bool valid = sampleRate > 0.0 && maxBlockSize > 0;
    sampleRate_ = valid ? sampleRate : 0.0;
    maxBlockSize_ = valid ? maxBlockSize : 0;
    emptyMidi_.ensureSize(4096); // room for MIDI a plugin emits into a later chunk
    for (const auto& slot : snapshot().slots)
        if (slot->plugin != nullptr) preparePlugin(*slot->plugin);
}

std::shared_ptr<PluginChain::Slot> PluginChain::makeSlot(std::unique_ptr<juce::AudioPluginInstance> plugin) {
    if (plugin == nullptr) throw std::invalid_argument("PluginChain: null plugin");

    // Bus layouts may only change while the plugin is inactive (a factory may hand it over prepared).
    plugin->releaseResources();
    bool mono = false;
    if (!trySetMainLayout(*plugin, juce::AudioChannelSet::stereo())) {
        if (!trySetMainLayout(*plugin, juce::AudioChannelSet::mono()))
            throw std::runtime_error("Plugin '" + plugin->getName().toStdString()
                                     + "' supports neither stereo nor mono input and output");
        mono = true;
    }
    preparePlugin(*plugin);
    juce::AudioProcessorListener& listener = *this; // private base: convert here, not in make_shared
    return std::make_shared<Slot>(std::move(plugin), listener, mono);
}

int PluginChain::add(std::unique_ptr<juce::AudioPluginInstance> plugin, int index) {
    auto slot = makeSlot(std::move(plugin));
    auto next = std::make_unique<Snapshot>(snapshot());
    const int size = static_cast<int>(next->slots.size());
    if (index < 0 || index > size) index = size;
    next->slots.insert(next->slots.begin() + index, std::move(slot));
    publish(std::move(next));
    notifyChanged();
    return index;
}

void PluginChain::remove(int index) {
    const auto& slots = snapshot().slots;
    if (index < 0 || index >= static_cast<int>(slots.size())) return;
    auto next = std::make_unique<Snapshot>(snapshot());
    next->slots.erase(next->slots.begin() + index);
    publish(std::move(next)); // the slot lives on in the retired snapshot until collectGarbage()
    notifyChanged();
}

void PluginChain::move(int from, int to) {
    const int size = static_cast<int>(snapshot().slots.size());
    if (from < 0 || from >= size) return;
    to = std::clamp(to, 0, size - 1);
    if (from == to) return;
    auto next = std::make_unique<Snapshot>(snapshot());
    auto slot = std::move(next->slots[static_cast<size_t>(from)]);
    next->slots.erase(next->slots.begin() + from);
    next->slots.insert(next->slots.begin() + to, std::move(slot));
    publish(std::move(next));
    notifyChanged();
}

void PluginChain::setBypassed(int index, bool bypassed) noexcept {
    const auto& slots = snapshot().slots;
    if (index < 0 || index >= static_cast<int>(slots.size())) return;
    slots[static_cast<size_t>(index)]->bypassed = bypassed;
    notifyChanged();
}

juce::AudioPluginInstance* PluginChain::pluginAt(int index) const {
    const auto& slots = snapshot().slots;
    if (index < 0 || index >= static_cast<int>(slots.size())) return nullptr;
    return slots[static_cast<size_t>(index)]->plugin.get();
}

std::vector<PluginChain::SlotInfo> PluginChain::slots() const {
    std::vector<SlotInfo> out;
    for (const auto& slot : snapshot().slots) {
        const bool missing = slot->plugin == nullptr;
        out.push_back({slot->name, slot->bypassed.load(), missing, missing ? 0 : slot->plugin->getLatencySamples()});
    }
    return out;
}

int PluginChain::totalLatencySamples() const {
    int total = 0;
    for (const auto& slot : snapshot().slots)
        if (slot->plugin != nullptr && !slot->bypassed.load()) total += slot->plugin->getLatencySamples();
    return total;
}

std::vector<PluginSlot> PluginChain::saveState() const {
    std::vector<PluginSlot> out;
    for (const auto& slot : snapshot().slots) {
        PluginSlot saved = slot->saved;
        if (slot->plugin != nullptr) {
            const auto format = juce::XmlElement::TextFormat().singleLine().withoutHeader();
            saved.descriptionXml = slot->plugin->getPluginDescription().createXml()->toString(format);
            slot->plugin->getStateInformation(saved.state);
        }
        saved.bypassed = slot->bypassed.load();
        out.push_back(std::move(saved));
    }
    return out;
}

void PluginChain::flushPluginParameters() {
    if (maxBlockSize_.load() <= 0) return;
    juce::MidiBuffer midi;
    for (const auto& slot : snapshot().slots) {
        auto* plugin = slot->plugin.get();
        if (plugin == nullptr || plugin->getVST3Client() == nullptr) continue;
        const int channels = std::max(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        juce::AudioBuffer<float> empty(std::max(channels, 1), 0);
        plugin->processBlock(empty, midi);
        midi.clear();
    }
}

std::vector<juce::String> PluginChain::restore(const std::vector<PluginSlot>& saved, const PluginFactory& factory) {
    std::vector<juce::String> errors;
    auto next = std::make_unique<Snapshot>();

    for (size_t i = 0; i < saved.size(); ++i) {
        const PluginSlot& entry = saved[i];
        juce::PluginDescription description;
        juce::String error;
        std::shared_ptr<Slot> slot;

        const auto xml = juce::parseXML(entry.descriptionXml);
        if (xml == nullptr || !description.loadFromXml(*xml)) {
            error = "invalid plugin description";
        } else if (auto instance = factory(description, sampleRate_, maxBlockSize_.load(), error)) {
            // Same order as JUCE's AudioPluginHost: state, then bus layout, then prepare.
            if (!entry.state.isEmpty())
                instance->setStateInformation(entry.state.getData(), static_cast<int>(entry.state.getSize()));
            try {
                slot = makeSlot(std::move(instance));
            } catch (const std::exception& e) {
                error = e.what();
            }
        } else if (error.isEmpty()) {
            error = "could not be loaded";
        }

        if (slot == nullptr) {
            const juce::String name = description.name.isNotEmpty() ? description.name
                                                                    : "Plugin " + juce::String(static_cast<int>(i) + 1);
            errors.push_back(name + ": " + error);
            slot = std::make_shared<Slot>(name, entry);
        }
        slot->bypassed = entry.bypassed;
        next->slots.push_back(std::move(slot));
    }

    publish(std::move(next));
    notifyChanged();
    return errors;
}

void PluginChain::collectGarbage() {
    Snapshot* const busy = inUse_.load();
    std::erase_if(retired_, [busy](const std::unique_ptr<Snapshot>& s) { return s.get() != busy; });
    if (latencyChangedPending_.exchange(false)) notifyChanged();
    if (stateChangedPending_.exchange(false) && onPluginStateChanged) onPluginStateChanged();
}

void PluginChain::audioProcessorParameterChanged(juce::AudioProcessor*, int, float) {
    stateChangedPending_ = true;   // any thread: lock-free only
}

void PluginChain::audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) {
    // May run on any thread, the audio thread included: only lock-free work off the message thread.
    if (details.programChanged || details.nonParameterStateChanged) stateChangedPending_ = true;
    if (!details.latencyChanged) return;
    if (juce::MessageManager::existsAndIsCurrentThread())
        notifyChanged();
    else
        latencyChangedPending_ = true;
}

void PluginChain::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept {
    Snapshot* s;
    do {
        s = current_.load();
        inUse_.store(s);
    } while (current_.load() != s);

    const int maxBlock = maxBlockSize_.load();
    const int total = buffer.getNumSamples();
    if (maxBlock > 0 && buffer.getNumChannels() >= 2) {
        float* const* channels = buffer.getArrayOfWritePointers();
        for (int start = 0; start < total; start += maxBlock) {
            const int n = std::min(maxBlock, total - start);
            // Incoming MIDI belongs to the first chunk; later chunks start from an empty buffer
            // (clear() keeps its storage).
            if (start > 0) emptyMidi_.clear();
            juce::MidiBuffer& chunkMidi = start == 0 ? midi : emptyMidi_;
            for (const auto& slot : s->slots) {
                if (slot->plugin == nullptr || slot->bypassed.load()) continue;
                // Views on the caller's channels: no allocation for <= 32 channels.
                juce::AudioBuffer<float> view(channels, slot->mono ? 1 : 2, start, n);
                slot->plugin->processBlock(view, chunkMidi);
                if (slot->mono) juce::FloatVectorOperations::copy(channels[1] + start, channels[0] + start, n);
            }
        }
    }

    inUse_.store(nullptr);
}

} // namespace fixomic
