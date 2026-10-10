#pragma once
#include "dve/editor_voxel_boolean.hpp"

namespace dve::editor {
// Voxel Slice classifies cell centres against a world-space plane. It does not
// cut individual cells into polygon fragments. Cells on the plane belong to
// Front.
enum class VoxelSliceOutput : std::uint8_t { Front, Back, Separate };
struct VoxelSliceCell {
    Int3 voxel{};
    MaterialId material{};
    Float3 worldCenter{};
    bool front{};
};
class EditorVoxelSliceSession {
  public:
    bool begin(const EditorDocument&, EditorObjectId, Float3 point, Float3 normal,
               std::string* error = nullptr);
    bool configure(const EditorDocument&, Float3 point, Float3 normal, VoxelSliceOutput,
                   std::string* error = nullptr);
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] EditorObjectId target() const noexcept { return target_; }
    [[nodiscard]] Float3 point() const noexcept { return point_; }
    [[nodiscard]] Float3 normal() const noexcept { return normal_; }
    [[nodiscard]] VoxelSliceOutput output() const noexcept { return output_; }
    [[nodiscard]] const std::vector<VoxelSliceCell>& cells() const noexcept { return cells_; }
    [[nodiscard]] bool target_unchanged(const EditorDocument&) const;
    [[nodiscard]] std::string blocked_reason(const EditorDocument&) const;
    [[nodiscard]] std::vector<std::string> describe(const EditorDocument&) const;
    CommandResult commit(EditorWorkspace&);
    void cancel() noexcept {
        active_ = false;
        cells_.clear();
    }

  private:
    bool active_{};
    EditorObjectId target_{};
    Float3 point_{}, normal_{1, 0, 0};
    VoxelSliceOutput output_{VoxelSliceOutput::Front};
    std::uint64_t voxelRevision_{};
    RigidTransform transform_{};
    float voxelSize_{};
    std::vector<Int3> anchors_;
    std::vector<VoxelSliceCell> cells_;
};
} // namespace dve::editor
