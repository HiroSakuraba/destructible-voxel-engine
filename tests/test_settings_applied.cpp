// Keeps SettingDefinition::applied honest. A setting marked applied must be read
// somewhere outside its definition, and one marked "not applied yet" must not be:
// wiring a setting up without updating kNotYetApplied (or adding a setting nothing
// reads) fails here. Usage: dve_settings_applied_tests <source root>
#include "dve/editor_settings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace dve::editor;

// Read into EditorPreferences, but nothing uses that preference field yet.
const std::map<std::string, std::string> kReadIntoUnusedPreference{
    {"accessibility.reduced_motion", "src/editor_workspace.cpp"},
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dve_settings_applied_tests <source root>\n";
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::vector<std::pair<std::string, std::string>> sources;  // relative path, contents
    for (const char* directory : {"src", "apps", "include"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / directory)) {
            if (!entry.is_regular_file()) continue;
            const auto extension = entry.path().extension();
            if (extension != ".cpp" && extension != ".hpp" && extension != ".h") continue;
            const std::string relative = entry.path().lexically_relative(root).generic_string();
            if (relative == "src/editor_settings.cpp") continue;  // the definitions themselves
            std::ifstream input(entry.path(), std::ios::binary);
            sources.emplace_back(relative, std::string(std::istreambuf_iterator<char>(input), {}));
        }
    }
    const auto registry = EditorSettingsRegistry::make_default();
    std::vector<std::string> neverRead, nowRead;
    std::size_t notApplied = 0;
    for (const SettingDefinition& definition : registry.definitions()) {
        const std::string quoted = "\"" + definition.id + "\"";
        std::set<std::string> readers;
        for (const auto& [path, text] : sources)
            if (text.find(quoted) != std::string::npos) readers.insert(path);
        if (definition.applied) {
            if (readers.empty()) neverRead.push_back(definition.id);
            continue;
        }
        ++notApplied;
        if (const auto allowed = kReadIntoUnusedPreference.find(definition.id); allowed != kReadIntoUnusedPreference.end())
            readers.erase(allowed->second);
        if (!readers.empty()) nowRead.push_back(definition.id + " (read in " + *readers.begin() + ")");
    }
    for (const auto& id : neverRead)
        std::cerr << "setting " << id << " is marked applied but nothing reads it; wire it up or add it to kNotYetApplied\n";
    for (const auto& id : nowRead)
        std::cerr << "setting " << id << " is now read; remove it from kNotYetApplied in src/editor_settings.cpp\n";
    if (!neverRead.empty() || !nowRead.empty()) return 1;
    std::cout << "dve_settings_applied_tests: PASS (" << registry.definitions().size() << " settings, " << notApplied
              << " not applied yet)\n";
    return 0;
}
