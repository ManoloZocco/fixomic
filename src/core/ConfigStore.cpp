#include "ConfigStore.h"

namespace fixomic {

ConfigStore::ConfigStore(juce::File file) : file_(std::move(file)) {}

juce::File ConfigStore::defaultLocation() {
    // userApplicationDataDirectory is ~/Library on macOS.
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Application Support")
        .getChildFile("fixomic")
        .getChildFile("config.xml");
}

Config ConfigStore::load() const {
    if (!file_.existsAsFile())
        return {};

    if (auto parsed = fromXmlString(file_.loadFileAsString()))
        return *std::move(parsed);

    // Corrupt: keep the evidence next to the original, then start from defaults.
    const juce::File backup(file_.getFullPathName() + ".bak");
    backup.deleteFile();
    file_.moveFileTo(backup);
    return {};
}

bool ConfigStore::save(const Config& config) const {
    if (!file_.getParentDirectory().createDirectory())
        return false;

    juce::TemporaryFile temp(file_);
    if (!temp.getFile().replaceWithText(toXmlString(config)))
        return false;

    // Rename over the target; the temporary file is cleaned up by its destructor on failure.
    return temp.overwriteTargetFileWithTemporary();
}

ConfigStore::SaveResult ConfigStore::saveIfChanged(const Config& config) const {
    if (file_.existsAsFile() && file_.loadFileAsString() == toXmlString(config)) return SaveResult::Unchanged;
    return save(config) ? SaveResult::Written : SaveResult::Failed;
}

} // namespace fixomic
