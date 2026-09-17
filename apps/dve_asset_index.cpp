#include "dve/editor_asset_browser.hpp"

#include <filesystem>
#include <iostream>
#include <string>

using namespace dve::editor;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dve_asset_index <project-root> [search]\n";
        return 2;
    }
    EditorAssetDatabase database{std::filesystem::path(argv[1])};
    EditorAssetScanReport report;
    std::string error;
    if (!database.scan(&report, &error)) {
        std::cerr << "asset scan failed: " << error << '\n';
        return 1;
    }
    EditorAssetQuery query;
    if (argc >= 3) query.text = argv[2];
    const auto rows = database.query(query);
    std::cout << "indexed=" << report.indexed << " added=" << report.added
              << " changed=" << report.changed << " moved=" << report.moved
              << " removed=" << report.removed << " thumbnails=" << report.thumbnailsGenerated << '\n';
    for (const auto* row : rows) {
        std::cout << row->id << '\t' << to_string(row->kind) << '\t'
                  << to_string(row->health) << '\t' << row->relativePath.generic_string()
                  << "\tgen=" << row->generation << "\tdeps=" << row->dependencies.size() << '\n';
    }
    return 0;
}
