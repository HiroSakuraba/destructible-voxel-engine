#include "dve/render/cascaded_shadow_atlas.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) { if (error) *error = std::move(message); }

rhi::TextureHandle layer_texture(const CascadedShadowAtlasResources& resources,
                                 CascadedShadowLayer layer) noexcept {
    return layer == CascadedShadowLayer::Static ? resources.depthAtlas
                                                : resources.dynamicDepthAtlas;
}
}

bool CascadedShadowAtlasResources::valid() const noexcept {
    return depthAtlas && depthAtlasView && dynamicDepthAtlas && dynamicDepthAtlasView &&
           comparisonSampler && bindGroupLayout && bindGroup && width > 0U && height > 0U;
}

bool destroy_cascaded_shadow_atlas(rhi::IDevice& device, CascadedShadowAtlasResources& r,
                                   std::string* error) {
    bool ok = true;
    std::string local;
    auto destroy = [&](bool result) {
        if (!result) { ok = false; if (error && error->empty()) *error = local; }
        local.clear();
    };
    if (r.bindGroup) destroy(device.destroy_bind_group(r.bindGroup, &local));
    if (r.bindGroupLayout) destroy(device.destroy_bind_group_layout(r.bindGroupLayout, &local));
    if (r.dynamicDepthAtlasView) destroy(device.destroy_texture_view(r.dynamicDepthAtlasView, &local));
    if (r.depthAtlasView) destroy(device.destroy_texture_view(r.depthAtlasView, &local));
    if (r.comparisonSampler) destroy(device.destroy_sampler(r.comparisonSampler, &local));
    if (r.dynamicDepthAtlas) destroy(device.destroy_texture(r.dynamicDepthAtlas, &local));
    if (r.depthAtlas) destroy(device.destroy_texture(r.depthAtlas, &local));
    r = {};
    return ok;
}

bool create_cascaded_shadow_atlas(rhi::IDevice& device, const CascadedShadowPlan& plan,
                                  CascadedShadowAtlasResources& out, std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) { set_error(error, validation); return false; }
    CascadedShadowAtlasResources r;
    auto fail = [&](std::string message) {
        std::string ignored;
        (void)destroy_cascaded_shadow_atlas(device, r, &ignored);
        set_error(error, std::move(message));
        return false;
    };
    std::string local;
    auto create_depth = [&](std::string name, rhi::TextureHandle& texture,
                            rhi::TextureViewHandle& view) -> bool {
        rhi::TextureDesc depth;
        depth.format = rhi::TextureFormat::D32Float;
        depth.width = plan.atlasWidth;
        depth.height = plan.atlasHeight;
        depth.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled |
                      rhi::TextureUsage::CopySource;
        depth.initialState = rhi::ResourceState::DepthWrite;
        depth.debugName = std::move(name);
        texture = device.create_texture(depth, &local);
        if (!texture) return false;
        rhi::TextureViewDesc viewDesc;
        viewDesc.texture = texture;
        viewDesc.debugName = depth.debugName + " view";
        view = device.create_texture_view(viewDesc, &local);
        return static_cast<bool>(view);
    };
    if (!create_depth("Cascaded static shadow depth atlas", r.depthAtlas, r.depthAtlasView))
        return fail(local);
    if (!create_depth("Cascaded dynamic shadow depth atlas", r.dynamicDepthAtlas,
                      r.dynamicDepthAtlasView)) return fail(local);

    rhi::SamplerDesc sampler;
    sampler.minFilter = rhi::FilterMode::Linear;
    sampler.magFilter = rhi::FilterMode::Linear;
    sampler.mipmapFilter = rhi::MipmapFilterMode::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = rhi::AddressMode::ClampToEdge;
    sampler.minimumLod = 0.0F;
    sampler.maximumLod = 0.0F;
    sampler.comparison = true;
    sampler.comparisonOp = rhi::CompareOp::LessEqual;
    sampler.debugName = "Layered cascaded shadow comparison sampler";
    r.comparisonSampler = device.create_sampler(sampler, &local);
    if (!r.comparisonSampler) return fail(local);

    rhi::BindGroupLayoutDesc layout;
    layout.debugName = "Layered cascaded shadow sampled atlas layout";
    layout.bindings = {
        {0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Compute | rhi::ShaderStage::Fragment},
        {1U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Compute | rhi::ShaderStage::Fragment}};
    r.bindGroupLayout = device.create_bind_group_layout(layout, &local);
    if (!r.bindGroupLayout) return fail(local);

    rhi::BindGroupDesc group;
    group.layout = r.bindGroupLayout;
    group.debugName = "Layered cascaded shadow sampled atlases";
    group.entries = {
        {0U, {}, r.depthAtlasView, 0U, 0U, r.comparisonSampler},
        {1U, {}, r.dynamicDepthAtlasView, 0U, 0U, r.comparisonSampler}};
    r.bindGroup = device.create_bind_group(group, &local);
    if (!r.bindGroup) return fail(local);
    r.width = plan.atlasWidth;
    r.height = plan.atlasHeight;
    if (out.valid()) {
        std::string ignored;
        (void)destroy_cascaded_shadow_atlas(device, out, &ignored);
    }
    out = r;
    return true;
}

bool CascadedShadowAtlasFramePlan::validate(const CascadedShadowPlan& cascades,
                                            std::string* error) const noexcept {
    auto fail = [&](const char* message) { set_error(error, message); return false; };
    if (!cascades.validate(error)) return false;
    if (!renderPass.colors.empty()) return fail("shadow atlas frame plan must be depth-only");
    if (!renderPass.depth || !renderPass.depth->texture)
        return fail("shadow atlas frame plan lacks a depth attachment");
    if (regions.size() != cascades.cascades.size())
        return fail("shadow atlas frame-plan region count mismatch");
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        const auto& c = cascades.cascades[i];
        if (r.cascadeIndex != i || r.viewport.x != static_cast<float>(c.atlas.x) ||
            r.viewport.y != static_cast<float>(c.atlas.y) ||
            r.viewport.width != static_cast<float>(c.atlas.width) ||
            r.viewport.height != static_cast<float>(c.atlas.height) ||
            r.scissor.x != static_cast<std::int32_t>(c.atlas.x) ||
            r.scissor.y != static_cast<std::int32_t>(c.atlas.y) ||
            r.scissor.width != c.atlas.width || r.scissor.height != c.atlas.height)
            return fail("shadow atlas frame-plan region mismatch");
        if (r.clearBeforeDraw && (!r.dirty || clearAtlas))
            return fail("shadow atlas region-clear policy is inconsistent");
    }
    return true;
}

CascadedShadowAtlasFramePlan make_cascaded_shadow_atlas_frame_plan(
    const CascadedShadowPlan& plan, const CascadedShadowAtlasResources& resources,
    const std::vector<std::uint32_t>& dirty, CascadedShadowLayer layer) {
    std::string error;
    if (!plan.validate(&error) || !resources.valid() || resources.width != plan.atlasWidth ||
        resources.height != plan.atlasHeight)
        throw std::invalid_argument(error.empty() ? "invalid cascaded shadow atlas resources" : error);
    std::unordered_set<std::uint32_t> dirtySet;
    for (const auto index : dirty) {
        if (index >= plan.cascades.size()) throw std::invalid_argument("dirty cascade index is out of range");
        dirtySet.insert(index);
    }
    CascadedShadowAtlasFramePlan out;
    out.layer = layer;
    out.clearAtlas = dirty.empty() || dirtySet.size() == plan.cascades.size();
    out.renderPass.depth = rhi::RenderPassDepthAttachment{
        layer_texture(resources, layer), out.clearAtlas, 1.0F};
    out.renderPass.debugName = layer == CascadedShadowLayer::Static
        ? "Static cascaded shadow atlas pass" : "Dynamic cascaded shadow atlas pass";
    for (const auto& c : plan.cascades) {
        CascadedShadowAtlasRegion region;
        region.cascadeIndex = c.index;
        region.viewport = {static_cast<float>(c.atlas.x), static_cast<float>(c.atlas.y),
                           static_cast<float>(c.atlas.width), static_cast<float>(c.atlas.height),
                           0.0F, 1.0F};
        region.scissor = {static_cast<std::int32_t>(c.atlas.x),
                          static_cast<std::int32_t>(c.atlas.y), c.atlas.width, c.atlas.height};
        region.dirty = dirty.empty() || dirtySet.contains(c.index);
        region.clearBeforeDraw = region.dirty && !out.clearAtlas;
        out.regions.push_back(region);
    }
    return out;
}
} // namespace dve::render
