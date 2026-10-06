// Radiance Cascades Phase 1 bench: CPU 2D RC vs a brute-force reference over voxel slices.
//
// Writes PNG shots (scene, brute force, RC per merge mode, error heatmaps, destruction
// before/after) and a Markdown results table. Usage:
//   dve_rc2d_bench [--out DIR] [--size N] [--bf-rays N] [--threads N]

#include "dve/render/radiance_cascades_2d.hpp"
#include "dve/voxel_object.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace rc = dve::render::rc2d;

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

// Reinhard tonemap with exposure; opaque cells shown as their albedo tint (dim) or emission.
std::vector<std::uint8_t> tonemap(const rc::Grid2D& grid, const rc::FluenceImage& image,
                                  float exposure, bool showScene) {
    std::vector<std::uint8_t> rgb(image.pixels.size() * 3U);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        rc::Rgb c = image.pixels[i];
        if (grid.opaque[i] != 0U) {
            const rc::Rgb e = grid.emission[i];
            const bool emits = e.r + e.g + e.b > 0.0F;
            c = emits ? rc::Rgb{std::min(e.r, 1.0F), std::min(e.g, 1.0F), std::min(e.b, 1.0F)}
                      : rc::Rgb{0.12F + 0.2F * grid.albedo[i].r, 0.1F + 0.2F * grid.albedo[i].g,
                                0.14F + 0.2F * grid.albedo[i].b};
            rgb[i * 3U + 0U] = to_byte(srgb(c.r));
            rgb[i * 3U + 1U] = to_byte(srgb(c.g));
            rgb[i * 3U + 2U] = to_byte(srgb(c.b));
            continue;
        }
        if (showScene) c = {0.0F, 0.0F, 0.0F};
        auto map = [exposure](float v) { v *= exposure; return v / (1.0F + v); };
        rgb[i * 3U + 0U] = to_byte(srgb(map(c.r)));
        rgb[i * 3U + 1U] = to_byte(srgb(map(c.g)));
        rgb[i * 3U + 2U] = to_byte(srgb(map(c.b)));
    }
    return rgb;
}

// |a-b| luminance relative to `scale`, mapped black -> red -> yellow -> white at 1.0.
std::vector<std::uint8_t> heatmap(const rc::Grid2D& grid, const rc::FluenceImage& a,
                                  const rc::FluenceImage& b, double scale) {
    std::vector<std::uint8_t> rgb(a.pixels.size() * 3U);
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (grid.opaque[i] != 0U) {
            rgb[i * 3U + 0U] = rgb[i * 3U + 1U] = rgb[i * 3U + 2U] = 60U;
            continue;
        }
        const double e = std::abs(static_cast<double>(rc::luminance(a.pixels[i])) -
                                  rc::luminance(b.pixels[i])) / scale;
        const auto t = static_cast<float>(std::clamp(e, 0.0, 1.0));
        rgb[i * 3U + 0U] = to_byte(std::min(1.0F, 3.0F * t));
        rgb[i * 3U + 1U] = to_byte(std::clamp(3.0F * t - 1.0F, 0.0F, 1.0F));
        rgb[i * 3U + 2U] = to_byte(std::clamp(3.0F * t - 2.0F, 0.0F, 1.0F));
    }
    return rgb;
}

struct Tile {
    std::vector<std::uint8_t> rgb;
};

// Lays out equally sized tiles in a grid with a 4-pixel dark gutter, upscaled by `scale`.
bool write_montage(const std::filesystem::path& path, std::uint32_t tileSize, std::uint32_t columns,
                   const std::vector<Tile>& tiles, std::uint32_t scale) {
    const std::uint32_t rows = (static_cast<std::uint32_t>(tiles.size()) + columns - 1U) / columns;
    const std::uint32_t gutter = 4U;
    const std::uint32_t cell = tileSize * scale;
    const std::uint32_t width = columns * cell + (columns + 1U) * gutter;
    const std::uint32_t height = rows * cell + (rows + 1U) * gutter;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 3U, 24U);
    for (std::size_t t = 0; t < tiles.size(); ++t) {
        const std::uint32_t col = static_cast<std::uint32_t>(t) % columns;
        const std::uint32_t row = static_cast<std::uint32_t>(t) / columns;
        // Montage rows are stacked top-down, but write_png flips; place row 0 at the top.
        const std::uint32_t oy = height - gutter - (row + 1U) * cell - row * gutter;
        const std::uint32_t ox = gutter + col * (cell + gutter);
        for (std::uint32_t y = 0; y < cell; ++y)
            for (std::uint32_t x = 0; x < cell; ++x) {
                const std::size_t src = (static_cast<std::size_t>(y / scale) * tileSize + x / scale) * 3U;
                const std::size_t dst = (static_cast<std::size_t>(oy + y) * width + ox + x) * 3U;
                for (int c = 0; c < 3; ++c) out[dst + c] = tiles[t].rgb[src + c];
            }
    }
    return write_png(path, width, height, out);
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

struct Options {
    std::filesystem::path out{"/workspace/rc-shots"};
    std::uint32_t size{256};
    std::uint32_t bfRays{2048};
    std::uint32_t threads{0};
    std::uint32_t scale{2};
};

struct Row {
    std::string scene;
    std::string mode;
    std::uint64_t rays{};
    std::uint64_t steps{};
    double ms{};
    double msSingle{};
    rc::ImageError error{};
    std::uint64_t bytes{};
};

std::string fmt(double value, int precision = 2) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

// 3D voxel room: floor-to-ceiling walls around a two-room layout; a warm light voxel column in
// the left room, a green light in the right room, separated by an interior wall.
struct VoxelRoom {
    dve::VoxelObject object{7};
    std::vector<rc::SliceMaterial> materials;
};

void build_voxel_room(VoxelRoom& room, std::int32_t size) {
    room.materials.assign(4, {});
    room.materials[1] = {{0.6F, 0.6F, 0.6F}, {}};                // wall
    room.materials[2] = {{1.0F, 1.0F, 1.0F}, {1.0F, 0.55F, 0.0F}};  // warm light (only light with red)
    room.materials[3] = {{1.0F, 1.0F, 1.0F}, {0.0F, 0.8F, 1.0F}};   // cyan light (no red)
    for (std::int32_t z = 0; z < 4; ++z) {
        for (std::int32_t y = 0; y < size; ++y) {
            for (std::int32_t x = 0; x < size; ++x) {
                const bool outer = x < 2 || y < 2 || x >= size - 2 || y >= size - 2;
                const bool interior = x >= size / 2 && x < size / 2 + 2;
                if (outer || interior) room.object.set_voxel({x, y, z}, 1U);
            }
        }
        const auto lx = size / 4;
        const auto ly = size / 2;
        for (std::int32_t y = ly - 3; y <= ly + 3; ++y)
            for (std::int32_t x = lx - 3; x <= lx + 3; ++x)
                if ((x - lx) * (x - lx) + (y - ly) * (y - ly) <= 9) room.object.set_voxel({x, y, z}, 2U);
        const auto gx = 3 * size / 4;
        const auto gy = size / 4;
        for (std::int32_t y = gy - 2; y <= gy + 2; ++y)
            for (std::int32_t x = gx - 2; x <= gx + 2; ++x) room.object.set_voxel({x, y, z}, 3U);
    }
}

// "Destruction": a spherical blast carves a hole through the interior wall.
std::uint32_t blast(dve::VoxelObject& object, dve::Int3 centre, std::int32_t radius) {
    std::uint32_t removed = 0;
    for (std::int32_t z = centre.z - radius; z <= centre.z + radius; ++z)
        for (std::int32_t y = centre.y - radius; y <= centre.y + radius; ++y)
            for (std::int32_t x = centre.x - radius; x <= centre.x + radius; ++x) {
                const std::int32_t dx = x - centre.x, dy = y - centre.y, dz = z - centre.z;
                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                if (object.material_at({x, y, z}) == dve::kAirMaterial) continue;
                object.set_voxel({x, y, z}, dve::kAirMaterial);
                ++removed;
            }
    return removed;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--out") options.out = value;
        else if (key == "--size") options.size = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--bf-rays") options.bfRays = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--threads") options.threads = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "--scale") options.scale = static_cast<std::uint32_t>(std::stoul(value));
    }
    std::filesystem::create_directories(options.out);
    const std::uint32_t n = options.size;
    std::vector<Row> rows;
    std::ostringstream report;
    report << "# RC 2D bench results\n\n"
           << "Grid " << n << "x" << n << ", brute force " << options.bfRays
           << " rays/pixel, threads " << (options.threads == 0 ? std::string("auto") : std::to_string(options.threads))
           << ". Errors are luminance over non-opaque cells, relative to the mean reference fluence.\n\n";

    struct SceneCase {
        const char* name;
        rc::Grid2D grid;
        float exposure;
    };
    rc::SyntheticSceneOptions sceneOptions;
    sceneOptions.width = n;
    sceneOptions.height = n;
    std::vector<SceneCase> scenes;
    scenes.push_back({"room", rc::make_synthetic_scene(rc::SyntheticScene::Room, sceneOptions), 12.0F});
    scenes.push_back({"occluder", rc::make_synthetic_scene(rc::SyntheticScene::OccluderShadow, sceneOptions), 30.0F});
    {
        rc::SyntheticSceneOptions small = sceneOptions;
        small.lightRadius = 1.5F;
        scenes.push_back({"single-light", rc::make_synthetic_scene(rc::SyntheticScene::SingleLight, small), 60.0F});
    }
    scenes.push_back({"thin-wall", rc::make_synthetic_scene(rc::SyntheticScene::ThinWall, sceneOptions), 30.0F});

    const std::array<rc::MergeMode, 3> modes{rc::MergeMode::Vanilla, rc::MergeMode::BilinearFix,
                                             rc::MergeMode::ParallaxFix};
    for (const SceneCase& scene : scenes) {
        rc::BruteForceConfig bf;
        bf.raysPerPixel = options.bfRays;
        bf.threads = options.threads;
        auto start = std::chrono::steady_clock::now();
        const rc::BruteForceResult reference = rc::solve_brute_force(scene.grid, bf);
        const double bfMs = elapsed_ms(start);
        const std::string prefix = std::string(scene.name) + "_";
        std::vector<Tile> topRow{{tonemap(scene.grid, reference.fluence, scene.exposure, false)}};
        std::vector<Tile> bottomRow{{tonemap(scene.grid, reference.fluence, 1.0F, true)}};
        write_png(options.out / (prefix + "scene.png"), n, n, bottomRow.front().rgb);
        write_png(options.out / (prefix + "bruteforce.png"), n, n, topRow.front().rgb);
        Row bfRow{scene.name, "brute-force", reference.totalRays, reference.totalDdaSteps, bfMs, 0.0, {}, 0};
        rows.push_back(bfRow);
        const double meanRef = rc::compare_fluence(scene.grid, reference.fluence, reference.fluence).meanReference;
        for (const rc::MergeMode mode : modes) {
            for (const float overlap : {0.0F, 1.0F}) {
                rc::CascadeConfig config;
                config.mergeMode = mode;
                config.intervalOverlap = overlap;
                config.threads = options.threads;
                start = std::chrono::steady_clock::now();
                const rc::RadianceCascadesResult result = rc::solve_radiance_cascades(scene.grid, config);
                const double ms = elapsed_ms(start);
                rc::CascadeConfig single = config;
                single.threads = 1;
                start = std::chrono::steady_clock::now();
                (void)rc::solve_radiance_cascades(scene.grid, single);
                const double msSingle = elapsed_ms(start);
                const std::string modeName = std::string(rc::merge_mode_name(mode)) + (overlap > 0.0F ? "+overlap" : "");
                rows.push_back({scene.name, modeName, result.totalRays, result.totalDdaSteps, ms, msSingle,
                                rc::compare_fluence(scene.grid, result.fluence, reference.fluence),
                                result.peakCascadeBytes});
                if (overlap == 0.0F) {
                    topRow.push_back({tonemap(scene.grid, result.fluence, scene.exposure, false)});
                    bottomRow.push_back({heatmap(scene.grid, result.fluence, reference.fluence, 0.5 * meanRef)});
                    write_png(options.out / (prefix + "rc_" + rc::merge_mode_name(mode) + ".png"), n, n,
                              topRow.back().rgb);
                    write_png(options.out / (prefix + "err_" + rc::merge_mode_name(mode) + ".png"), n, n,
                              bottomRow.back().rgb);
                }
            }
        }
        std::vector<Tile> tiles = topRow;
        tiles.insert(tiles.end(), bottomRow.begin(), bottomRow.end());
        write_montage(options.out / (prefix + "montage.png"), n, 4, tiles, options.scale);
        std::cout << "scene " << scene.name << " done\n" << std::flush;
    }

    report << "| scene | mode | rays | DDA steps | ms (auto threads) | ms (1 thread) | rel RMSE | rel max | RMSE | peak cascade MB |\n"
           << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const Row& row : rows) {
        report << "| " << row.scene << " | " << row.mode << " | " << row.rays << " | " << row.steps << " | "
               << fmt(row.ms, 1) << " | " << (row.msSingle > 0.0 ? fmt(row.msSingle, 1) : std::string("-")) << " | "
               << (row.mode == "brute-force" ? std::string("ref") : fmt(row.error.relativeRmse, 4)) << " | "
               << (row.mode == "brute-force" ? std::string("ref") : fmt(row.error.relativeMax, 3)) << " | "
               << (row.mode == "brute-force" ? std::string("-") : fmt(row.error.rmse, 6)) << " | "
               << (row.bytes > 0 ? fmt(static_cast<double>(row.bytes) / (1024.0 * 1024.0), 2) : std::string("-"))
               << " |\n";
    }

    // Thin-wall leak table.
    {
        const rc::Grid2D& grid = scenes[3].grid;
        report << "\n## Thin-wall leak (1-voxel wall, light on the left, sky = 0)\n\n"
               << "| mode | left mean | right mean | right max | leak ratio (right mean / left mean) |\n|---|---:|---:|---:|---:|\n";
        for (const rc::MergeMode mode : modes) {
            for (const float overlap : {0.0F, 1.0F}) {
                rc::CascadeConfig config;
                config.mergeMode = mode;
                config.intervalOverlap = overlap;
                config.threads = options.threads;
                const auto result = rc::solve_radiance_cascades(grid, config);
                double left = 0, right = 0, rightMax = 0;
                std::uint64_t nl = 0, nr = 0;
                for (std::uint32_t y = 0; y < n; ++y)
                    for (std::uint32_t x = 0; x < n; ++x) {
                        if (grid.is_opaque(x, y)) continue;
                        const double v = rc::luminance(result.fluence.pixels[grid.index(x, y)]);
                        if (x < n / 2) { left += v; ++nl; }
                        else { right += v; ++nr; rightMax = std::max(rightMax, v); }
                    }
                left /= static_cast<double>(nl);
                right /= static_cast<double>(nr);
                report << "| " << rc::merge_mode_name(mode) << (overlap > 0.0F ? "+overlap" : "") << " | "
                       << fmt(left, 5) << " | " << fmt(right, 6) << " | " << fmt(rightMax, 6) << " | "
                       << fmt(right / left, 5) << " |\n";
            }
        }
    }

    // Cascade layout + penumbra-condition check for the default config.
    {
        rc::CascadeConfig config;
        const auto levels = rc::build_cascade_levels(config, n, n);
        const auto checks = rc::check_penumbra_condition(levels, 1.0F, 90.0F);
        report << "\n## Default cascade layout (" << n << "x" << n
               << ") and penumbra check (1-cell light, 90 deg, kornel.ski/radiance criteria)\n\n"
               << "| cascade | spacing | probes | rays/probe | interval start | interval end | required rays | angular ok | spatial ok |\n"
               << "|---:|---:|---:|---:|---:|---:|---:|---|---|\n";
        for (std::size_t i = 0; i < levels.size(); ++i) {
            report << "| " << i << " | " << levels[i].probeSpacing << " | " << levels[i].probesX << "x"
                   << levels[i].probesY << " | " << levels[i].rayCount << " | " << fmt(levels[i].intervalStart, 1)
                   << " | " << fmt(levels[i].intervalEnd, 1) << " | " << checks[i].requiredRays << " | "
                   << (checks[i].angularOk ? "yes" : "no") << " | " << (checks[i].spatialOk ? "yes" : "no") << " |\n";
        }
    }

    // Parameter sweep on the room scene.
    {
        const rc::Grid2D& grid = scenes[0].grid;
        rc::BruteForceConfig bf;
        bf.raysPerPixel = options.bfRays;
        bf.threads = options.threads;
        const auto reference = rc::solve_brute_force(grid, bf);
        report << "\n## Parameter sweep (room scene)\n\n"
               << "| mode | base spacing | base rays | base interval | cascades | rays | ms | rel RMSE | rel max | peak cascade MB |\n"
               << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const rc::MergeMode mode : {rc::MergeMode::Vanilla, rc::MergeMode::BilinearFix}) {
            for (const std::uint32_t spacing : {1U, 2U}) {
                for (const std::uint32_t rays : {4U, 16U}) {
                    for (const float interval : {0.5F, 1.0F, 2.0F, 4.0F}) {
                        rc::CascadeConfig config;
                        config.mergeMode = mode;
                        config.baseProbeSpacing = spacing;
                        config.baseRayCount = rays;
                        config.baseIntervalLength = interval;
                        config.threads = options.threads;
                        const auto start = std::chrono::steady_clock::now();
                        const auto result = rc::solve_radiance_cascades(grid, config);
                        const double ms = elapsed_ms(start);
                        const auto error = rc::compare_fluence(grid, result.fluence, reference.fluence);
                        report << "| " << rc::merge_mode_name(mode) << " | " << spacing << " | " << rays << " | "
                               << fmt(interval, 1) << " | " << result.levels.size() << " | " << result.totalRays
                               << " | " << fmt(ms, 1) << " | " << fmt(error.relativeRmse, 4) << " | "
                               << fmt(error.relativeMax, 3) << " | "
                               << fmt(static_cast<double>(result.peakCascadeBytes) / (1024.0 * 1024.0), 2) << " |\n";
                    }
                }
            }
        }
    }

    // Destruction: slice a real VoxelObject, blast a hole in the interior wall, re-slice, re-solve.
    {
        VoxelRoom room;
        build_voxel_room(room, static_cast<std::int32_t>(n));
        rc::SliceRequest slice;
        slice.normal = rc::SliceNormal::Z;
        slice.plane = 1;
        slice.width = n;
        slice.height = n;
        rc::CascadeConfig config;
        config.mergeMode = rc::MergeMode::BilinearFix;
        config.threads = options.threads;
        auto start = std::chrono::steady_clock::now();
        const rc::Grid2D before = rc::slice_voxel_object(room.object, slice, room.materials);
        const double sliceBeforeMs = elapsed_ms(start);
        start = std::chrono::steady_clock::now();
        const auto resultBefore = rc::solve_radiance_cascades(before, config);
        const double solveBeforeMs = elapsed_ms(start);
        start = std::chrono::steady_clock::now();
        const std::uint32_t removed = blast(room.object, {static_cast<std::int32_t>(n) / 2 + 1,
                                                          static_cast<std::int32_t>(n) / 2, 1}, 10);
        const double blastMs = elapsed_ms(start);
        start = std::chrono::steady_clock::now();
        const rc::Grid2D after = rc::slice_voxel_object(room.object, slice, room.materials);
        const double sliceAfterMs = elapsed_ms(start);
        start = std::chrono::steady_clock::now();
        const auto resultAfter = rc::solve_radiance_cascades(after, config);
        const double solveAfterMs = elapsed_ms(start);
        rc::BruteForceConfig bf;
        bf.raysPerPixel = options.bfRays;
        bf.threads = options.threads;
        const auto referenceAfter = rc::solve_brute_force(after, bf);
        const auto errAfter = rc::compare_fluence(after, resultAfter.fluence, referenceAfter.fluence);
        write_png(options.out / "destruction_before_scene.png", n, n, tonemap(before, resultBefore.fluence, 1.0F, true));
        write_png(options.out / "destruction_before_rc.png", n, n, tonemap(before, resultBefore.fluence, 12.0F, false));
        write_png(options.out / "destruction_after_scene.png", n, n, tonemap(after, resultAfter.fluence, 1.0F, true));
        write_png(options.out / "destruction_after_rc.png", n, n, tonemap(after, resultAfter.fluence, 12.0F, false));
        write_png(options.out / "destruction_after_bruteforce.png", n, n,
                  tonemap(after, referenceAfter.fluence, 12.0F, false));
        write_montage(options.out / "destruction_montage.png", n, 3,
                      {{tonemap(before, resultBefore.fluence, 1.0F, true)},
                       {tonemap(before, resultBefore.fluence, 12.0F, false)},
                       {std::vector<std::uint8_t>(static_cast<std::size_t>(n) * n * 3U, 24U)},
                       {tonemap(after, resultAfter.fluence, 1.0F, true)},
                       {tonemap(after, resultAfter.fluence, 12.0F, false)},
                       {tonemap(after, referenceAfter.fluence, 12.0F, false)}},
                      options.scale);
        double rightBefore = 0, rightAfter = 0;
        std::uint64_t count = 0;
        for (std::uint32_t y = 2; y < n - 2; ++y)
            for (std::uint32_t x = n / 2 + 2; x < n - 2; ++x) {
                if (before.is_opaque(x, y) || after.is_opaque(x, y)) continue;
                const rc::Rgb b = resultBefore.fluence.pixels[before.index(x, y)];
                const rc::Rgb a = resultAfter.fluence.pixels[after.index(x, y)];
                rightBefore += b.r;  // red isolates the warm light in the left room
                rightAfter += a.r;
                ++count;
            }
        report << "\n## Destruction before/after (VoxelObject slice, bilinear fix)\n\n"
               << "Blast removed " << removed << " voxels (3D sphere r=10 through the interior wall). "
               << "No precompute: the frame after the edit is a fresh slice + solve.\n\n"
               << "| step | ms |\n|---|---:|\n"
               << "| slice before | " << fmt(sliceBeforeMs) << " |\n| solve before | " << fmt(solveBeforeMs) << " |\n"
               << "| blast (set_voxel) | " << fmt(blastMs) << " |\n| slice after | " << fmt(sliceAfterMs) << " |\n"
               << "| solve after | " << fmt(solveAfterMs) << " |\n\n"
               << "Right-room mean red fluence (red comes only from the warm light in the left room): before "
               << fmt(rightBefore / static_cast<double>(count), 6) << ", after "
               << fmt(rightAfter / static_cast<double>(count), 6) << ". After-edit RC vs brute force: rel RMSE "
               << fmt(errAfter.relativeRmse, 4) << ", rel max " << fmt(errAfter.relativeMax, 3) << ".\n";
    }

    std::ofstream(options.out / "results.md") << report.str();
    std::cout << report.str();
    return 0;
}
