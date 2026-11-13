#include "Config.h"

namespace fixomic {

namespace {

constexpr auto kRootTag = "FixomicConfig";
constexpr auto kPluginTag = "Plugin";

bool isValidBufferSize(int size) {
    return size == 64 || size == 128 || size == 256;
}

} // namespace

juce::String toXmlString(const Config& config) {
    juce::XmlElement root(kRootTag);
    root.setAttribute("version", config.version);
    root.setAttribute("inputDeviceUid", config.inputDeviceUid);
    root.setAttribute("inputDeviceName", config.inputDeviceName);
    root.setAttribute("inputChannel", config.inputChannel);
    root.setAttribute("bufferSize", config.bufferSize);
    root.setAttribute("aecEnabled", config.aecEnabled);
    root.setAttribute("globalBypass", config.globalBypass);
    root.setAttribute("launchAtLogin", config.launchAtLogin);
    root.setAttribute("onboardingDone", config.onboardingDone);

    for (const auto& slot : config.chain) {
        auto* plugin = root.createNewChildElement(kPluginTag);
        plugin->setAttribute("description", slot.descriptionXml);
        plugin->setAttribute("state", slot.state.toBase64Encoding());
        plugin->setAttribute("bypassed", slot.bypassed);
    }

    return root.toString();
}

std::optional<Config> fromXmlString(const juce::String& text) {
    auto parsed = parseConfig(text);
    if (!parsed) return std::nullopt;
    return std::move(parsed->config);
}

std::optional<ParsedConfig> parseConfig(const juce::String& text) {
    const auto root = juce::XmlDocument::parse(text);
    if (root == nullptr || !root->hasTagName(kRootTag))
        return std::nullopt;

    const Config defaults;
    Config config;
    config.version = root->getIntAttribute("version", defaults.version);
    config.inputDeviceUid = root->getStringAttribute("inputDeviceUid", defaults.inputDeviceUid);
    config.inputDeviceName = root->getStringAttribute("inputDeviceName", defaults.inputDeviceName);

    const auto channel = root->getIntAttribute("inputChannel", defaults.inputChannel);
    config.inputChannel = channel >= 0 ? channel : defaults.inputChannel;

    const auto bufferSize = root->getIntAttribute("bufferSize", defaults.bufferSize);
    config.bufferSize = isValidBufferSize(bufferSize) ? bufferSize : defaults.bufferSize;

    config.aecEnabled = root->getBoolAttribute("aecEnabled", defaults.aecEnabled);
    config.globalBypass = root->getBoolAttribute("globalBypass", defaults.globalBypass);
    config.launchAtLogin = root->getBoolAttribute("launchAtLogin", defaults.launchAtLogin);

    for (const auto* plugin : root->getChildWithTagNameIterator(kPluginTag)) {
        PluginSlot slot;
        slot.descriptionXml = plugin->getStringAttribute("description");
        slot.state.fromBase64Encoding(plugin->getStringAttribute("state"));
        slot.bypassed = plugin->getBoolAttribute("bypassed", false);
        config.chain.push_back(std::move(slot));
    }

    // Written before the flag existed: onboarded if it was set up (a microphone or a chain).
    const bool flagStored = root->hasAttribute("onboardingDone");
    config.onboardingDone = flagStored ? root->getBoolAttribute("onboardingDone")
                                       : config.inputDeviceUid.isNotEmpty() || !config.chain.empty();
    return ParsedConfig{std::move(config), flagStored};
}

} // namespace fixomic
