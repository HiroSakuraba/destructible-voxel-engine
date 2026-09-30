#include "dve/v235_foundations.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: dve_pack <project-root> <output.dvepak> <file> [file ...]\n"
                     "       dve_pack <project-root> <output.dvepak> --all\n";
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path output = argv[2];
    std::vector<std::filesystem::path> inputs;
    const dve::DvePakBuildOptions options{};
    if (std::string_view(argv[3]) == "--all") {
        std::error_code error;
        const auto outputAbsolute = std::filesystem::absolute(output, error).lexically_normal();
        if (error) {
            std::cerr << "output path failed: " << error.message() << '\n';
            return 1;
        }
        for (std::filesystem::recursive_directory_iterator it(root, error), end;
             it != end && !error; it.increment(error)) {
            // Do not even descend into editor-only folders such as `.autosave/`;
            // build_dvepak would strip their files anyway, this just avoids reading them.
            if (options.stripEditorOnly && it->is_directory(error) &&
                std::find(options.editorOnlyDirectoryNames.begin(), options.editorOnlyDirectoryNames.end(),
                          it->path().filename().generic_string()) != options.editorOnlyDirectoryNames.end()) {
                it.disable_recursion_pending();
                continue;
            }
            if (error) break;
            if (!it->is_regular_file(error)) continue;
            const auto candidateAbsolute = std::filesystem::absolute(it->path(), error).lexically_normal();
            if (error) break;
            if (candidateAbsolute == outputAbsolute) continue;
            const auto relative = std::filesystem::relative(it->path(), root, error);
            if (!error) inputs.push_back(relative);
        }
        if (error) {
            std::cerr << "scan failed: " << error.message() << '\n';
            return 1;
        }
    } else {
        for (int index = 3; index < argc; ++index) inputs.emplace_back(argv[index]);
    }
    dve::DvePakManifest manifest;
    std::string error;
    if (!dve::build_dvepak(root, inputs, output, options, &manifest, &error)) {
        std::cerr << "package failed: " << error << '\n';
        return 1;
    }
    std::cout << "published " << output.generic_string() << " with "
              << manifest.entries.size() << " files; hash=" << manifest.packageHash << '\n';
    return 0;
}
