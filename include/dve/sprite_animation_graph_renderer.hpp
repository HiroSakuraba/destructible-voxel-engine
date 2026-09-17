#pragma once

#include "dve/sprite_animation_authoring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dve {

enum class SpriteAnimationGraphDrawKind : std::uint8_t {
    FilledRect,
    OutlineRect,
    Polyline,
    Circle,
    Text,
};

struct SpriteAnimationGraphDrawCommand {
    SpriteAnimationGraphDrawKind kind{SpriteAnimationGraphDrawKind::FilledRect};
    SpriteAnimationGraphRect rect{};
    std::vector<SpriteVec2> points;
    SpriteVec2 center{};
    float radius{};
    float thickness{1.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::string text;
};

struct SpriteAnimationGraphDrawList {
    std::vector<SpriteAnimationGraphDrawCommand> commands;
    std::size_t nodeCount{};
    std::size_t transitionCount{};
    std::size_t validationBadgeCount{};
    std::size_t commentCount{};
    std::size_t groupCount{};
};

struct SpriteAnimationGraphReferenceImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::uint64_t contentHash{};
};

[[nodiscard]] SpriteAnimationGraphDrawList build_sprite_animation_graph_draw_list(
    const SpriteAnimationGraphFrame& frame);
[[nodiscard]] SpriteAnimationGraphReferenceImage rasterize_sprite_animation_graph_reference(
    const SpriteAnimationGraphDrawList& drawList, std::uint32_t width, std::uint32_t height);

} // namespace dve
