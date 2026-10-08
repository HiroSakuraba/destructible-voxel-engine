#include "dve/resumable_destruction.hpp"
#include <algorithm>
#include <iterator>
#include <utility>

namespace dve {
ResumableDestruction::ResumableDestruction(const VoxelObject& source, SphereDamageCommand command,
                                           MaterialMassTable masses)
    : sourceRevision_(source.revision()), sourceId_(source.id()), command_(command),
      masses_(masses) {}

void ResumableDestruction::finish_mass(std::size_t index) {
    const auto& c = components_[index];
    auto& out = prepared_[index].mass;
    out.voxelCount = c.voxels.size();
    out.massBearingVoxelCount = c.count;
    out.massUnits = c.mass;
    out.exactWithinLimits = true;
    if (!c.mass)
        return;
    const long double m = c.mass, x = c.x, y = c.y, z = c.z;
    out.centerOfMass = {float(c.minimum.x + x / (2 * m)), float(c.minimum.y + y / (2 * m)),
                        float(c.minimum.z + z / (2 * m))};
    const long double vx = (c.xx - x * x / m) / 4, vy = (c.yy - y * y / m) / 4,
                      vz = (c.zz - z * z / m) / 4;
    out.inertiaAboutCenter = {double(vy + vz + m / 6),         double(vx + vz + m / 6),
                              double(vx + vy + m / 6),         double(-(c.xy - x * y / m) / 4),
                              double(-(c.xz - x * z / m) / 4), double(-(c.yz - y * z / m) / 4)};
}

std::uint32_t ResumableDestruction::resume(const VoxelObject& source, std::uint32_t maximumUnits) {
    if (source.revision() != sourceRevision_) {
        phase_ = Phase::Stale;
        return 0;
    }
    std::uint32_t used{};
    constexpr Int3 neighbors[] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                  {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    while (used < maximumUnits && phase_ < Phase::Ready) {
        if (phase_ == Phase::Raster) {
            if (brickIndex_ == source.brick_count()) {
                phase_ = Phase::Connectivity;
                continue;
            }
            const auto& entry =
                *(source.bricks().begin() + static_cast<std::ptrdiff_t>(brickIndex_++));
            VoxelObject scratch(sourceId_);
            const auto materials = entry.second.materials();
            scratch.replace_brick({entry.first, entry.second.generation(), materials,
                                   VoxelObject::brick_content_hash(materials)});
            const auto report = apply_damage_commands(scratch, {command_});
            removed_ += report.removedVoxelCount;
            if (const auto* brick = std::as_const(scratch).find_brick(entry.first)) {
                brick->occupancy().for_each_set([&](std::uint16_t index) {
                    unvisited_.emplace(
                        global_from_local(entry.first, local_from_index_unchecked(index)),
                        brick->material(index));
                });
            }
        } else if (phase_ == Phase::Connectivity) {
            if (frontier_.empty()) {
                if (unvisited_.empty()) {
                    prepared_.resize(components_.size());
                    phase_ = Phase::Geometry;
                    continue;
                }
                if (components_.size() >= 32U) {
                    phase_ = Phase::Failed;
                    break;
                }
                components_.emplace_back();
                const auto first = unvisited_.begin();
                frontier_.push_back(first->first);
                components_.back().voxels.emplace_back(first->first, first->second);
                unvisited_.erase(first);
            }
            const auto point = frontier_.front();
            frontier_.pop_front();
            auto& component = components_.back();
            component.minimum = min_components(component.minimum, point);
            component.maximum = max_components(component.maximum, point);
            for (const auto offset : neighbors) {
                const Int3 next{point.x + offset.x, point.y + offset.y, point.z + offset.z};
                const auto found = unvisited_.find(next);
                if (found == unvisited_.end())
                    continue;
                frontier_.push_back(next);
                component.voxels.emplace_back(next, found->second);
                unvisited_.erase(found);
            }
        } else if (phase_ == Phase::Geometry) {
            if (componentIndex_ == components_.size()) {
                componentIndex_ = brickIndex_ = 0;
                phase_ = Phase::Proxy;
                continue;
            }
            auto& c = components_[componentIndex_];
            auto& out = prepared_[componentIndex_];
            if (!out.voxels) {
                const auto extent =
                    Int3{c.maximum.x - c.minimum.x + 1, c.maximum.y - c.minimum.y + 1,
                         c.maximum.z - c.minimum.z + 1};
                if (c.voxels.size() > kMaximumDynamicFragmentVoxels ||
                    extent.x > kMaximumDynamicFragmentExtent ||
                    extent.y > kMaximumDynamicFragmentExtent ||
                    extent.z > kMaximumDynamicFragmentExtent) {
                    phase_ = Phase::Failed;
                    break;
                }
                out.voxels = std::make_unique<VoxelObject>(sourceId_);
                out.voxelCount = c.voxels.size();
            }
            if (voxelIndex_ == c.voxels.size()) {
                finish_mass(componentIndex_++);
                voxelIndex_ = 0;
                continue;
            }
            const auto [point, material] = c.voxels[voxelIndex_++];
            out.voxels->set_voxel(point, material);
            const std::uint64_t m = masses_.density_units(material);
            if (m) {
                const auto x = std::uint64_t(point.x - c.minimum.x) * 2 + 1,
                           y = std::uint64_t(point.y - c.minimum.y) * 2 + 1,
                           z = std::uint64_t(point.z - c.minimum.z) * 2 + 1;
                ++c.count;
                c.mass += m;
                c.x += m * x;
                c.y += m * y;
                c.z += m * z;
                c.xx += m * x * x;
                c.yy += m * y * y;
                c.zz += m * z * z;
                c.xy += m * x * y;
                c.xz += m * x * z;
                c.yz += m * y * z;
            }
        } else if (phase_ == Phase::Proxy) {
            if (componentIndex_ == prepared_.size()) {
                phase_ = Phase::Ready;
                continue;
            }
            auto& out = prepared_[componentIndex_];
            if (brickIndex_ == out.voxels->brick_count()) {
                ++componentIndex_;
                brickIndex_ = 0;
                continue;
            }
            const auto& entry =
                *(out.voxels->bricks().begin() + static_cast<std::ptrdiff_t>(brickIndex_++));
            const auto boxes = build_brick_box_proxy(*out.voxels, entry.first);
            out.boxes.insert(out.boxes.end(), boxes.begin(), boxes.end());
            // Admission limit bounds native body publication too. Reject without
            // changing the world, rather than claiming a hard frame budget for an
            // unbounded commit.
            if (out.boxes.size() > 256U) {
                phase_ = Phase::Failed;
                break;
            }
        }
        ++used;
        ++workUnits_;
    }
    return used;
}
} // namespace dve
