#include "dve/render3d_diagnostics.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {
bool write_heatmap(const std::filesystem::path& path,
                   const dve::Render3DDepthComplexityImage& image) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || image.width == 0U || image.height == 0U || image.heatmapRgba8.empty()) return false;
    stream << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t pixel = 0U; pixel < image.samples.size(); ++pixel) {
        const std::size_t base = pixel * 4U;
        const char rgb[3]{
            static_cast<char>(std::to_integer<unsigned char>(image.heatmapRgba8[base])),
            static_cast<char>(std::to_integer<unsigned char>(image.heatmapRgba8[base + 1U])),
            static_cast<char>(std::to_integer<unsigned char>(image.heatmapRgba8[base + 2U]))};
        stream.write(rgb, 3);
    }
    return stream.good();
}
}

int main(int argc, char** argv) {
    const dve::Render3DDiagnosticsReport report = dve::make_render3d_diagnostics_demo_report();
    const std::filesystem::path output = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path(DVE_SOURCE_DIR) / "artifacts/v223_render3d_diagnostics_demo.json";
    std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::cerr << "could not open " << output << '\n';
        return 1;
    }
    stream << dve::render3d_diagnostics_json(report);
    if (!stream.good()) {
        std::cerr << "could not write " << output << '\n';
        return 1;
    }
    std::filesystem::path heatmap = output;
    heatmap.replace_filename(output.stem().string() + "_depth_complexity.ppm");
    if (!write_heatmap(heatmap, report.depthComplexity)) {
        std::cerr << "could not write " << heatmap << '\n';
        return 1;
    }
    std::cout << "3D diagnostics: objects=" << report.visibleObjectCount
              << " draws=" << report.drawCallCount
              << " breaks=" << report.pipelineBreaks.size()
              << " triangles=" << report.triangleCount
              << " skinned_vertices=" << report.skinning.totalSkinnedVertices
              << " occlusion_culled=" << report.occlusion.culledObjectCount
              << " max_depth_complexity=" << report.depthComplexity.maximumDepthComplexity
              << " hash=" << report.contentHash << '\n';
    return 0;
}
