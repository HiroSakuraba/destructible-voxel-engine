// Radiance Cascades Phase 2 bench: CPU SPWI (screen probes, world-space intervals) on the
// reference voxel renderer, against a many-sample brute-force estimate of the same one-bounce
// GI integral.
//
// Writes /workspace/rc-shots/p2-*.png (final images, indirect buffers per mode, error heatmaps,
// destruction before/after) and p2-results.md (errors, timings, memory, sweep). Usage:
//   dve_rc_spwi_bench [--out DIR] [--width W] [--height H] [--bf-samples N] [--threads N]
//                     [--quick 1] [--sweep 0|1] [--large 0|1]

#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace spwi = dve::render::spwi;
using dve::Float3;
using dve::render::RadianceCascadeMerge;
using dve::render::RadianceCascadeSettings;

namespace {

// ---- Minimal PNG writer (stored deflate blocks; no external dependency) --------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFU) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            t[n] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    return crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFU);
}

bool write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    raw.reserve((width * 3U + 1U) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0U);
        // Flip so +y is up in the image.
        const std::size_t row = static_cast<std::size_t>(height - 1U - y) * width * 3U;
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(row),
                   rgb.begin() + static_cast<std::ptrdiff_t>(row + width * 3U));
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::size_t offset = 0;
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    do {
        const std::size_t block = std::min<std::size_t>(65535U, raw.size() - offset);
        const bool last = offset + block == raw.size();
        z.push_back(last ? 1U : 0U);
        z.push_back(static_cast<std::uint8_t>(block & 0xFFU));
        z.push_back(static_cast<std::uint8_t>(block >> 8U));
        z.push_back(static_cast<std::uint8_t>(~block & 0xFFU));
        z.push_back(static_cast<std::uint8_t>((~block >> 8U) & 0xFFU));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                 raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
        offset += block;
    } while (offset < raw.size());
    put32(z, (b << 16U) | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return file.good();
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

float srgb(float linear) {
    linear = std::clamp(linear, 0.0F, 1.0F);
    return linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

// write_png flips rows (+y up); renderer images are top-down, so pre-flip.
bool write_image(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 const std::vector<std::uint8_t>& topDown) {
    std::vector<std::uint8_t> flipped(topDown.size());
    const std::size_t row = static_cast<std::size_t>(width) * 3U;
    for (std::uint32_t y = 0; y < height; ++y)
        std::copy_n(topDown.begin() + static_cast<std::ptrdiff_t>(y * row), row,
                    flipped.begin() + static_cast<std::ptrdiff_t>((height - 1U - y) * row));
    return write_png(path, width, height, flipped);
}

std::vector<std::uint8_t> tonemap(const std::vector<Float3>& image, const spwi::VoxelGBuffer& gbuffer,
                                  float exposure) {
    std::vector<std::uint8_t> rgb(image.size() * 3U, 0U);
    auto map = [exposure](float v) { v = std::max(0.0F, v) * exposure; return v / (1.0F + v); };
    for (std::size_t i = 0; i < image.size(); ++i) {
        if (!gbuffer.texels[i].valid) {
            rgb[i * 3U] = 18U; rgb[i * 3U + 1U] = 20U; rgb[i * 3U + 2U] = 28U;
            continue;
        }
        rgb[i * 3U + 0U] = to_byte(srgb(map(image[i].x)));
        rgb[i * 3U + 1U] = to_byte(srgb(map(image[i].y)));
        rgb[i * 3U + 2U] = to_byte(srgb(map(image[i].z)));
    }
    return rgb;
}

// |a-b| luminance relative to `scale`: black -> red -> yellow -> white at 1.0.
std::vector<std::uint8_t> heatmap(const std::vector<Float3>& a, const std::vector<Float3>& b,
                                  const spwi::VoxelGBuffer& gbuffer, double scale) {
    std::vector<std::uint8_t> rgb(a.size() * 3U, 0U);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!gbuffer.texels[i].valid) {
            rgb[i * 3U] = rgb[i * 3U + 1U] = rgb[i * 3U + 2U] = 40U;
            continue;
        }
        const double e = std::abs(static_cast<double>(spwi::luminance(a[i])) - spwi::luminance(b[i])) / scale;
        const auto t = static_cast<float>(std::clamp(e, 0.0, 1.0));
        rgb[i * 3U + 0U] = to_byte(std::min(1.0F, 3.0F * t));
        rgb[i * 3U + 1U] = to_byte(std::clamp(3.0F * t - 1.0F, 0.0F, 1.0F));
        rgb[i * 3U + 2U] = to_byte(std::clamp(3.0F * t - 2.0F, 0.0F, 1.0F));
    }
    return rgb;
}

// Final colour = direct (renderer with GI off) + base·(1-metallic)·indirect, i.e. exactly what
// shade_voxel does with a given indirect term for the default (lit) shading model.
std::vector<Float3> compose_final(const dve::render::PolygonRenderTarget& direct,
                                  const spwi::SyntheticSceneSetup& setup,
                                  const spwi::VoxelGBuffer& gbuffer, const std::vector<Float3>& indirect) {
    std::vector<Float3> out(indirect.size());
    for (std::size_t i = 0; i < indirect.size(); ++i) {
        const dve::Float4 c = direct.hdrColor[i];
        out[i] = {c.x, c.y, c.z};
        if (!gbuffer.texels[i].valid) continue;
        const auto& material = setup.materials[gbuffer.texels[i].material];
        const float k = 1.0F - material.metallic;
        out[i].x += material.baseColor.x * indirect[i].x * k;
        out[i].y += material.baseColor.y * indirect[i].y * k;
        out[i].z += material.baseColor.z * indirect[i].z * k;
    }
    return out;
}

std::vector<Float3> target_rgb(const dve::render::PolygonRenderTarget& target) {
    std::vector<Float3> out(target.hdrColor.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = {target.hdrColor[i].x, target.hdrColor[i].y, target.hdrColor[i].z};
    return out;
}

struct Tile {
    std::string label;
    std::vector<std::uint8_t> rgb;
};

// Tiles in a grid with a 4-pixel gutter; rows top-down.
bool write_montage(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                   std::uint32_t columns, const std::vector<Tile>& tiles) {
    const std::uint32_t rows = (static_cast<std::uint32_t>(tiles.size()) + columns - 1U) / columns;
    const std::uint32_t gutter = 4U;
    const std::uint32_t outW = columns * width + (columns + 1U) * gutter;
    const std::uint32_t outH = rows * height + (rows + 1U) * gutter;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outW) * outH * 3U, 24U);
    for (std::size_t t = 0; t < tiles.size(); ++t) {
        const std::uint32_t col = static_cast<std::uint32_t>(t) % columns;
        const std::uint32_t row = static_cast<std::uint32_t>(t) / columns;
        const std::uint32_t ox = gutter + col * (width + gutter);
        const std::uint32_t oy = gutter + row * (height + gutter);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    out[(static_cast<std::size_t>(oy + y) * outW + ox + x) * 3U + c] =
                        tiles[t].rgb[(static_cast<std::size_t>(y) * width + x) * 3U + c];
    }
    return write_image(path, outW, outH, out);
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string fmt(double value, int precision = 3) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

std::string mb(std::uint64_t bytes) { return fmt(static_cast<double>(bytes) / (1024.0 * 1024.0), 1); }

struct Options {
    std::filesystem::path out{"/workspace/rc-shots"};
    std::uint32_t width{320};
    std::uint32_t height{180};
    std::uint32_t bfSamples{2048};
    std::uint32_t threads{0};
    bool sweep{true};
    bool large{true};
};

struct Variant {
    std::string key;   // file-name suffix
    std::string label; // table label
    RadianceCascadeSettings settings;
};

std::vector<Variant> variants(std::uint32_t threads) {
    RadianceCascadeSettings base;
    base.threadCount = threads;
    std::vector<Variant> list;
    auto add = [&](std::string key, std::string label, RadianceCascadeMerge merge, float overlap, float growth,
                   bool /*unused*/ = false, bool depthScaled = false) {
        RadianceCascadeSettings s = base;
        s.merge = merge;
        s.intervalOverlap = overlap;
        s.intervalGrowth = growth;
        if (depthScaled) s.intervalScaling = dve::render::RadianceCascadeIntervalScaling::ProbeSpacing;
        list.push_back({std::move(key), std::move(label), s});
    };
    // Defaults: world intervals, 2 px spacing, 8x8 directions, L0 = 1, x4 growth, overlap 1.
    add("vanilla", "vanilla (no overlap)", RadianceCascadeMerge::Vanilla, 0.0F, 4.0F);
    add("vanilla-overlap", "vanilla + overlap", RadianceCascadeMerge::Vanilla, 1.0F, 4.0F);
    add("bilinear", "bilinear fix (no overlap)", RadianceCascadeMerge::BilinearFix, 0.0F, 4.0F);
    add("bilinear-overlap", "bilinear fix + overlap (default)", RadianceCascadeMerge::BilinearFix, 1.0F, 4.0F);
    add("bilinear-g2", "bilinear fix + overlap, x2 intervals", RadianceCascadeMerge::BilinearFix, 1.0F, 2.0F);
    add("bilinear-probe", "bilinear fix + overlap, depth-scaled intervals", RadianceCascadeMerge::BilinearFix, 1.0F, 4.0F, false, true);
    {
        RadianceCascadeSettings s = base;
        s.baseDirectionResolution = 4U;
        list.push_back({"bilinear-4x4", "bilinear fix + overlap, 4x4 dirs", s});
    }
    return list;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--out") options.out = value;
        else if (key == "--width") options.width = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--height") options.height = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--bf-samples") options.bfSamples = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--threads") options.threads = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--sweep") options.sweep = value != "0";
        else if (key == "--large") options.large = value != "0";
    }
    std::filesystem::create_directories(options.out);
    const std::uint32_t W = options.width, H = options.height;
    std::ostringstream report;
    report << "# RC Phase 2 (CPU SPWI) bench results\n\n"
           << W << "x" << H << ", brute force " << options.bfSamples
           << " cosine samples/pixel, threads " << (options.threads == 0 ? std::string("auto") : std::to_string(options.threads))
           << ". Errors: luminance of the indirect term over pixels with a primary hit; `rel` = divided by the "
              "mean brute-force luminance.\n\n";

    const std::vector<Variant> modes = variants(options.threads);
    struct SceneCase { const char* name; spwi::SyntheticScene scene; float exposure; };
    const std::array<SceneCase, 3> scenes{{{"courtyard", spwi::SyntheticScene::Courtyard, 1.5F},
                                           {"thin-wall", spwi::SyntheticScene::ThinWall, 3.0F},
                                           {"bunker", spwi::SyntheticScene::Bunker, 4.0F}}};

    report << "## Accuracy and cost per mode\n\n"
           << "| scene | mode | cascades | interval rays | sun rays | fallback rays | ms | rel RMSE | rel max | mean bias | peak MB | all-levels MB |\n"
           << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    std::ostringstream leakTable;
    leakTable << "## Thin-wall leak (1-voxel = 0.1 m wall; brute force is exactly 0 on the right)\n\n"
              << "| mode | right mean / left mean | right max / left mean |\n|---|---:|---:|\n";

    for (const SceneCase& sceneCase : scenes) {
        spwi::SyntheticSceneSetup setup = spwi::make_synthetic_scene(sceneCase.scene);
        const auto instance = setup.instance();
        const std::span<const dve::render::VoxelReferenceInstance> instances(&instance, 1);
        auto t0 = std::chrono::steady_clock::now();
        const spwi::VoxelSceneTracer tracer(instances);
        const double tracerMs = elapsed_ms(t0);
        t0 = std::chrono::steady_clock::now();
        const spwi::VoxelGBuffer gbuffer = spwi::build_voxel_gbuffer(tracer, setup.camera, W, H, {}, options.threads);
        const double gbufferMs = elapsed_ms(t0);
        const auto reference = spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, options.bfSamples, options.threads, setup.metersPerVoxel);
        const auto mc16 = spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, 16U, options.threads, setup.metersPerVoxel);
        const double meanRef = spwi::compare_indirect(reference, reference, gbuffer).meanReference;
        const double exposure = meanRef > 0.0 ? 0.6 / meanRef : 1.0;
        std::cout << sceneCase.name << ": tracer " << fmt(tracerMs, 1) << " ms, gbuffer " << fmt(gbufferMs, 1)
                  << " ms, brute force " << fmt(reference.stats.milliseconds, 0) << " ms, mean " << meanRef << "\n";

        dve::render::PolygonRenderTarget direct;
        direct.resize(W, H);
        dve::RenderEnvironment directEnvironment = setup.environment;
        directEnvironment.globalIlluminationMode = dve::GlobalIlluminationMode::Off;
        dve::render::ReferenceVoxelRenderer directRenderer;
        directRenderer.metersPerVoxel = setup.metersPerVoxel;
        (void)directRenderer.render(instances, setup.camera, directEnvironment, direct);

        const std::string prefix = "p2-" + std::string(sceneCase.name) + "-";
        const auto finalReference = compose_final(direct, setup, gbuffer, reference.indirect);
        write_image(options.out / (prefix + "final-bruteforce.png"), W, H, tonemap(finalReference, gbuffer, sceneCase.exposure));
        write_image(options.out / (prefix + "indirect-bruteforce.png"), W, H, tonemap(reference.indirect, gbuffer, static_cast<float>(exposure)));
        std::vector<Tile> tiles{{"final (brute-force GI)", tonemap(finalReference, gbuffer, sceneCase.exposure)},
                                {"indirect brute force", tonemap(reference.indirect, gbuffer, static_cast<float>(exposure))}};
        std::vector<Tile> errorTiles;

        auto emit_row = [&](const std::string& label, const spwi::IndirectResult& result) {
            const auto e = spwi::compare_indirect(result, reference, gbuffer);
            report << "| " << sceneCase.name << " | " << label << " | " << result.stats.cascades << " | "
                   << result.stats.intervalRays << " | " << result.stats.sunRays << " | " << result.stats.fallbackRays
                   << " | " << fmt(result.stats.milliseconds, 0) << " | " << fmt(e.relativeRmse) << " | "
                   << fmt(e.relativeMax, 2) << " | " << fmt(e.meanReference > 0 ? e.meanBias / e.meanReference : 0.0)
                   << " | " << mb(result.stats.peakBytes) << " | " << mb(result.stats.allLevelsBytes) << " |\n";
            std::cout << "  " << label << ": " << fmt(result.stats.milliseconds, 0) << " ms rel RMSE "
                      << fmt(e.relativeRmse) << " max " << fmt(e.relativeMax, 2) << " bias " << fmt(e.meanBias / std::max(1e-12, e.meanReference)) << "\n";
            return e;
        };
        {
            spwi::IndirectResult bfRow = reference;
            report << "| " << sceneCase.name << " | brute force (" << options.bfSamples << " spp) | - | "
                   << reference.stats.intervalRays << " | " << reference.stats.sunRays << " | 0 | "
                   << fmt(reference.stats.milliseconds, 0) << " | ref | ref | ref | - | - |\n";
        }
        emit_row("Monte Carlo 16 spp (VoxelOneBounce cap)", mc16);
        write_image(options.out / (prefix + "indirect-mc16.png"), W, H, tonemap(mc16.indirect, gbuffer, static_cast<float>(exposure)));
        write_image(options.out / (prefix + "err-mc16.png"), W, H, heatmap(mc16.indirect, reference.indirect, gbuffer, 0.5 * meanRef));
        tiles.push_back({"MC 16 spp", tonemap(mc16.indirect, gbuffer, static_cast<float>(exposure))});
        errorTiles.push_back({"err MC16", heatmap(mc16.indirect, reference.indirect, gbuffer, 0.5 * meanRef)});

        for (const Variant& variant : modes) {
            const auto result = spwi::solve_radiance_cascades(tracer, gbuffer, setup.environment, variant.settings, setup.metersPerVoxel);
            (void)emit_row(variant.label, result);
            const auto shaded = tonemap(result.indirect, gbuffer, static_cast<float>(exposure));
            const auto err = heatmap(result.indirect, reference.indirect, gbuffer, 0.5 * meanRef);
            write_image(options.out / (prefix + "indirect-" + variant.key + ".png"), W, H, shaded);
            write_image(options.out / (prefix + "err-" + variant.key + ".png"), W, H, err);
            if (variant.key == "vanilla" || variant.key == "vanilla-overlap" || variant.key == "bilinear-overlap") {
                tiles.push_back({variant.label, shaded});
                errorTiles.push_back({"err " + variant.label, err});
            }
            if (variant.key == "bilinear-overlap") {
                const auto finalRc = compose_final(direct, setup, gbuffer, result.indirect);
                write_image(options.out / (prefix + "final-rc.png"), W, H, tonemap(finalRc, gbuffer, sceneCase.exposure));
            }
            if (sceneCase.scene == spwi::SyntheticScene::ThinWall) {
                double left = 0.0, right = 0.0, rightMax = 0.0;
                std::uint64_t nl = 0, nr = 0;
                for (std::size_t i = 0; i < gbuffer.texels.size(); ++i) {
                    const auto& t = gbuffer.texels[i];
                    if (!t.valid || t.normal.y < 0.5F || t.position.y > 1.5F) continue; // floor only
                    if (t.position.x < static_cast<float>(setup.wallX)) { left += spwi::luminance(reference.indirect[i]); ++nl; }
                    else if (t.position.x > static_cast<float>(setup.wallX + 1)) {
                        const double v = spwi::luminance(result.indirect[i]);
                        right += v; rightMax = std::max(rightMax, v); ++nr;
                    }
                }
                left /= std::max<std::uint64_t>(nl, 1U);
                right /= std::max<std::uint64_t>(nr, 1U);
                leakTable << "| " << variant.label << " | " << fmt(right / left, 5) << " | " << fmt(rightMax / left, 4) << " |\n";
            }
        }
        for (Tile& tile : errorTiles) tiles.push_back(std::move(tile));
        write_montage(options.out / (prefix + "montage.png"), W, H, 5, tiles);
    }
    report << "\n" << leakTable.str() << "\n";

    // ---- Destruction: bunker wall section removed, no precompute, same-frame update ----------
    {
        spwi::SyntheticSceneSetup setup = spwi::make_synthetic_scene(spwi::SyntheticScene::Bunker);
        const auto instance = setup.instance();
        const std::span<const dve::render::VoxelReferenceInstance> instances(&instance, 1);
        RadianceCascadeSettings settings;
        settings.threadCount = options.threads;
        dve::RenderEnvironment environment = setup.environment;
        environment.globalIlluminationMode = dve::GlobalIlluminationMode::RadianceCascades;
        dve::render::ReferenceVoxelRenderer renderer;
        renderer.radianceCascades = settings;
        renderer.metersPerVoxel = setup.metersPerVoxel;

        auto solve = [&](const char* tag) {
            auto t0 = std::chrono::steady_clock::now();
            const spwi::VoxelSceneTracer tracer(instances);
            const double tracerMs = elapsed_ms(t0);
            const auto gbuffer = spwi::build_voxel_gbuffer(tracer, setup.camera, W, H, {}, options.threads);
            const auto rc = spwi::solve_radiance_cascades(tracer, gbuffer, setup.environment, settings, setup.metersPerVoxel);
            const auto bf = spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, options.bfSamples, options.threads, setup.metersPerVoxel);
            dve::render::PolygonRenderTarget target;
            target.resize(W, H);
            t0 = std::chrono::steady_clock::now();
            (void)renderer.render(instances, setup.camera, environment, target);
            const double renderMs = elapsed_ms(t0);
            // Receiver: room floor within 6 voxels of the shared wall.
            double rcSum = 0.0, bfSum = 0.0; std::uint64_t count = 0;
            for (std::size_t i = 0; i < gbuffer.texels.size(); ++i) {
                const auto& t = gbuffer.texels[i];
                if (!t.valid || t.normal.y < 0.5F || t.position.y > 1.5F) continue;
                if (t.position.x < static_cast<float>(setup.wallX + 1) || t.position.x > static_cast<float>(setup.wallX + 7)) continue;
                rcSum += spwi::luminance(rc.indirect[i]); bfSum += spwi::luminance(bf.indirect[i]); ++count;
            }
            struct Out { spwi::VoxelGBuffer gbuffer; spwi::IndirectResult rc, bf; dve::render::PolygonRenderTarget target;
                         double tracerMs, renderMs, rcReceiver, bfReceiver; spwi::IndirectError error; };
            Out out{gbuffer, rc, bf, target, tracerMs, renderMs, rcSum / std::max<std::uint64_t>(count, 1U),
                    bfSum / std::max<std::uint64_t>(count, 1U), spwi::compare_indirect(rc, bf, gbuffer)};
            std::cout << "destruction " << tag << ": render " << fmt(out.renderMs, 0) << " ms, receiver rc "
                      << out.rcReceiver << " bf " << out.bfReceiver << "\n";
            return out;
        };
        const auto before = solve("before");
        auto t0 = std::chrono::steady_clock::now();
        const std::uint32_t removed = spwi::remove_box(*setup.object, setup.removableMinimum, setup.removableMaximum);
        const double editMs = elapsed_ms(t0);
        const auto after = solve("after");
        const double meanRef = after.error.meanReference;
        const float exposure = static_cast<float>(meanRef > 0.0 ? 0.6 / meanRef : 1.0);
        const std::vector<Tile> tiles{
            {"before final", tonemap(target_rgb(before.target), before.gbuffer, 2.5F)},
            {"after final", tonemap(target_rgb(after.target), after.gbuffer, 2.5F)},
            {"before indirect RC", tonemap(before.rc.indirect, before.gbuffer, exposure)},
            {"after indirect RC", tonemap(after.rc.indirect, after.gbuffer, exposure)},
            {"after indirect brute force", tonemap(after.bf.indirect, after.gbuffer, exposure)},
            {"after error", heatmap(after.rc.indirect, after.bf.indirect, after.gbuffer, 0.5 * meanRef)}};
        write_image(options.out / "p2-destruction-before-final.png", W, H, tiles[0].rgb);
        write_image(options.out / "p2-destruction-after-final.png", W, H, tiles[1].rgb);
        write_image(options.out / "p2-destruction-before-indirect.png", W, H, tiles[2].rgb);
        write_image(options.out / "p2-destruction-after-indirect.png", W, H, tiles[3].rgb);
        write_image(options.out / "p2-destruction-after-bruteforce.png", W, H, tiles[4].rgb);
        write_image(options.out / "p2-destruction-after-err.png", W, H, tiles[5].rgb);
        write_montage(options.out / "p2-destruction-montage.png", W, H, 3, tiles);
        report << "## Destruction (bunker: 1-voxel wall section removed between frames)\n\n"
               << "`remove_box` cleared " << removed << " voxels in " << fmt(editMs, 2)
               << " ms. Each frame rebuilds the tracer snapshot and re-solves; nothing is cached or precomputed, "
                  "and there is no temporal history to reset.\n\n"
               << "| frame | tracer snapshot ms | RC-mode render ms | receiver RC | receiver brute force | rel RMSE vs brute force |\n"
               << "|---|---:|---:|---:|---:|---:|\n"
               << "| before | " << fmt(before.tracerMs, 2) << " | " << fmt(before.renderMs, 0) << " | " << fmt(before.rcReceiver, 4)
               << " | " << fmt(before.bfReceiver, 4) << " | " << fmt(before.error.relativeRmse) << " |\n"
               << "| after | " << fmt(after.tracerMs, 2) << " | " << fmt(after.renderMs, 0) << " | " << fmt(after.rcReceiver, 4)
               << " | " << fmt(after.bfReceiver, 4) << " | " << fmt(after.error.relativeRmse) << " |\n\n"
               << "Receiver = mean indirect luminance on the room floor within 6 voxels of the wall.\n\n";
    }

    // ---- Cost versus resolution (courtyard) ---------------------------------------------------
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes{{320U, 180U}};
        if (options.large) sizes.push_back({640U, 360U});
        report << "## Cost versus resolution (courtyard; threads auto = " << std::thread::hardware_concurrency() << ")\n\n"
               << "| resolution | mode | cascades | probes (all levels) | interval rays | ms (auto) | ms (1 thread) | rel RMSE | peak MB | all-levels MB |\n"
               << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        spwi::SyntheticSceneSetup setup = spwi::make_synthetic_scene(spwi::SyntheticScene::Courtyard);
        const auto instance = setup.instance();
        const std::span<const dve::render::VoxelReferenceInstance> instances(&instance, 1);
        const spwi::VoxelSceneTracer tracer(instances);
        for (const auto& [w, h] : sizes) {
            const auto gbuffer = spwi::build_voxel_gbuffer(tracer, setup.camera, w, h, {}, options.threads);
            const std::uint32_t samples = w > 320U ? std::min(options.bfSamples, 2048U) : options.bfSamples;
            const auto reference = spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, samples, options.threads, setup.metersPerVoxel);
            const std::string res = std::to_string(w) + "x" + std::to_string(h);
            report << "| " << res << " | brute force " << samples << " spp | - | - | " << reference.stats.intervalRays
                   << " | " << fmt(reference.stats.milliseconds, 0) << " | - | ref | - | - |\n";
            auto single = [&](auto&& run) { return run(1U); };
            const auto mc = spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, 16U, options.threads, setup.metersPerVoxel);
            const auto mc1 = single([&](std::uint32_t t) { return spwi::solve_brute_force_indirect(tracer, gbuffer, setup.environment, 16U, t, setup.metersPerVoxel); });
            report << "| " << res << " | Monte Carlo 16 spp | - | - | " << mc.stats.intervalRays << " | "
                   << fmt(mc.stats.milliseconds, 0) << " | " << fmt(mc1.stats.milliseconds, 0) << " | "
                   << fmt(spwi::compare_indirect(mc, reference, gbuffer).relativeRmse) << " | - | - |\n";
            for (const Variant& variant : modes) {
                if (variant.key != "vanilla-overlap" && variant.key != "bilinear-overlap" && variant.key != "bilinear-4x4") continue;
                const auto r = spwi::solve_radiance_cascades(tracer, gbuffer, setup.environment, variant.settings, setup.metersPerVoxel);
                RadianceCascadeSettings one = variant.settings;
                one.threadCount = 1U;
                const auto r1 = spwi::solve_radiance_cascades(tracer, gbuffer, setup.environment, one, setup.metersPerVoxel);
                report << "| " << res << " | " << variant.label << " | " << r.stats.cascades << " | " << r.stats.probes
                       << " | " << r.stats.intervalRays << " | " << fmt(r.stats.milliseconds, 0) << " | "
                       << fmt(r1.stats.milliseconds, 0) << " | " << fmt(spwi::compare_indirect(r, reference, gbuffer).relativeRmse)
                       << " | " << mb(r.stats.peakBytes) << " | " << mb(r.stats.allLevelsBytes) << " |\n";
                std::cout << res << " " << variant.label << " " << fmt(r.stats.milliseconds, 0) << " ms\n";
            }
        }
        report << "\nTracer snapshot: " << fmt(static_cast<double>(tracer.dense_bytes()) / 1024.0, 1) << " KiB dense occupancy.\n\n";
    }

    if (options.sweep) {
        // Parameter sweep over the three scenes: accuracy (courtyard, bunker), leak (thin wall).
        struct Prepared {
            spwi::SyntheticSceneSetup setup;
            dve::render::VoxelReferenceInstance instance{};
            std::unique_ptr<spwi::VoxelSceneTracer> tracer;
            spwi::VoxelGBuffer gbuffer;
            spwi::IndirectResult reference;
        };
        std::vector<Prepared> prepared;
        for (const SceneCase& sceneCase : scenes) {
            Prepared p;
            p.setup = spwi::make_synthetic_scene(sceneCase.scene);
            p.instance = p.setup.instance();
            prepared.push_back(std::move(p));
        }
        for (Prepared& p : prepared) {
            p.tracer = std::make_unique<spwi::VoxelSceneTracer>(std::span<const dve::render::VoxelReferenceInstance>(&p.instance, 1));
            p.gbuffer = spwi::build_voxel_gbuffer(*p.tracer, p.setup.camera, W, H, {}, options.threads);
            p.reference = spwi::solve_brute_force_indirect(*p.tracer, p.gbuffer, p.setup.environment, options.bfSamples, options.threads, p.setup.metersPerVoxel);
        }
        std::ofstream csv(options.out / "p2-sweep.csv");
        csv << "merge,scaling,spacing,dirs,L0,growth,overlap,courtyard_rel_rmse,bunker_rel_rmse,thinwall_rel_rmse,leak,ms_courtyard,ms_bunker,peak_mb\n";
        struct SweepRow { std::string text; double score; double leak; std::string merge; };
        std::vector<SweepRow> sweepRows;
        for (const auto merge : {RadianceCascadeMerge::Vanilla, RadianceCascadeMerge::BilinearFix})
        for (const auto scaling : {dve::render::RadianceCascadeIntervalScaling::ProbeSpacing, dve::render::RadianceCascadeIntervalScaling::World})
        for (const std::uint32_t spacing : {1U, 2U, 4U})
        for (const std::uint32_t dirs : {4U, 8U})
        for (const float l0 : {0.5F, 1.0F, 2.0F})
        for (const float growth : {2.0F, 4.0F})
        for (const float overlap : {0.0F, 1.0F}) {
            RadianceCascadeSettings s;
            s.threadCount = options.threads;
            s.merge = merge; s.intervalScaling = scaling; s.baseProbeSpacingPixels = spacing;
            s.baseDirectionResolution = dirs; s.baseIntervalLength = l0; s.intervalGrowth = growth;
            s.intervalOverlap = overlap;
            std::array<double, 3> rel{}; std::array<double, 3> ms{}; double leak = 0.0; std::uint64_t peak = 0;
            for (std::size_t k = 0; k < prepared.size(); ++k) {
                Prepared& p = prepared[k];
                const auto r = spwi::solve_radiance_cascades(*p.tracer, p.gbuffer, p.setup.environment, s, p.setup.metersPerVoxel);
                rel[k] = spwi::compare_indirect(r, p.reference, p.gbuffer).relativeRmse;
                ms[k] = r.stats.milliseconds;
                peak = std::max(peak, r.stats.peakBytes);
                if (scenes[k].scene == spwi::SyntheticScene::ThinWall) {
                    double left = 0.0, right = 0.0; std::uint64_t nl = 0, nr = 0;
                    for (std::size_t i = 0; i < p.gbuffer.texels.size(); ++i) {
                        const auto& t = p.gbuffer.texels[i];
                        if (!t.valid || t.normal.y < 0.5F || t.position.y > 1.5F) continue;
                        if (t.position.x < static_cast<float>(p.setup.wallX)) { left += spwi::luminance(p.reference.indirect[i]); ++nl; }
                        else if (t.position.x > static_cast<float>(p.setup.wallX + 1)) { right += spwi::luminance(r.indirect[i]); ++nr; }
                    }
                    leak = (right / std::max<std::uint64_t>(nr, 1U)) / std::max(1e-12, left / std::max<std::uint64_t>(nl, 1U));
                }
            }
            const std::string mergeName = merge == RadianceCascadeMerge::Vanilla ? "vanilla" : "bilinear";
            const std::string scalingName = scaling == dve::render::RadianceCascadeIntervalScaling::World ? "world" : "probe";
            csv << mergeName << "," << scalingName << "," << spacing << "," << dirs << "," << l0 << "," << growth << ","
                << overlap << "," << rel[0] << "," << rel[2] << "," << rel[1] << "," << leak << "," << ms[0] << ","
                << ms[2] << "," << mb(peak) << "\n";
            std::ostringstream row;
            row << "| " << mergeName << " | " << scalingName << " | " << spacing << " | " << dirs << "x" << dirs << " | "
                << fmt(l0, 1) << " | x" << fmt(growth, 0) << " | " << fmt(overlap, 0) << " | " << fmt(rel[0]) << " | "
                << fmt(rel[2]) << " | " << fmt(rel[1]) << " | " << fmt(leak, 4) << " | " << fmt(ms[0], 0) << " | "
                << mb(peak) << " |\n";
            sweepRows.push_back({row.str(), 0.5 * (rel[0] + rel[2]), leak, mergeName});
            std::cout << row.str();
        }
        std::sort(sweepRows.begin(), sweepRows.end(), [](const SweepRow& a, const SweepRow& b) { return a.score < b.score; });
        report << "## Parameter sweep (" << W << "x" << H << "; sorted by mean rel RMSE of courtyard and bunker)\n\n"
               << "Full grid in `p2-sweep.csv`. Top 12 overall, then the best 6 with thin-wall leak < 0.001.\n\n"
               << "| merge | intervals | spacing px | dirs | L0 | growth | overlap | courtyard | bunker | thin wall | leak | ms (courtyard) | peak MB |\n"
               << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(12U, sweepRows.size()); ++i) report << sweepRows[i].text;
        report << "| | | | | | | | | | | | | |\n";
        std::size_t shown = 0;
        for (const SweepRow& row : sweepRows) {
            if (row.leak >= 0.001 || shown >= 6U) continue;
            report << row.text;
            ++shown;
        }
        report << "\n";
    }
    std::ofstream(options.out / "p2-results.md") << report.str();
    std::cout << report.str();
    return 0;
}
