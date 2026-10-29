#include "dve/render/scene_voxel_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dve::render {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& hash, const void* data, std::size_t bytes) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * kFnvPrime;
}
template <class T> void mix_value(std::uint64_t& hash, const T& value) noexcept { mix(hash, &value, sizeof(T)); }

const std::array<VoxelMaterialDefinition, 256>& default_palette() {
    static const std::array<VoxelMaterialDefinition, 256> palette = [] {
        std::array<VoxelMaterialDefinition, 256> result{};
        for (std::size_t id = 0; id < result.size(); ++id) result[id] = default_voxel_material(static_cast<MaterialId>(id));
        return result;
    }();
    return palette;
}

struct Box {
    Float3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Float3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool valid() const noexcept { return minimum.x <= maximum.x; }
    void grow(Float3 p) noexcept {
        minimum = {std::min(minimum.x, p.x), std::min(minimum.y, p.y), std::min(minimum.z, p.z)};
        maximum = {std::max(maximum.x, p.x), std::max(maximum.y, p.y), std::max(maximum.z, p.z)};
    }
};

// World-space box of an object's occupied voxels; invalid when the object is empty.
Box world_box(const SceneGridObject& object) {
    Box box;
    if (object.voxels == nullptr) return box;
    const auto bounds = object.voxels->occupied_bounds();
    if (!bounds.valid) return box;
    for (int corner = 0; corner < 8; ++corner) {
        const Float3 local{
            static_cast<float>((corner & 1) ? bounds.maximum.x + 1 : bounds.minimum.x) * object.cellMeters.x,
            static_cast<float>((corner & 2) ? bounds.maximum.y + 1 : bounds.minimum.y) * object.cellMeters.y,
            static_cast<float>((corner & 4) ? bounds.maximum.z + 1 : bounds.minimum.z) * object.cellMeters.z};
        box.grow(transform_point(object.transform, local));
    }
    return box;
}

std::uint64_t object_key(const SceneGridObject& object, std::uint64_t placement) {
    std::uint64_t hash = kFnvOffset;
    mix_value(hash, object.voxels);
    mix_value(hash, object.voxels ? object.voxels->revision() : std::uint64_t{});
    mix_value(hash, object.transform.position);
    mix_value(hash, object.transform.rotation);
    mix_value(hash, object.cellMeters);
    mix_value(hash, object.materials.data());
    mix_value(hash, object.materials.size());
    mix_value(hash, placement);
    return hash;
}

} // namespace

std::uint8_t SceneVoxelGrid::palette_slot(const VoxelMaterialDefinition& material, const void* table, std::uint32_t id) {
    const auto key = std::make_pair(table, id);
    if (const auto it = paletteIndex_.find(key); it != paletteIndex_.end()) return it->second;
    if (palette_.size() >= 255U) return 255U;   // out of slots: the last one is shared
    SceneGridMaterial entry;
    entry.albedo = {material.baseColor.x, material.baseColor.y, material.baseColor.z};
    entry.metallic = material.metallic;
    entry.emissive = material.emissive;
    palette_.push_back(entry);
    const auto slot = static_cast<std::uint8_t>(palette_.size());   // slot 0 is air, so entries start at 1
    paletteIndex_.emplace(key, slot);
    return slot;
}

bool SceneVoxelGrid::update(std::span<const SceneGridObject> objects, const SceneVoxelGridSettings& settings,
                            Float3 focus) {
    ++updateCounter_;
    lastRasterized_ = 0U;

    float cell = settings.cellMeters;
    std::vector<Box> boxes(objects.size());
    Box all;
    float smallest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        boxes[i] = world_box(objects[i]);
        if (!boxes[i].valid()) continue;
        all.grow(boxes[i].minimum);
        all.grow(boxes[i].maximum);
        smallest = std::min({smallest, objects[i].cellMeters.x, objects[i].cellMeters.y, objects[i].cellMeters.z});
    }
    if (!all.valid()) {
        const bool changed = !cells_.empty();
        cells_.clear(); dimensions_ = {}; contentKey_ = 0U; placementKey_ = 0U;
        return changed;
    }
    if (!(cell > 0.0F)) cell = 0.5F * smallest;   // half a voxel: rotated objects stair-step less
    cell = std::max(cell, settings.minimumCellMeters);

    const float align = cell * static_cast<float>(std::max(settings.alignmentCells, 1U));
    const float pad = 2.0F * cell;
    Float3 minimum = {std::floor((all.minimum.x - pad) / align) * align,
                      std::floor((all.minimum.y - pad) / align) * align,
                      std::floor((all.minimum.z - pad) / align) * align};
    Float3 maximum = {std::ceil((all.maximum.x + pad) / align) * align,
                      std::ceil((all.maximum.y + pad) / align) * align,
                      std::ceil((all.maximum.z + pad) / align) * align};
    const auto cells_along = [&](float lo, float hi) {
        return static_cast<std::uint32_t>(std::max(1.0F, std::round((hi - lo) / cell)));
    };
    std::array<std::uint32_t, 3> dims{cells_along(minimum.x, maximum.x), cells_along(minimum.y, maximum.y),
                                      cells_along(minimum.z, maximum.z)};
    const std::uint32_t limit = std::max(settings.maximumDimension, 1U);
    const float focusValue[3] = {focus.x, focus.y, focus.z};
    float lowValue[3] = {minimum.x, minimum.y, minimum.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (dims[static_cast<std::size_t>(axis)] <= limit) continue;
        dims[static_cast<std::size_t>(axis)] = limit;
        const float extent = static_cast<float>(limit) * cell;
        lowValue[axis] = std::floor((focusValue[axis] - 0.5F * extent) / align) * align;
    }
    const Float3 origin{lowValue[0], lowValue[1], lowValue[2]};

    std::uint64_t placement = kFnvOffset;
    mix_value(placement, origin);
    mix_value(placement, dims);
    mix_value(placement, cell);

    std::uint64_t content = placement;
    std::vector<CachedObject*> ordered;
    ordered.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!boxes[i].valid()) continue;
        const SceneGridObject& object = objects[i];
        const std::uint64_t key = object_key(object, placement);
        CachedObject& cached = cache_[object.id];
        cached.seenUpdate = updateCounter_;
        if (cached.key != key || cached.cells.empty()) {
            ++lastRasterized_;
            cached.key = key;
            cached.cells.clear();
            const std::uint32_t lo[3] = {
                static_cast<std::uint32_t>(std::clamp(std::floor((boxes[i].minimum.x - origin.x) / cell), 0.0F, float(dims[0]))),
                static_cast<std::uint32_t>(std::clamp(std::floor((boxes[i].minimum.y - origin.y) / cell), 0.0F, float(dims[1]))),
                static_cast<std::uint32_t>(std::clamp(std::floor((boxes[i].minimum.z - origin.z) / cell), 0.0F, float(dims[2])))};
            const std::uint32_t hi[3] = {
                static_cast<std::uint32_t>(std::clamp(std::ceil((boxes[i].maximum.x - origin.x) / cell), 0.0F, float(dims[0]))),
                static_cast<std::uint32_t>(std::clamp(std::ceil((boxes[i].maximum.y - origin.y) / cell), 0.0F, float(dims[1]))),
                static_cast<std::uint32_t>(std::clamp(std::ceil((boxes[i].maximum.z - origin.z) / cell), 0.0F, float(dims[2])))};
            std::span<const VoxelMaterialDefinition> table = object.materials;
            const void* tableKey = table.data();
            if (table.empty()) { table = default_palette(); tableKey = nullptr; }
            for (std::uint32_t z = lo[2]; z < hi[2]; ++z)
                for (std::uint32_t y = lo[1]; y < hi[1]; ++y)
                    for (std::uint32_t x = lo[0]; x < hi[0]; ++x) {
                        const Float3 centre{origin.x + (static_cast<float>(x) + 0.5F) * cell,
                                            origin.y + (static_cast<float>(y) + 0.5F) * cell,
                                            origin.z + (static_cast<float>(z) + 0.5F) * cell};
                        const Float3 local = inverse_transform_point(object.transform, centre);
                        const Int3 voxel{static_cast<int>(std::floor(local.x / object.cellMeters.x)),
                                         static_cast<int>(std::floor(local.y / object.cellMeters.y)),
                                         static_cast<int>(std::floor(local.z / object.cellMeters.z))};
                        const MaterialId material = object.voxels->material_at(voxel);
                        if (material == kAirMaterial) continue;
                        const VoxelMaterialDefinition& definition =
                            material < table.size() ? table[material] : default_palette()[material];
                        const std::uint8_t slot = palette_slot(definition, tableKey, material);
                        cached.cells.emplace_back((z * dims[1] + y) * dims[0] + x, slot);
                    }
        }
        mix_value(content, cached.key);
        ordered.push_back(&cached);
    }
    // Forget cached objects that left the scene.
    for (auto it = cache_.begin(); it != cache_.end();)
        it = it->second.seenUpdate == updateCounter_ ? std::next(it) : cache_.erase(it);

    mix_value(content, palette_.size());
    if (content == contentKey_ && placement == placementKey_ && !cells_.empty()) return false;
    contentKey_ = content;
    placementKey_ = placement;
    dimensions_ = dims;
    origin_ = origin;
    cellMeters_ = cell;
    cells_.assign(static_cast<std::size_t>(dims[0]) * dims[1] * dims[2], 0U);
    for (const CachedObject* cached : ordered)
        for (const auto& [index, slot] : cached->cells) cells_[index] = slot;
    return true;
}

std::uint8_t SceneVoxelGrid::cell_at_world(Float3 position) const noexcept {
    if (cells_.empty() || !(cellMeters_ > 0.0F)) return 0U;
    const float fx = std::floor((position.x - origin_.x) / cellMeters_);
    const float fy = std::floor((position.y - origin_.y) / cellMeters_);
    const float fz = std::floor((position.z - origin_.z) / cellMeters_);
    if (fx < 0.0F || fy < 0.0F || fz < 0.0F || fx >= static_cast<float>(dimensions_[0]) ||
        fy >= static_cast<float>(dimensions_[1]) || fz >= static_cast<float>(dimensions_[2])) return 0U;
    const auto x = static_cast<std::uint32_t>(fx), y = static_cast<std::uint32_t>(fy), z = static_cast<std::uint32_t>(fz);
    return cells_[(static_cast<std::size_t>(z) * dimensions_[1] + y) * dimensions_[0] + x];
}

} // namespace dve::render
