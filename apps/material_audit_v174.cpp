#include "dve/material_authoring.hpp"
#include "dve/polygon_asset.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_usage() {
    std::cerr << "Usage: dve_material_audit <asset.dmesh> [--json] [--strict] [--output <path>]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 64;
    }

    std::filesystem::path input;
    std::filesystem::path output;
    bool json = false;
    bool strict = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--json") {
            json = true;
        } else if (argument == "--strict") {
            strict = true;
        } else if (argument == "--output") {
            if (index + 1 >= argc) {
                print_usage();
                return 64;
            }
            output = argv[++index];
        } else if (!argument.empty() && argument.front() == '-') {
            std::cerr << "Unknown option: " << argument << '\n';
            print_usage();
            return 64;
        } else if (input.empty()) {
            input = argument;
        } else {
            std::cerr << "Only one input asset is supported per invocation.\n";
            return 64;
        }
    }

    if (input.empty()) {
        print_usage();
        return 64;
    }

    const dve::PolygonAssetReadResult read = dve::read_dmesh(input);
    if (!read) {
        std::cerr << "Could not read " << input << ": " << read.error << '\n';
        return 2;
    }

    const dve::PolygonMaterialAuthoringReport report =
        dve::analyze_polygon_material_authoring(read.asset);
    const std::string formatted = json
        ? dve::format_material_authoring_report_json(report)
        : dve::format_material_authoring_report_text(report);

    if (!output.empty()) {
        std::string error;
        if (!dve::write_material_authoring_report(output, report, json, &error)) {
            std::cerr << "Could not write " << output << ": " << error << '\n';
            return 2;
        }
    } else {
        std::cout << formatted;
    }

    if (report.has_errors()) return 2;
    if (strict && report.has_warnings()) return 1;
    return 0;
}
