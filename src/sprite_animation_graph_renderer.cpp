#include "dve/sprite_animation_graph_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace dve {
namespace {

using Color = std::array<float, 4>;

[[nodiscard]] Color severity_color(SpriteAnimationGraphBadgeSeverity severity) noexcept {
    switch (severity) {
    case SpriteAnimationGraphBadgeSeverity::Info: return {0.25F, 0.62F, 0.95F, 1.0F};
    case SpriteAnimationGraphBadgeSeverity::Warning: return {0.96F, 0.68F, 0.18F, 1.0F};
    case SpriteAnimationGraphBadgeSeverity::Error: return {0.94F, 0.24F, 0.24F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

void add_rect(SpriteAnimationGraphDrawList& list, SpriteAnimationGraphRect rect, Color color,
              bool outline = false, float thickness = 1.0F) {
    SpriteAnimationGraphDrawCommand command;
    command.kind = outline ? SpriteAnimationGraphDrawKind::OutlineRect
                           : SpriteAnimationGraphDrawKind::FilledRect;
    command.rect = rect;
    command.color = color;
    command.thickness = thickness;
    list.commands.push_back(std::move(command));
}

void add_text(SpriteAnimationGraphDrawList& list, SpriteVec2 position, std::string text, Color color) {
    SpriteAnimationGraphDrawCommand command;
    command.kind = SpriteAnimationGraphDrawKind::Text;
    command.center = position;
    command.text = std::move(text);
    command.color = color;
    list.commands.push_back(std::move(command));
}

void add_circle(SpriteAnimationGraphDrawList& list, SpriteVec2 center, float radius, Color color) {
    SpriteAnimationGraphDrawCommand command;
    command.kind = SpriteAnimationGraphDrawKind::Circle;
    command.center = center;
    command.radius = radius;
    command.color = color;
    list.commands.push_back(std::move(command));
}

[[nodiscard]] SpriteVec2 cubic(SpriteVec2 p0, SpriteVec2 p1, SpriteVec2 p2, SpriteVec2 p3,
                               float t) noexcept {
    const float u = 1.0F - t;
    const float a = u * u * u;
    const float b = 3.0F * u * u * t;
    const float c = 3.0F * u * t * t;
    const float d = t * t * t;
    return {a * p0.x + b * p1.x + c * p2.x + d * p3.x,
            a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

void append_cubic(std::vector<SpriteVec2>& points, SpriteVec2 p0, SpriteVec2 p1,
                  SpriteVec2 p2, SpriteVec2 p3) {
    constexpr std::size_t segments = 24U;
    if (points.empty()) points.push_back(p0);
    for (std::size_t index = 1U; index <= segments; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(segments);
        points.push_back(cubic(p0, p1, p2, p3, t));
    }
}

[[nodiscard]] std::uint8_t byte_color(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

void blend_pixel(SpriteAnimationGraphReferenceImage& image, int x, int y, Color color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) return;
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width +
                                static_cast<std::size_t>(x)) * 4U;
    const float sourceAlpha = std::clamp(color[3], 0.0F, 1.0F);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const float destination = static_cast<float>(std::to_integer<std::uint8_t>(image.rgba8[offset + channel])) / 255.0F;
        image.rgba8[offset + channel] = std::byte{byte_color(color[channel] * sourceAlpha +
                                                            destination * (1.0F - sourceAlpha))};
    }
    image.rgba8[offset + 3U] = std::byte{255U};
}

void draw_line(SpriteAnimationGraphReferenceImage& image, SpriteVec2 from, SpriteVec2 to,
               Color color, float thickness) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    const int radius = std::max(0, static_cast<int>(std::floor(thickness * 0.5F)));
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const int x = static_cast<int>(std::lround(from.x + dx * t));
        const int y = static_cast<int>(std::lround(from.y + dy * t));
        for (int oy = -radius; oy <= radius; ++oy)
            for (int ox = -radius; ox <= radius; ++ox)
                blend_pixel(image, x + ox, y + oy, color);
    }
}

} // namespace

SpriteAnimationGraphDrawList build_sprite_animation_graph_draw_list(
    const SpriteAnimationGraphFrame& frame) {
    SpriteAnimationGraphDrawList list;
    list.nodeCount = frame.nodes.size();
    list.transitionCount = frame.edges.size();
    list.commentCount = frame.comments.size();
    list.groupCount = frame.groups.size();

    add_rect(list, frame.viewport, {0.055F, 0.065F, 0.085F, 1.0F});
    for (const SpriteAnimationGraphGroupFrame& group : frame.groups) {
        add_rect(list, group.rect, {0.12F, 0.24F, 0.38F, 0.18F});
        add_rect(list, group.rect, {0.28F, 0.58F, 0.92F, 0.75F}, true, 2.0F);
        add_text(list, {group.rect.x + 8.0F, group.rect.y + 16.0F}, group.name,
                 {0.72F, 0.86F, 1.0F, 1.0F});
    }
    for (const SpriteAnimationGraphCommentFrame& comment : frame.comments) {
        add_rect(list, comment.rect, {0.38F, 0.31F, 0.08F, 0.52F});
        add_rect(list, comment.rect, {0.92F, 0.76F, 0.22F, 0.9F}, true, 1.0F);
        add_text(list, {comment.rect.x + 8.0F, comment.rect.y + 18.0F}, comment.text,
                 {1.0F, 0.93F, 0.62F, 1.0F});
    }
    for (const SpriteAnimationGraphEdgeFrame& edge : frame.edges) {
        SpriteAnimationGraphDrawCommand command;
        command.kind = SpriteAnimationGraphDrawKind::Polyline;
        command.thickness = edge.live ? 4.0F : edge.selected ? 3.0F : 2.0F;
        command.color = edge.live ? Color{0.28F, 1.0F, 0.44F, 1.0F}
                                  : edge.selected ? Color{1.0F, 0.75F, 0.22F, 1.0F}
                                                  : edge.anyState ? Color{0.72F, 0.45F, 0.92F, 0.9F}
                                                                  : Color{0.52F, 0.62F, 0.76F, 0.9F};
        if (edge.reroutePoints.empty()) {
            append_cubic(command.points, edge.from, edge.controlA, edge.controlB, edge.to);
        } else {
            command.points.push_back(edge.from);
            command.points.insert(command.points.end(), edge.reroutePoints.begin(), edge.reroutePoints.end());
            command.points.push_back(edge.to);
        }
        list.commands.push_back(std::move(command));
        const SpriteVec2 labelPosition{(edge.from.x + edge.to.x) * 0.5F,
                                       (edge.from.y + edge.to.y) * 0.5F - 6.0F};
        add_text(list, labelPosition, edge.label, {0.86F, 0.9F, 0.98F, 1.0F});
        if (!edge.conditionSummary.empty())
            add_text(list, {labelPosition.x, labelPosition.y + 13.0F}, edge.conditionSummary,
                     {0.64F, 0.72F, 0.84F, 1.0F});
        list.validationBadgeCount += edge.badges.size();
    }
    for (const SpriteAnimationGraphNodeFrame& node : frame.nodes) {
        const Color fill = node.live ? Color{0.12F, 0.42F, 0.22F, 1.0F}
                                    : node.selected ? Color{0.22F, 0.29F, 0.42F, 1.0F}
                                                    : Color{0.13F, 0.16F, 0.22F, 1.0F};
        add_rect(list, node.rect, fill);
        const Color outline = node.initial ? Color{0.95F, 0.73F, 0.20F, 1.0F}
                                          : node.live ? Color{0.30F, 1.0F, 0.46F, 1.0F}
                                                      : Color{0.42F, 0.53F, 0.68F, 1.0F};
        add_rect(list, node.rect, outline, true, node.selected ? 3.0F : 2.0F);
        add_circle(list, {node.inputPort.x + node.inputPort.width * 0.5F,
                          node.inputPort.y + node.inputPort.height * 0.5F}, 5.0F, outline);
        add_circle(list, {node.outputPort.x + node.outputPort.width * 0.5F,
                          node.outputPort.y + node.outputPort.height * 0.5F}, 5.0F, outline);
        add_text(list, {node.rect.x + 12.0F, node.rect.y + 22.0F}, node.title,
                 {0.95F, 0.97F, 1.0F, 1.0F});
        add_text(list, {node.rect.x + 12.0F, node.rect.y + 42.0F}, node.subtitle,
                 {0.62F, 0.72F, 0.84F, 1.0F});
        float badgeX = node.rect.x + 12.0F;
        for (const SpriteAnimationGraphBadge& badge : node.badges) {
            add_circle(list, {badgeX, node.rect.y + node.rect.height - 11.0F}, 4.0F,
                       severity_color(badge.severity));
            badgeX += 12.0F;
        }
        list.validationBadgeCount += node.badges.size();
    }
    if (frame.transitionPreview) {
        SpriteAnimationGraphDrawCommand command;
        command.kind = SpriteAnimationGraphDrawKind::Polyline;
        command.points = {frame.transitionPreview->first, frame.transitionPreview->second};
        command.color = {0.35F, 0.76F, 1.0F, 0.9F};
        command.thickness = 2.0F;
        list.commands.push_back(std::move(command));
    }
    if (frame.marquee) {
        add_rect(list, *frame.marquee, {0.30F, 0.68F, 1.0F, 0.14F});
        add_rect(list, *frame.marquee, {0.30F, 0.68F, 1.0F, 0.9F}, true, 1.0F);
    }
    add_rect(list, frame.minimap.rect, {0.02F, 0.03F, 0.05F, 0.85F});
    add_rect(list, frame.minimap.rect, {0.42F, 0.52F, 0.66F, 0.9F}, true, 1.0F);
    return list;
}

SpriteAnimationGraphReferenceImage rasterize_sprite_animation_graph_reference(
    const SpriteAnimationGraphDrawList& drawList, std::uint32_t width, std::uint32_t height) {
    SpriteAnimationGraphReferenceImage image;
    image.width = width;
    image.height = height;
    if (width == 0U || height == 0U) return image;
    image.rgba8.assign(static_cast<std::size_t>(width) * height * 4U, std::byte{0});
    for (const SpriteAnimationGraphDrawCommand& command : drawList.commands) {
        if (command.kind == SpriteAnimationGraphDrawKind::FilledRect ||
            command.kind == SpriteAnimationGraphDrawKind::OutlineRect) {
            const int x0 = static_cast<int>(std::floor(command.rect.x));
            const int y0 = static_cast<int>(std::floor(command.rect.y));
            const int x1 = static_cast<int>(std::ceil(command.rect.x + command.rect.width));
            const int y1 = static_cast<int>(std::ceil(command.rect.y + command.rect.height));
            if (command.kind == SpriteAnimationGraphDrawKind::FilledRect) {
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) blend_pixel(image, x, y, command.color);
            } else {
                draw_line(image, {static_cast<float>(x0), static_cast<float>(y0)},
                          {static_cast<float>(x1), static_cast<float>(y0)}, command.color, command.thickness);
                draw_line(image, {static_cast<float>(x1), static_cast<float>(y0)},
                          {static_cast<float>(x1), static_cast<float>(y1)}, command.color, command.thickness);
                draw_line(image, {static_cast<float>(x1), static_cast<float>(y1)},
                          {static_cast<float>(x0), static_cast<float>(y1)}, command.color, command.thickness);
                draw_line(image, {static_cast<float>(x0), static_cast<float>(y1)},
                          {static_cast<float>(x0), static_cast<float>(y0)}, command.color, command.thickness);
            }
        } else if (command.kind == SpriteAnimationGraphDrawKind::Polyline) {
            for (std::size_t index = 1U; index < command.points.size(); ++index)
                draw_line(image, command.points[index - 1U], command.points[index], command.color,
                          command.thickness);
        } else if (command.kind == SpriteAnimationGraphDrawKind::Circle) {
            const int radius = std::max(1, static_cast<int>(std::ceil(command.radius)));
            for (int y = -radius; y <= radius; ++y)
                for (int x = -radius; x <= radius; ++x)
                    if (x * x + y * y <= radius * radius)
                        blend_pixel(image, static_cast<int>(std::lround(command.center.x)) + x,
                                    static_cast<int>(std::lround(command.center.y)) + y, command.color);
        } else if (command.kind == SpriteAnimationGraphDrawKind::Text) {
            // Text is represented by deterministic baseline blocks in the reference renderer; native
            // frontends render the retained UTF-8 string through their normal font system.
            const int widthPixels = std::max(2, static_cast<int>(command.text.size()) * 4);
            const int x0 = static_cast<int>(std::lround(command.center.x));
            const int y0 = static_cast<int>(std::lround(command.center.y));
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < widthPixels; ++x) blend_pixel(image, x0 + x, y0 + y, command.color);
        }
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : image.rgba8) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    image.contentHash = hash;
    return image;
}

} // namespace dve
