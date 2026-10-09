#pragma once
#include "dve/damage.hpp"
#include "dve/fragment.hpp"
#include <deque>
#include <map>
#include <memory>

namespace dve {
struct PreparedDestructionComponent {
    std::unique_ptr<VoxelObject> voxels;
    FragmentMassProperties mass;
    std::vector<VoxelBox> boxes;
    std::uint64_t voxelCount{};
};
// Tick-budgeted, geometry-local work. One unit is one brick raster/proxy, one
// BFS voxel, or one mass/geometry voxel. It publishes no changes to its source.
class ResumableDestruction {
  public:
    enum class Phase : std::uint8_t { Raster, Connectivity, Geometry, Proxy, Ready, Stale, Failed };
    ResumableDestruction(const VoxelObject& source, SphereDamageCommand command,
                         MaterialMassTable masses);
    std::uint32_t resume(const VoxelObject& source, std::uint32_t maximumUnits);
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] bool matches_mass_table(const MaterialMassTable& table) const noexcept {
        for (unsigned i = 0; i < 256U; ++i)
            if (masses_.density_units(static_cast<MaterialId>(i)) !=
                table.density_units(static_cast<MaterialId>(i)))
                return false;
        return true;
    }
    [[nodiscard]] std::uint64_t source_revision() const noexcept { return sourceRevision_; }
    [[nodiscard]] std::uint64_t work_units() const noexcept { return workUnits_; }
    [[nodiscard]] std::uint64_t removed_voxels() const noexcept { return removed_; }
    [[nodiscard]] const SphereDamageCommand& command() const noexcept { return command_; }
    [[nodiscard]] std::vector<PreparedDestructionComponent>& components() noexcept {
        return prepared_;
    }

    // A piece whose merged collision proxy needs more boxes than this is rejected.
    static constexpr std::size_t kMaximumProxyBoxes = 256U;

  private:
    [[nodiscard]] bool sphere_may_touch_brick(BrickKey key) const noexcept;
    struct Component {
        std::deque<std::pair<Int3, MaterialId>> voxels;
        Int3 minimum{kInt3Max}, maximum{kInt3Min};
        std::uint64_t mass{}, count{}, x{}, y{}, z{}, xx{}, yy{}, zz{}, xy{}, xz{}, yz{};
    };
    void finish_mass(std::size_t index);
    std::uint64_t sourceRevision_{}, sourceId_{}, workUnits_{}, removed_{};
    SphereDamageCommand command_;
    MaterialMassTable masses_;
    Phase phase_{Phase::Raster};
    std::size_t brickIndex_{}, componentIndex_{}, voxelIndex_{};
    std::map<Int3, MaterialId> unvisited_;
    std::deque<Int3> frontier_;
    std::vector<Component> components_;
    std::vector<PreparedDestructionComponent> prepared_;
};
} // namespace dve
