#include "dve/material_authoring.hpp"
#include "dve/render/polygon_renderer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dve::render::PolygonMaterialDebugView;

struct NamedView {
    std::string_view name;
    PolygonMaterialDebugView view;
};

constexpr std::array<NamedView, 14> kViews{{
    {"lit", PolygonMaterialDebugView::Lit},
    {"base-color", PolygonMaterialDebugView::BaseColor},
    {"world-normal", PolygonMaterialDebugView::WorldNormal},
    {"metallic", PolygonMaterialDebugView::Metallic},
    {"roughness", PolygonMaterialDebugView::Roughness},
    {"emissive", PolygonMaterialDebugView::Emissive},
    {"opacity", PolygonMaterialDebugView::Opacity},
    {"uv0", PolygonMaterialDebugView::Uv0},
    {"uv1", PolygonMaterialDebugView::Uv1},
    {"triplanar-weights", PolygonMaterialDebugView::TriplanarWeights},
    {"detail-fade", PolygonMaterialDebugView::DetailFade},
    {"parallax-samples", PolygonMaterialDebugView::ParallaxSampleCount},
    {"layer-mask", PolygonMaterialDebugView::LayerMask},
    {"layer-coverage", PolygonMaterialDebugView::LayerCoverage},
}};

void print_usage() {
    std::cerr
        << "Usage: dve_material_preview <asset.dmesh> <output-directory> "
           "[--all | --view <name>] [--layer <0..3>] [--size <width>x<height>] [--exposure <value>]\n"
        << "Views: lit, base-color, world-normal, metallic, roughness, emissive, opacity, "
           "uv0, uv1, triplanar-weights, detail-fade, parallax-samples, layer-mask, layer-coverage\n";
}

std::optional<PolygonMaterialDebugView> parse_view(std::string_view name) {
    const auto found = std::find_if(kViews.begin(), kViews.end(), [&](const NamedView& value) {
        return value.name == name;
    });
    return found == kViews.end() ? std::nullopt : std::optional(found->view);
}

std::string_view view_name(PolygonMaterialDebugView view) {
    const auto found = std::find_if(kViews.begin(), kViews.end(), [&](const NamedView& value) {
        return value.view == view;
    });
    return found == kViews.end() ? "unknown" : found->name;
}

bool parse_size(std::string_view text, std::uint32_t& width, std::uint32_t& height) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return false;
    const auto parse = [](std::string_view value, std::uint32_t& output) {
        const char* begin = value.data();
        const char* end = begin + value.size();
        const auto result = std::from_chars(begin, end, output);
        return result.ec == std::errc{} && result.ptr == end;
    };
    return parse(text.substr(0U, separator), width) &&
           parse(text.substr(separator + 1U), height) &&
           width >= 16U && height >= 16U && width <= 4096U && height <= 4096U;
}

dve::Float3 normalized(dve::Float3 value) noexcept {
    const float magnitude = dve::length(value);
    return magnitude > 1.0e-6F ? dve::multiply(value, 1.0F / magnitude)
                               : dve::Float3{0.0F, 0.0F, 1.0F};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 64;
    }

    const std::filesystem::path input = argv[1];
    const std::filesystem::path outputDirectory = argv[2];
    std::uint32_t width = 512U;
    std::uint32_t height = 512U;
    float exposure = 1.0F;
    bool all = false;
    std::uint32_t layerIndex = 0U;
    std::vector<PolygonMaterialDebugView> requestedViews;

    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--all") {
            all = true;
        } else if (argument == "--view") {
            if (index + 1 >= argc) {
                print_usage();
                return 64;
            }
            const std::string_view name(argv[++index]);
            const auto view = parse_view(name);
            if (!view) {
                std::cerr << "Unknown material preview view: " << name << '\n';
                return 64;
            }
            requestedViews.push_back(*view);
        } else if (argument == "--size") {
            if (index + 1 >= argc || !parse_size(argv[++index], width, height)) {
                std::cerr << "Invalid preview size. Use WIDTHxHEIGHT in the range 16..4096.\n";
                return 64;
            }
        } else if (argument == "--layer") {
            if (index + 1 >= argc) { print_usage(); return 64; }
            const std::string_view value(argv[++index]);
            const auto parsed = std::from_chars(value.data(), value.data()+value.size(), layerIndex);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || layerIndex >= dve::kMaximumVoxelMaterialLayers) {
                std::cerr << "Layer index must be in 0..3.\n";
                return 64;
            }
        } else if (argument == "--exposure") {
            if (index + 1 >= argc) {
                print_usage();
                return 64;
            }
            try {
                exposure = std::stof(argv[++index]);
            } catch (...) {
                std::cerr << "Invalid exposure value.\n";
                return 64;
            }
            if (!std::isfinite(exposure) || exposure <= 0.0F || exposure > 64.0F) {
                std::cerr << "Exposure must be finite and in (0, 64].\n";
                return 64;
            }
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            print_usage();
            return 64;
        }
    }

    if (all) {
        requestedViews.clear();
        for (const NamedView& view : kViews) requestedViews.push_back(view.view);
    }
    if (requestedViews.empty()) requestedViews.push_back(PolygonMaterialDebugView::Lit);
    std::sort(requestedViews.begin(), requestedViews.end(), [](auto left, auto right) {
        return static_cast<unsigned>(left) < static_cast<unsigned>(right);
    });
    requestedViews.erase(std::unique(requestedViews.begin(), requestedViews.end()), requestedViews.end());

    const dve::PolygonAssetReadResult read = dve::read_dmesh(input);
    if (!read) {
        std::cerr << "Could not read " << input << ": " << read.error << '\n';
        return 2;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "Could not create output directory: " << filesystemError.message() << '\n';
        return 2;
    }

    const dve::Float3 center = dve::multiply(dve::add(read.asset.bounds.minimum, read.asset.bounds.maximum), 0.5F);
    const dve::Float3 halfExtent = dve::multiply(dve::subtract(read.asset.bounds.maximum, read.asset.bounds.minimum), 0.5F);
    const float radius = std::max(dve::length(halfExtent), 0.25F);
    const dve::Float3 viewDirection = normalized({1.0F, 0.65F, 1.0F});

    dve::render::PolygonCamera camera;
    camera.target = center;
    camera.position = dve::add(center, dve::multiply(viewDirection, radius * 3.2F));
    camera.nearPlane = std::max(0.005F, radius * 0.01F);
    camera.farPlane = std::max(100.0F, radius * 20.0F);

    dve::RenderEnvironment environment;
    environment.sunDirection = normalized({0.6F, 0.8F, 0.5F});
    environment.sunIntensity = 2.0F;
    environment.globalIlluminationIntensity = 0.8F;

    dve::render::PolygonRenderTarget target;
    target.resize(width, height);
    const dve::render::PolygonRenderInstance instance{
        read.asset.objectId, &read.asset, {}, {}, {1.0F, 1.0F, 1.0F, 1.0F}, true};
    const dve::render::ReferencePolygonRenderer renderer;

    for (const PolygonMaterialDebugView view : requestedViews) {
        dve::render::PolygonRenderOptions options;
        options.preserveExistingDepth = false;
        options.materialDebugView = view;
        options.materialLayerDebugIndex = layerIndex;
        const dve::render::PolygonRenderStats stats = renderer.render(
            std::span<const dve::render::PolygonRenderInstance>(&instance, 1U),
            camera, environment, target, options);
        if (stats.shadedFragments == 0U) {
            std::cerr << "View " << view_name(view) << " produced no visible fragments.\n";
            return 2;
        }
        const std::filesystem::path output = outputDirectory /
            (std::string(view_name(view)) + ".ppm");
        std::string error;
        if (!dve::render::write_polygon_render_ppm(output, target, exposure, &error)) {
            std::cerr << "Could not write " << output << ": " << error << '\n';
            return 2;
        }
        std::cout << output.string() << " (" << stats.shadedFragments << " fragments)\n";
    }

    const dve::PolygonMaterialAuthoringReport report =
        dve::analyze_polygon_material_authoring(read.asset);
    std::string reportError;
    const std::filesystem::path reportPath = outputDirectory / "material-audit.json";
    if (!dve::write_material_authoring_report(reportPath, report, true, &reportError)) {
        std::cerr << "Could not write material audit: " << reportError << '\n';
        return 2;
    }
    return report.has_errors() ? 2 : 0;
}
