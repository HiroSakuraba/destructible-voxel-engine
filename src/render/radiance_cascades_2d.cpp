#include "dve/render/radiance_cascades_2d.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

#include "dve/packed_brickmap.hpp"
#include "dve/voxel_object.hpp"

namespace dve::render::rc2d {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 2.0F * kPi;

struct Radiance {
    float r{};
    float g{};
    float b{};
    float t{1.0F};  // transmittance / visibility term
};

Radiance from_segment(const SegmentResult& segment) noexcept {
    return {segment.radiance.r, segment.radiance.g, segment.radiance.b, segment.transmittance};
}

// Front-to-back composite of `near` followed by `far`.
Radiance merge(Radiance nearValue, Radiance farValue) noexcept {
    return {nearValue.r + nearValue.t * farValue.r, nearValue.g + nearValue.t * farValue.g,
            nearValue.b + nearValue.t * farValue.b, nearValue.t * farValue.t};
}

void accumulate(Radiance& target, Radiance value, float weight) noexcept {
    target.r += value.r * weight;
    target.g += value.g * weight;
    target.b += value.b * weight;
    target.t += value.t * weight;
}

bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

std::uint32_t resolve_threads(std::uint32_t requested) noexcept {
    std::uint32_t threads = requested;
    if (threads == 0U) threads = std::max(1U, std::thread::hardware_concurrency());
    return std::clamp(threads, 1U, 16U);
}

// Static row partition: every output element is written by exactly one thread and per-thread
// counters are summed in thread order, so results are independent of scheduling.
template <typename Function>
void parallel_rows(std::uint32_t rows, std::uint32_t threads, Function&& function) {
    threads = std::min(threads, std::max(rows, 1U));
    if (threads <= 1U) {
        function(0U, 0U, rows);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(threads);
    const std::uint32_t chunk = (rows + threads - 1U) / threads;
    for (std::uint32_t worker = 0; worker < threads; ++worker) {
        const std::uint32_t begin = std::min(rows, worker * chunk);
        const std::uint32_t end = std::min(rows, begin + chunk);
        workers.emplace_back([&function, worker, begin, end] { function(worker, begin, end); });
    }
    for (std::thread& thread : workers) thread.join();
}

std::uint32_t pcg_hash(std::uint32_t input) noexcept {
    const std::uint32_t state = input * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

float nominal_interval_start(const CascadeConfig& config, std::uint32_t index) noexcept {
    const float growth = config.intervalGrowth;
    if (std::abs(growth - 1.0F) < 1.0e-6F) return config.baseIntervalLength * static_cast<float>(index);
    return config.baseIntervalLength * (std::pow(growth, static_cast<float>(index)) - 1.0F) /
           (growth - 1.0F);
}

float nominal_interval_length(const CascadeConfig& config, std::uint32_t index) noexcept {
    return config.baseIntervalLength * std::pow(config.intervalGrowth, static_cast<float>(index));
}

float grid_diagonal(std::uint32_t width, std::uint32_t height) noexcept {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    return std::sqrt(w * w + h * h);
}

Rgb average(Rgb sum, float count) noexcept {
    return {sum.r / count, sum.g / count, sum.b / count};
}

} // namespace

float luminance(Rgb value) noexcept {
    return 0.2126F * value.r + 0.7152F * value.g + 0.0722F * value.b;
}

// ---- Grid ----------------------------------------------------------------------------------

Grid2D Grid2D::empty(std::uint32_t width, std::uint32_t height) {
    Grid2D grid;
    grid.width = width;
    grid.height = height;
    const std::size_t cells = static_cast<std::size_t>(width) * height;
    grid.opaque.assign(cells, 0U);
    grid.emission.assign(cells, Rgb{});
    grid.albedo.assign(cells, Rgb{});
    return grid;
}

void Grid2D::fill_rect(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                       bool isOpaque, Rgb emissionValue, Rgb albedoValue) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<std::int32_t>(width));
    y1 = std::min(y1, static_cast<std::int32_t>(height));
    for (std::int32_t y = y0; y < y1; ++y) {
        for (std::int32_t x = x0; x < x1; ++x) {
            const std::size_t cell = index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            opaque[cell] = isOpaque ? 1U : 0U;
            emission[cell] = isOpaque ? emissionValue : Rgb{};
            albedo[cell] = isOpaque ? albedoValue : Rgb{};
        }
    }
}

void Grid2D::fill_disc(float cx, float cy, float radius, bool isOpaque, Rgb emissionValue,
                       Rgb albedoValue) {
    const float radiusSquared = radius * radius;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            if (dx * dx + dy * dy > radiusSquared) continue;
            const std::size_t cell = index(x, y);
            opaque[cell] = isOpaque ? 1U : 0U;
            emission[cell] = isOpaque ? emissionValue : Rgb{};
            albedo[cell] = isOpaque ? albedoValue : Rgb{};
        }
    }
}

bool Grid2D::validate(std::string* error) const {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (width == 0U || height == 0U) return fail("grid extent must be non-zero");
    const std::size_t cells = static_cast<std::size_t>(width) * height;
    if (opaque.size() != cells || emission.size() != cells || albedo.size() != cells)
        return fail("grid channel sizes do not match the extent");
    for (std::size_t cell = 0; cell < cells; ++cell) {
        const Rgb e = emission[cell];
        if (!std::isfinite(e.r) || !std::isfinite(e.g) || !std::isfinite(e.b) ||
            e.r < 0.0F || e.g < 0.0F || e.b < 0.0F)
            return fail("emission must be finite and non-negative");
        if (opaque[cell] == 0U && (e.r != 0.0F || e.g != 0.0F || e.b != 0.0F))
            return fail("empty cells cannot emit");
    }
    return true;
}

// ---- Slicing -------------------------------------------------------------------------------

Int3 slice_cell_to_voxel(const SliceRequest& request, std::uint32_t u, std::uint32_t v) noexcept {
    const std::int32_t su = request.originU + static_cast<std::int32_t>(u);
    const std::int32_t sv = request.originV + static_cast<std::int32_t>(v);
    switch (request.normal) {
    case SliceNormal::X: return {request.plane, sv, su};
    case SliceNormal::Y: return {su, request.plane, sv};
    case SliceNormal::Z: default: return {su, sv, request.plane};
    }
}

Grid2D slice_voxels(const std::function<MaterialId(Int3)>& materialAt, const SliceRequest& request,
                    std::span<const SliceMaterial> materials) {
    Grid2D grid = Grid2D::empty(request.width, request.height);
    for (std::uint32_t v = 0; v < request.height; ++v) {
        for (std::uint32_t u = 0; u < request.width; ++u) {
            const MaterialId material = materialAt(slice_cell_to_voxel(request, u, v));
            if (material == kAirMaterial) continue;
            const std::size_t cell = grid.index(u, v);
            grid.opaque[cell] = 1U;
            if (material < materials.size()) {
                grid.emission[cell] = materials[material].emission;
                grid.albedo[cell] = materials[material].albedo;
            } else {
                grid.albedo[cell] = {0.5F, 0.5F, 0.5F};
            }
        }
    }
    return grid;
}

Grid2D slice_voxel_object(const VoxelObject& object, const SliceRequest& request,
                          std::span<const SliceMaterial> materials) {
    return slice_voxels([&object](Int3 voxel) { return object.material_at(voxel); }, request,
                        materials);
}

Grid2D slice_packed_brickmap(const PackedBrickmapScene& scene, const SliceRequest& request,
                             std::span<const SliceMaterial> materials) {
    return slice_voxels([&scene](Int3 voxel) { return scene.material_at(voxel); }, request,
                        materials);
}

// ---- Synthetic scenes ----------------------------------------------------------------------

Grid2D make_synthetic_scene(SyntheticScene scene, const SyntheticSceneOptions& options) {
    Grid2D grid = Grid2D::empty(options.width, options.height);
    const float w = static_cast<float>(options.width);
    const float h = static_cast<float>(options.height);
    const auto iw = static_cast<std::int32_t>(options.width);
    const auto ih = static_cast<std::int32_t>(options.height);
    switch (scene) {
    case SyntheticScene::Empty:
        break;
    case SyntheticScene::SingleLight:
        grid.fill_disc(0.5F * w, 0.5F * h, options.lightRadius, true, options.lightColor);
        break;
    case SyntheticScene::OccluderShadow: {
        grid.fill_disc(0.25F * w, 0.5F * h, options.lightRadius, true, options.lightColor);
        const auto barX = static_cast<std::int32_t>(0.40F * w);
        grid.fill_rect(barX, static_cast<std::int32_t>(0.40F * h), barX + 2,
                       static_cast<std::int32_t>(0.60F * h), true);
        break;
    }
    case SyntheticScene::ThinWall: {
        grid.fill_disc(0.25F * w, 0.5F * h, options.lightRadius, true, options.lightColor);
        const std::int32_t wallX = iw / 2;
        grid.fill_rect(wallX, 0, wallX + 1, ih, true);
        break;
    }
    case SyntheticScene::Room: {
        const Rgb wall{0.6F, 0.6F, 0.6F};
        grid.fill_rect(0, 0, iw, 2, true, {}, wall);
        grid.fill_rect(0, ih - 2, iw, ih, true, {}, wall);
        grid.fill_rect(0, 0, 2, ih, true, {}, wall);
        grid.fill_rect(iw - 2, 0, iw, ih, true, {}, wall);
        // Interior wall with a doorway splitting the room.
        const std::int32_t splitX = static_cast<std::int32_t>(0.62F * w);
        grid.fill_rect(splitX, 2, splitX + 2, static_cast<std::int32_t>(0.42F * h), true, {}, wall);
        grid.fill_rect(splitX, static_cast<std::int32_t>(0.58F * h), splitX + 2, ih - 2, true, {}, wall);
        // Pillars.
        for (int i = 0; i < 3; ++i) {
            const auto px = static_cast<std::int32_t>((0.18F + 0.14F * static_cast<float>(i)) * w);
            grid.fill_rect(px, static_cast<std::int32_t>(0.62F * h), px + 4,
                           static_cast<std::int32_t>(0.62F * h) + 4, true, {}, wall);
        }
        grid.fill_disc(0.20F * w, 0.25F * h, options.lightRadius, true,
                       {options.lightColor.r * 1.0F, options.lightColor.g * 0.55F,
                        options.lightColor.b * 0.2F});
        grid.fill_disc(0.45F * w, 0.80F * h, options.lightRadius * 0.75F, true,
                       {options.lightColor.r * 0.2F, options.lightColor.g * 0.5F,
                        options.lightColor.b * 1.0F});
        // Small bright light in the far room: only reachable through the doorway.
        grid.fill_disc(0.85F * w, 0.20F * h, std::max(1.0F, options.lightRadius * 0.5F), true,
                       {options.lightColor.r * 0.3F * 4.0F, options.lightColor.g * 1.0F * 4.0F,
                        options.lightColor.b * 0.4F * 4.0F});
        break;
    }
    }
    return grid;
}

// ---- Cascade configuration -----------------------------------------------------------------

const char* merge_mode_name(MergeMode mode) noexcept {
    switch (mode) {
    case MergeMode::Vanilla: return "vanilla";
    case MergeMode::BilinearFix: return "bilinear-fix";
    case MergeMode::ParallaxFix: return "parallax-fix";
    }
    return "unknown";
}

bool CascadeConfig::validate(std::string* error) const {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!is_power_of_two(baseProbeSpacing) || baseProbeSpacing > 64U)
        return fail("baseProbeSpacing must be a power of two in [1,64]");
    if (baseRayCount < 1U || baseRayCount > 1024U) return fail("baseRayCount must be in [1,1024]");
    if (rayBranching < 1U || rayBranching > 16U) return fail("rayBranching must be in [1,16]");
    if (!std::isfinite(baseIntervalLength) || baseIntervalLength <= 0.0F)
        return fail("baseIntervalLength must be finite and positive");
    if (!std::isfinite(intervalGrowth) || intervalGrowth < 1.0F || intervalGrowth > 16.0F)
        return fail("intervalGrowth must be in [1,16]");
    if (!std::isfinite(intervalOverlap) || intervalOverlap < 0.0F || intervalOverlap > 4.0F)
        return fail("intervalOverlap must be in [0,4]");
    if (cascadeCount > 16U) return fail("cascadeCount must be at most 16");
    if (!std::isfinite(sky.r) || !std::isfinite(sky.g) || !std::isfinite(sky.b) ||
        sky.r < 0.0F || sky.g < 0.0F || sky.b < 0.0F)
        return fail("sky must be finite and non-negative");
    return true;
}

std::uint32_t auto_cascade_count(const CascadeConfig& config, std::uint32_t width,
                                 std::uint32_t height) noexcept {
    const float diagonal = grid_diagonal(width, height);
    for (std::uint32_t count = 1U; count <= 16U; ++count) {
        const std::uint32_t last = count - 1U;
        if (nominal_interval_start(config, last) + nominal_interval_length(config, last) >= diagonal)
            return count;
    }
    return 16U;
}

std::vector<CascadeLevel> build_cascade_levels(const CascadeConfig& config, std::uint32_t width,
                                               std::uint32_t height) {
    std::vector<CascadeLevel> levels;
    if (!config.validate() || width == 0U || height == 0U) return levels;
    const std::uint32_t count =
        config.cascadeCount != 0U ? config.cascadeCount : auto_cascade_count(config, width, height);
    const float diagonal = grid_diagonal(width, height);
    std::uint64_t rays = config.baseRayCount;
    for (std::uint32_t index = 0; index < count; ++index) {
        CascadeLevel level;
        level.index = index;
        level.probeSpacing = config.baseProbeSpacing << index;
        level.probesX = (width + level.probeSpacing - 1U) / level.probeSpacing;
        level.probesY = (height + level.probeSpacing - 1U) / level.probeSpacing;
        level.rayCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(rays, 1U << 20U));
        level.intervalStart = nominal_interval_start(config, index);
        level.intervalEnd = level.intervalStart + nominal_interval_length(config, index);
        if (index + 1U < count) {
            level.intervalEnd += config.intervalOverlap * std::sqrt(2.0F) *
                                 static_cast<float>(level.probeSpacing * 2U);
        } else {
            // The top level always reaches past the grid so no ray is left unresolved; this makes
            // extra cascades beyond auto_cascade_count() a no-op instead of a sky leak.
            level.intervalEnd = std::max(level.intervalEnd, diagonal + 2.0F);
        }
        levels.push_back(level);
        rays *= config.rayBranching;
    }
    return levels;
}

// ---- DDA -----------------------------------------------------------------------------------

SegmentResult trace_segment(const Grid2D& grid, float originX, float originY, float directionX,
                            float directionY, float length, Rgb sky) noexcept {
    SegmentResult result;
    std::int32_t cellX = static_cast<std::int32_t>(std::floor(originX));
    std::int32_t cellY = static_cast<std::int32_t>(std::floor(originY));
    const std::int32_t stepX = directionX > 0.0F ? 1 : -1;
    const std::int32_t stepY = directionY > 0.0F ? 1 : -1;
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    const float absX = std::abs(directionX);
    const float absY = std::abs(directionY);
    const float deltaX = absX > 1.0e-12F ? 1.0F / absX : kInfinity;
    const float deltaY = absY > 1.0e-12F ? 1.0F / absY : kInfinity;
    float nextX = absX > 1.0e-12F
        ? (directionX > 0.0F ? static_cast<float>(cellX + 1) - originX
                             : originX - static_cast<float>(cellX)) * deltaX
        : kInfinity;
    float nextY = absY > 1.0e-12F
        ? (directionY > 0.0F ? static_cast<float>(cellY + 1) - originY
                             : originY - static_cast<float>(cellY)) * deltaY
        : kInfinity;
    const auto width = static_cast<std::int32_t>(grid.width);
    const auto height = static_cast<std::int32_t>(grid.height);
    for (;;) {
        if (cellX < 0 || cellY < 0 || cellX >= width || cellY >= height) {
            result.radiance = sky;
            result.transmittance = 0.0F;
            return result;
        }
        const std::size_t cell = grid.index(static_cast<std::uint32_t>(cellX),
                                            static_cast<std::uint32_t>(cellY));
        if (grid.opaque[cell] != 0U) {
            result.radiance = grid.emission[cell];
            result.transmittance = 0.0F;
            return result;
        }
        const float next = std::min(nextX, nextY);
        if (!(next < length)) return result;
        ++result.steps;
        if (nextX < nextY) {
            cellX += stepX;
            nextX += deltaX;
        } else {
            cellY += stepY;
            nextY += deltaY;
        }
    }
}

// ---- Radiance cascades ---------------------------------------------------------------------

namespace {

struct Bilinear {
    std::uint32_t x[2]{};
    std::uint32_t y[2]{};
    float w[4]{};  // (x0,y0) (x1,y0) (x0,y1) (x1,y1)
};

Bilinear bilinear_upper(float px, float py, const CascadeLevel& upper) noexcept {
    const float spacing = static_cast<float>(upper.probeSpacing);
    const float u = px / spacing - 0.5F;
    const float v = py / spacing - 0.5F;
    const float fu = std::floor(u);
    const float fv = std::floor(v);
    const float ax = u - fu;
    const float ay = v - fv;
    auto clampIndex = [](float value, std::uint32_t count) {
        const float maximum = static_cast<float>(count - 1U);
        return static_cast<std::uint32_t>(std::clamp(value, 0.0F, maximum));
    };
    Bilinear result;
    result.x[0] = clampIndex(fu, upper.probesX);
    result.x[1] = clampIndex(fu + 1.0F, upper.probesX);
    result.y[0] = clampIndex(fv, upper.probesY);
    result.y[1] = clampIndex(fv + 1.0F, upper.probesY);
    result.w[0] = (1.0F - ax) * (1.0F - ay);
    result.w[1] = ax * (1.0F - ay);
    result.w[2] = (1.0F - ax) * ay;
    result.w[3] = ax * ay;
    return result;
}

// Mean of the piecewise-constant upper directions over [low, high) in bin units (wrapping).
Radiance angular_box(const std::vector<Radiance>& upper, std::size_t probeBase,
                     std::uint32_t rayCount, double low, double high) noexcept {
    Radiance sum{0.0F, 0.0F, 0.0F, 0.0F};
    const auto first = static_cast<std::int64_t>(std::floor(low));
    const auto last = static_cast<std::int64_t>(std::ceil(high));
    const auto count = static_cast<std::int64_t>(rayCount);
    for (std::int64_t bin = first; bin < last; ++bin) {
        const double overlap = std::min(high, static_cast<double>(bin + 1)) -
                               std::max(low, static_cast<double>(bin));
        if (overlap <= 0.0) continue;
        const std::int64_t wrapped = ((bin % count) + count) % count;
        accumulate(sum, upper[probeBase + static_cast<std::size_t>(wrapped)],
                   static_cast<float>(overlap));
    }
    const auto width = static_cast<float>(high - low);
    return {sum.r / width, sum.g / width, sum.b / width, sum.t / width};
}

} // namespace

RadianceCascadesResult solve_radiance_cascades(const Grid2D& grid, const CascadeConfig& config) {
    RadianceCascadesResult result;
    if (!grid.validate() || !config.validate()) return result;
    result.levels = build_cascade_levels(config, grid.width, grid.height);
    const std::size_t levelCount = result.levels.size();
    result.levelStats.assign(levelCount, {});
    const std::uint32_t threads = resolve_threads(config.threads);
    const std::uint32_t branching = config.rayBranching;

    std::vector<Radiance> upperFull;  // merged radiance of level i+1, all directions
    std::vector<Radiance> upperAvg;   // level i+1 pre-averaged to level-i directions
    std::vector<Radiance> current;

    for (std::size_t li = levelCount; li-- > 0;) {
        const CascadeLevel& level = result.levels[li];
        const bool hasUpper = li + 1U < levelCount;
        const CascadeLevel* upper = hasUpper ? &result.levels[li + 1U] : nullptr;
        const std::size_t probeCount = static_cast<std::size_t>(level.probesX) * level.probesY;
        current.assign(probeCount * level.rayCount, Radiance{});
        const float spacing = static_cast<float>(level.probeSpacing);
        const float angleStep = kTau / static_cast<float>(level.rayCount);
        const MergeMode mode = hasUpper ? config.mergeMode : MergeMode::Vanilla;
        const float upperMid = hasUpper
            ? upper->intervalStart + 0.5F * nominal_interval_length(config, upper->index) : 0.0F;

        std::vector<CascadeLevelStats> threadStats(threads);
        parallel_rows(level.probesY, threads, [&](std::uint32_t worker, std::uint32_t begin,
                                                  std::uint32_t end) {
            CascadeLevelStats& stats = threadStats[worker];
            for (std::uint32_t py = begin; py < end; ++py) {
                for (std::uint32_t px = 0; px < level.probesX; ++px) {
                    const float ox = (static_cast<float>(px) + 0.5F) * spacing;
                    const float oy = (static_cast<float>(py) + 0.5F) * spacing;
                    const std::size_t probe = static_cast<std::size_t>(py) * level.probesX + px;
                    const Bilinear bl = hasUpper ? bilinear_upper(ox, oy, *upper) : Bilinear{};
                    for (std::uint32_t k = 0; k < level.rayCount; ++k) {
                        const float angle = (static_cast<float>(k) + 0.5F) * angleStep;
                        const float dx = std::cos(angle);
                        const float dy = std::sin(angle);
                        const float sx = ox + dx * level.intervalStart;
                        const float sy = oy + dy * level.intervalStart;
                        Radiance out{0.0F, 0.0F, 0.0F, 0.0F};
                        if (mode == MergeMode::BilinearFix) {
                            for (int corner = 0; corner < 4; ++corner) {
                                const std::uint32_t qx = bl.x[corner & 1];
                                const std::uint32_t qy = bl.y[corner >> 1];
                                const float us = static_cast<float>(upper->probeSpacing);
                                const float ex = (static_cast<float>(qx) + 0.5F) * us +
                                                 dx * upper->intervalStart;
                                const float ey = (static_cast<float>(qy) + 0.5F) * us +
                                                 dy * upper->intervalStart;
                                const float vx = ex - sx;
                                const float vy = ey - sy;
                                const float length = std::sqrt(vx * vx + vy * vy);
                                const float inv = length > 1.0e-6F ? 1.0F / length : 0.0F;
                                const SegmentResult segment = trace_segment(
                                    grid, sx, sy, length > 1.0e-6F ? vx * inv : dx,
                                    length > 1.0e-6F ? vy * inv : dy, length, config.sky);
                                ++stats.rays;
                                stats.ddaSteps += segment.steps;
                                const std::size_t upperProbe =
                                    static_cast<std::size_t>(qy) * upper->probesX + qx;
                                const Radiance far = upperAvg[upperProbe * level.rayCount + k];
                                accumulate(out, merge(from_segment(segment), far), bl.w[corner]);
                            }
                        } else {
                            const SegmentResult segment = trace_segment(
                                grid, sx, sy, dx, dy, level.intervalEnd - level.intervalStart,
                                config.sky);
                            ++stats.rays;
                            stats.ddaSteps += segment.steps;
                            const Radiance nearValue = from_segment(segment);
                            if (!hasUpper || nearValue.t <= 0.0F) {
                                out = hasUpper ? nearValue
                                               : merge(nearValue, {config.sky.r, config.sky.g,
                                                                   config.sky.b, 0.0F});
                            } else {
                                Radiance far{0.0F, 0.0F, 0.0F, 0.0F};
                                for (int corner = 0; corner < 4; ++corner) {
                                    const std::uint32_t qx = bl.x[corner & 1];
                                    const std::uint32_t qy = bl.y[corner >> 1];
                                    const std::size_t upperProbe =
                                        static_cast<std::size_t>(qy) * upper->probesX + qx;
                                    if (mode == MergeMode::ParallaxFix) {
                                        const float us = static_cast<float>(upper->probeSpacing);
                                        const float qxw = (static_cast<float>(qx) + 0.5F) * us;
                                        const float qyw = (static_cast<float>(qy) + 0.5F) * us;
                                        const float tx = ox + dx * upperMid - qxw;
                                        const float ty = oy + dy * upperMid - qyw;
                                        double a = std::atan2(static_cast<double>(ty),
                                                              static_cast<double>(tx));
                                        if (a < 0.0) a += 2.0 * static_cast<double>(kPi);
                                        const double centre =
                                            a / (2.0 * static_cast<double>(kPi)) * upper->rayCount;
                                        const double half = 0.5 * branching;
                                        accumulate(far, angular_box(upperFull,
                                                                    upperProbe * upper->rayCount,
                                                                    upper->rayCount, centre - half,
                                                                    centre + half),
                                                   bl.w[corner]);
                                    } else {
                                        accumulate(far, upperAvg[upperProbe * level.rayCount + k],
                                                   bl.w[corner]);
                                    }
                                }
                                out = merge(nearValue, far);
                            }
                        }
                        current[probe * level.rayCount + k] = out;
                    }
                }
            }
        });
        for (const CascadeLevelStats& stats : threadStats) {
            result.levelStats[li].rays += stats.rays;
            result.levelStats[li].ddaSteps += stats.ddaSteps;
        }
        const std::uint64_t upperBytes = (config.mergeMode == MergeMode::ParallaxFix
                                              ? upperFull.size() : upperAvg.size()) *
                                         sizeof(Radiance);
        result.peakCascadeBytes = std::max<std::uint64_t>(
            result.peakCascadeBytes, current.size() * sizeof(Radiance) + upperBytes);

        if (li > 0U) {
            // Pre-average this level into the directions of the level below (GM Shaders part 2).
            const std::uint32_t lowerRays = level.rayCount / branching;
            upperAvg.assign(probeCount * lowerRays, Radiance{});
            const float inverse = 1.0F / static_cast<float>(branching);
            for (std::size_t probe = 0; probe < probeCount; ++probe) {
                for (std::uint32_t j = 0; j < lowerRays; ++j) {
                    Radiance sum{0.0F, 0.0F, 0.0F, 0.0F};
                    for (std::uint32_t c = 0; c < branching; ++c)
                        accumulate(sum, current[probe * level.rayCount + j * branching + c], inverse);
                    upperAvg[probe * lowerRays + j] = sum;
                }
            }
            if (config.mergeMode == MergeMode::ParallaxFix) upperFull.swap(current);
        }
    }

    for (const CascadeLevelStats& stats : result.levelStats) {
        result.totalRays += stats.rays;
        result.totalDdaSteps += stats.ddaSteps;
    }

    // Cascade-0 probe fluence, then bilinear reconstruction at cell centres.
    const CascadeLevel& base = result.levels.front();
    std::vector<Rgb> probeFluence(static_cast<std::size_t>(base.probesX) * base.probesY);
    for (std::size_t probe = 0; probe < probeFluence.size(); ++probe) {
        Rgb sum{};
        for (std::uint32_t k = 0; k < base.rayCount; ++k) {
            const Radiance value = current[probe * base.rayCount + k];
            sum.r += value.r;
            sum.g += value.g;
            sum.b += value.b;
        }
        probeFluence[probe] = average(sum, static_cast<float>(base.rayCount));
    }
    FluenceImage& image = result.fluence;
    image.width = grid.width;
    image.height = grid.height;
    image.pixels.assign(static_cast<std::size_t>(grid.width) * grid.height, Rgb{});
    for (std::uint32_t y = 0; y < grid.height; ++y) {
        for (std::uint32_t x = 0; x < grid.width; ++x) {
            const std::size_t cell = grid.index(x, y);
            if (grid.opaque[cell] != 0U) {
                image.pixels[cell] = grid.emission[cell];
                continue;
            }
            const Bilinear bl = bilinear_upper(static_cast<float>(x) + 0.5F,
                                               static_cast<float>(y) + 0.5F, base);
            Rgb value{};
            for (int corner = 0; corner < 4; ++corner) {
                const Rgb sample = probeFluence[static_cast<std::size_t>(bl.y[corner >> 1]) *
                                                    base.probesX + bl.x[corner & 1]];
                value.r += sample.r * bl.w[corner];
                value.g += sample.g * bl.w[corner];
                value.b += sample.b * bl.w[corner];
            }
            image.pixels[cell] = value;
        }
    }
    return result;
}

// ---- Brute force ---------------------------------------------------------------------------

BruteForceResult solve_brute_force(const Grid2D& grid, const BruteForceConfig& config) {
    BruteForceResult result;
    if (!grid.validate() || config.raysPerPixel == 0U) return result;
    const std::uint32_t threads = resolve_threads(config.threads);
    FluenceImage& image = result.fluence;
    image.width = grid.width;
    image.height = grid.height;
    image.pixels.assign(static_cast<std::size_t>(grid.width) * grid.height, Rgb{});
    const float maxLength = 2.0F * static_cast<float>(grid.width + grid.height) + 4.0F;
    const double angleStep = 2.0 * static_cast<double>(kPi) / config.raysPerPixel;
    std::vector<std::uint64_t> threadSteps(threads, 0U);
    std::vector<std::uint64_t> threadRays(threads, 0U);
    parallel_rows(grid.height, threads, [&](std::uint32_t worker, std::uint32_t begin,
                                            std::uint32_t end) {
        for (std::uint32_t y = begin; y < end; ++y) {
            for (std::uint32_t x = 0; x < grid.width; ++x) {
                const std::size_t cell = grid.index(x, y);
                if (grid.opaque[cell] != 0U) {
                    image.pixels[cell] = grid.emission[cell];
                    continue;
                }
                const double rotation = config.rotatePerPixel
                    ? static_cast<double>(pcg_hash(static_cast<std::uint32_t>(cell))) / 4294967296.0
                    : 0.5;
                double r = 0.0, g = 0.0, b = 0.0;
                for (std::uint32_t k = 0; k < config.raysPerPixel; ++k) {
                    const double angle = (static_cast<double>(k) + rotation) * angleStep;
                    const SegmentResult segment = trace_segment(
                        grid, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                        static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)),
                        maxLength, config.sky);
                    threadSteps[worker] += segment.steps;
                    r += segment.radiance.r;
                    g += segment.radiance.g;
                    b += segment.radiance.b;
                }
                threadRays[worker] += config.raysPerPixel;
                const double inverse = 1.0 / config.raysPerPixel;
                image.pixels[cell] = {static_cast<float>(r * inverse), static_cast<float>(g * inverse),
                                      static_cast<float>(b * inverse)};
            }
        }
    });
    for (std::uint32_t worker = 0; worker < threads; ++worker) {
        result.totalRays += threadRays[worker];
        result.totalDdaSteps += threadSteps[worker];
    }
    return result;
}

// ---- Metrics -------------------------------------------------------------------------------

ImageError compare_fluence(const Grid2D& grid, const FluenceImage& candidate,
                           const FluenceImage& reference) {
    ImageError error;
    if (candidate.pixels.size() != reference.pixels.size() ||
        candidate.pixels.size() != grid.opaque.size())
        return error;
    double squared = 0.0, sumReference = 0.0;
    for (std::size_t cell = 0; cell < grid.opaque.size(); ++cell) {
        if (grid.opaque[cell] != 0U) continue;
        const double a = luminance(candidate.pixels[cell]);
        const double b = luminance(reference.pixels[cell]);
        const double difference = a - b;
        squared += difference * difference;
        error.maxAbs = std::max(error.maxAbs, std::abs(difference));
        sumReference += b;
        ++error.comparedPixels;
    }
    if (error.comparedPixels == 0U) return error;
    const auto count = static_cast<double>(error.comparedPixels);
    error.rmse = std::sqrt(squared / count);
    error.meanReference = sumReference / count;
    if (error.meanReference > 0.0) {
        error.relativeRmse = error.rmse / error.meanReference;
        error.relativeMax = error.maxAbs / error.meanReference;
    }
    return error;
}

// ---- Penumbra calculator port --------------------------------------------------------------

namespace {
// Double-precision pi, as in the calculator's JavaScript (Math.PI). The port deliberately keeps
// the calculator's arithmetic order so results match it bit-for-bit, including its rounding
// quirks (e.g. tan(45 deg) = 0.9999999999999999 makes an 8-cell interval count as 9 steps).
constexpr double kPiDouble = 3.141592653589793;

struct StartAndAngle {
    double start;
    double angle;
};

StartAndAngle interval_start_for_spacing(const PenumbraCalculatorInput& input, double spacing) {
    const double penumbra = static_cast<double>(input.penumbraAngleDegrees) * kPiDouble / 180.0;
    const double distanceFromWall = spacing * 0.5 / std::tan(penumbra * 0.5);
    const double lightToWall = static_cast<double>(input.minLightWidth) * 0.5 / std::tan(penumbra * 0.5);
    return {distanceFromWall + lightToWall, 2.0 * std::atan(spacing * 0.5 / distanceFromWall)};
}
} // namespace

PenumbraCascade penumbra_cascade(const PenumbraCalculatorInput& input, std::uint32_t cascade) noexcept {
    PenumbraCascade out;
    const double diagonal = input.diagonals ? std::sqrt(2.0) : 1.0;
    const std::uint32_t spacing = input.initialSpacing << cascade;
    const std::uint32_t nextSpacing = input.initialSpacing << (cascade + 1U);
    const StartAndAngle start = interval_start_for_spacing(input, spacing * diagonal);
    const StartAndAngle endValue = interval_start_for_spacing(input, nextSpacing * diagonal);
    const double intervalStart = cascade > 0U ? start.start : 0.0;
    const double minLight = std::ceil(static_cast<double>(input.minLightWidth));
    const double minAngle = minLight / (endValue.start + spacing * diagonal);
    const double maxAngle = minLight / endValue.start;
    out.probeSpacing = spacing;
    out.intervalStart = static_cast<float>(intervalStart);
    out.intervalStartNominal = static_cast<float>(start.start);
    out.intervalEnd = static_cast<float>(endValue.start);
    out.minRays = static_cast<std::uint32_t>(std::ceil(2.0 * kPiDouble / maxAngle));
    out.maxRays = static_cast<std::uint32_t>(std::ceil(2.0 * kPiDouble / minAngle));
    out.penumbraAngleDegrees = static_cast<float>(start.angle * 180.0 / kPiDouble);
    out.probes = static_cast<std::uint64_t>((input.width + spacing - 1U) / spacing) *
                 ((input.height + spacing - 1U) / spacing);
    out.intervalSteps = static_cast<std::uint32_t>(
        std::ceil(std::ceil(endValue.start - intervalStart) / minLight));
    out.outsideBounds = intervalStart > static_cast<double>(std::max(input.width, input.height));
    return out;
}

std::vector<PenumbraLevelCheck> check_penumbra_condition(std::span<const CascadeLevel> levels,
                                                         float minLightWidth,
                                                         float penumbraAngleDegrees) {
    std::vector<PenumbraLevelCheck> checks;
    const double penumbra = static_cast<double>(penumbraAngleDegrees) * kPiDouble / 180.0;
    const double halfTan = std::tan(penumbra * 0.5);
    const double lightToWall = static_cast<double>(minLightWidth) * 0.5 / halfTan;
    for (const CascadeLevel& level : levels) {
        PenumbraLevelCheck check;
        check.index = level.index;
        check.rayCount = level.rayCount;
        // Same criterion as the calculator's minRays: resolve the light at the interval end.
        check.requiredRays = static_cast<std::uint32_t>(std::ceil(
            2.0 * kPiDouble * level.intervalEnd / static_cast<double>(minLightWidth)));
        check.angularOk = level.rayCount >= check.requiredRays;
        check.penumbraWidthAtStart = static_cast<float>(
            std::max(0.0, 2.0 * (static_cast<double>(level.intervalStart) - lightToWall) * halfTan));
        check.spatialOk = level.index == 0U ||
                          static_cast<float>(level.probeSpacing) <= check.penumbraWidthAtStart;
        checks.push_back(check);
    }
    return checks;
}

} // namespace dve::render::rc2d
