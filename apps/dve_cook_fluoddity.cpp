#include <filesystem>
#include <iostream>
#include <string>

#include "dve/fluoddity.hpp"

namespace {
void usage() {
    std::cout
        << "dve_cook_fluoddity --input PRESET.json --output RULE.dfluoddity\n"
        << "dve_cook_fluoddity --input-dir PRESET_DIR --output-dir COOKED_DIR\n";
}
}

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path inputDirectory;
    std::filesystem::path outputDirectory;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--input" && index + 1 < argc) input = argv[++index];
        else if (argument == "--output" && index + 1 < argc) output = argv[++index];
        else if (argument == "--input-dir" && index + 1 < argc) inputDirectory = argv[++index];
        else if (argument == "--output-dir" && index + 1 < argc) outputDirectory = argv[++index];
        else if (argument == "--help") { usage(); return 0; }
        else { std::cerr << "Unknown or incomplete argument: " << argument << '\n'; usage(); return 2; }
    }

    if (!input.empty() || !output.empty()) {
        if (input.empty() || output.empty() || !inputDirectory.empty() || !outputDirectory.empty()) {
            std::cerr << "Single-preset mode requires exactly --input and --output\n";
            return 2;
        }
        dve::FluoddityImportResult imported = dve::import_fluoddity_preset_json(input);
        if (!imported) { std::cerr << imported.error << '\n'; return 1; }
        std::error_code ec;
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path(), ec);
        std::string error;
        if (!dve::write_dfluoddity(output, *imported.asset, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const dve::FluoddityImportResult verified = dve::read_dfluoddity(output);
        if (!verified || verified.asset->contentHash != imported.asset->contentHash) {
            std::cerr << "post-write verification failed";
            if (!verified.error.empty()) std::cerr << ": " << verified.error;
            std::cerr << '\n';
            return 1;
        }
        for (const std::string& warning : imported.warnings) std::cerr << "warning: " << warning << '\n';
        std::cout << "name=" << imported.asset->name
                  << " cohorts=" << imported.asset->cohortCount
                  << " hash=" << imported.asset->contentHash << '\n';
        return 0;
    }

    if (inputDirectory.empty() || outputDirectory.empty()) {
        usage();
        return 2;
    }
    dve::FluoddityPresetBatchResult batch =
        dve::import_fluoddity_preset_directory(inputDirectory);
    for (const std::string& warning : batch.warnings) std::cerr << "warning: " << warning << '\n';
    for (const std::string& error : batch.errors) std::cerr << "error: " << error << '\n';

    std::size_t written{};
    for (const dve::FluoddityRuleAsset& asset : batch.assets) {
        std::filesystem::path relative = asset.sourcePreset;
        if (relative.empty() || relative.is_absolute() || relative.generic_string().starts_with("..")) {
            relative = std::filesystem::path(asset.name + ".json");
        }
        relative.replace_extension(".dfluoddity");
        const std::filesystem::path destination = outputDirectory / relative;
        std::error_code directoryError;
        std::filesystem::create_directories(destination.parent_path(), directoryError);
        if (directoryError) {
            std::cerr << "error: unable to create " << destination.parent_path() << ": "
                      << directoryError.message() << '\n';
            continue;
        }
        std::string error;
        if (!dve::write_dfluoddity(destination, asset, &error)) {
            std::cerr << "error: " << destination << ": " << error << '\n';
            continue;
        }
        const dve::FluoddityImportResult verified = dve::read_dfluoddity(destination);
        if (!verified || verified.asset->contentHash != asset.contentHash) {
            std::cerr << "error: " << destination << ": post-write verification failed";
            if (!verified.error.empty()) std::cerr << ": " << verified.error;
            std::cerr << '\n';
            continue;
        }
        ++written;
    }
    std::cout << "imported=" << batch.assets.size() << " written=" << written
              << " warnings=" << batch.warnings.size() << " errors=" << batch.errors.size() << '\n';
    return batch.errors.empty() && written == batch.assets.size() ? 0 : 1;
}
