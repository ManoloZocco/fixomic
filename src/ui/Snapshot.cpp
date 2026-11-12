#include "ui/Snapshot.h"

#include "ui/Theme.h"

namespace fixomic::ui {

std::vector<juce::File> writeSnapshots(const juce::File& directory, const std::vector<SnapshotTarget>& targets,
                                       juce::String& error) {
    std::vector<juce::File> written;
    if (const auto created = directory.createDirectory(); created.failed()) {
        error = "cannot create " + directory.getFullPathName() + ": " + created.getErrorMessage();
        return written;
    }
    const std::pair<Appearance, const char*> appearances[] = {{Appearance::Light, "light"}, {Appearance::Dark, "dark"}};
    for (const auto& [appearance, suffix] : appearances) {
        Theme theme(appearance);
        for (const auto& target : targets) {
            if (target.component == nullptr) continue;
            auto& component = *target.component;
            component.setLookAndFeel(&theme);
            const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 2.0f);
            component.setLookAndFeel(nullptr);

            const auto file = directory.getChildFile(target.name + "-" + suffix + ".png");
            file.deleteFile();
            juce::FileOutputStream stream(file);
            juce::PNGImageFormat png;
            if (!stream.openedOk() || !png.writeImageToStream(image, stream)) {
                error = "cannot write " + file.getFullPathName();
                return written;
            }
            written.push_back(file);
        }
    }
    return written;
}

} // namespace fixomic::ui
