#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/render/cascaded_shadow_map.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

enum class CascadedShadowLayer : std::uint8_t { Static = 0, Dynamic = 1 };

struct CascadedShadowAtlasResources {
    // Static depth is retained until a static-scene refresh. Dynamic depth is independently
    // region-cleared and redrawn, preventing moving casters from leaving stale depth.
    rhi::TextureHandle depthAtlas;
    rhi::TextureViewHandle depthAtlasView;
    rhi::TextureHandle dynamicDepthAtlas;
    rhi::TextureViewHandle dynamicDepthAtlasView;
    rhi::SamplerHandle comparisonSampler;
    rhi::BindGroupLayoutHandle bindGroupLayout;
    rhi::BindGroupHandle bindGroup;
    std::uint32_t width{};
    std::uint32_t height{};
    [[nodiscard]] bool valid() const noexcept;
};

struct CascadedShadowAtlasRegion {
    std::uint32_t cascadeIndex{};
    rhi::Viewport viewport;
    rhi::ScissorRect scissor;
    bool dirty{true};
    bool clearBeforeDraw{};
};

struct CascadedShadowAtlasFramePlan {
    rhi::RenderPassDesc renderPass;
    std::vector<CascadedShadowAtlasRegion> regions;
    CascadedShadowLayer layer{CascadedShadowLayer::Static};
    bool clearAtlas{true};
    [[nodiscard]] bool validate(const CascadedShadowPlan& cascades,
                                std::string* error = nullptr) const noexcept;
};

[[nodiscard]] bool create_cascaded_shadow_atlas(
    rhi::IDevice& device,
    const CascadedShadowPlan& plan,
    CascadedShadowAtlasResources& resources,
    std::string* error = nullptr);
[[nodiscard]] bool destroy_cascaded_shadow_atlas(
    rhi::IDevice& device,
    CascadedShadowAtlasResources& resources,
    std::string* error = nullptr);
[[nodiscard]] CascadedShadowAtlasFramePlan make_cascaded_shadow_atlas_frame_plan(
    const CascadedShadowPlan& plan,
    const CascadedShadowAtlasResources& resources,
    const std::vector<std::uint32_t>& dirtyCascades = {},
    CascadedShadowLayer layer = CascadedShadowLayer::Static);

} // namespace dve::render
