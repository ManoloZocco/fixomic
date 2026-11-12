#pragma once

#include "Config.h"

namespace fixomic {

class ConfigStore {
public:
    explicit ConfigStore(juce::File file);

    // ~/Library/Application Support/fixomic/config.xml
    static juce::File defaultLocation();

    // Missing or corrupt file -> Config{}. A corrupt file is renamed to <file>.bak first.
    Config load() const;

    // Writes to a temporary file next to the target, then renames it over the target.
    bool save(const Config&) const;

    // save(), unless the file already holds exactly this config (no rewrite, mtime untouched).
    enum class SaveResult { Written, Unchanged, Failed };
    SaveResult saveIfChanged(const Config&) const;

    const juce::File& file() const { return file_; }

private:
    juce::File file_;
};

} // namespace fixomic
