#include "dve/render/live_environment_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace dve::render {
namespace {
constexpr std::size_t kDefaultObjectConstantCapacity = 4U * 1024U * 1024U;
constexpr std::size_t kDefaultCascadeConstantCapacity = 64U * 1024U;

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    if (alignment <= 1U) return value;
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

struct PreparedSubmesh {
    const LivePolygonDraw* draw{};
    const PolygonSubmesh* submesh{};
    rhi::BindGroupHandle objectGroup;
    rhi::BindGroupHandle shadowMaterialGroup;
    rhi::BindGroupHandle mainMaterialGroup;
    bool masked{};
    bool alphaTexturePresent{};
    bool baseColorTexturePresent{};
    bool opacityTexturePresent{};
    bool dashr{};
    std::uint32_t submeshIndex{};
    const MainMaterialDescriptor* mainDescriptor{};
};

GpuLiveObjectConstants make_object_constants(const LivePolygonDraw& draw,
                                             const PolygonSubmesh& submesh) {
    GpuLiveObjectConstants result;
    result.objectToWorld = draw.objectToWorld;
    const std::uint64_t objectId = draw.asset->objectId;
    result.objectIdLow = static_cast<std::uint32_t>(objectId & 0xFFFFFFFFULL);
    result.objectIdHigh = static_cast<std::uint32_t>(objectId >> 32U);
    result.materialIndex = submesh.materialIndex;
    const auto& material = draw.asset->materials[submesh.materialIndex];
    const auto& binding = draw.asset->materialBindings[submesh.materialIndex];
    const bool masked = material.blendMode == MaterialBlendMode::Masked;
    const bool baseColorTexture = binding.baseColor.texture.has_value();
    const bool opacityTexture = binding.opacity.texture.has_value();
    const bool alphaTexture = baseColorTexture || opacityTexture;
    if (masked) result.flags |= kLiveObjectFlagAlphaMasked;
    if (alphaTexture) result.flags |= kLiveObjectFlagAlphaTexturePresent;
    if (binding.doubleSided) result.flags |= kLiveObjectFlagDoubleSided;
    if (baseColorTexture) result.flags |= kLiveObjectFlagBaseColorTexturePresent;
    if (opacityTexture) result.flags |= kLiveObjectFlagOpacityTexturePresent;
    result.alphaCutoff = binding.alphaCutoff;
    result.baseAlpha = material.baseColor.w;
    result.normalScale = binding.normalScale;
    return result;
}

GpuLiveCascadeConstants make_cascade_constants(const CascadedShadowPlan& plan,
                                               const CascadedShadowCascade& cascade) {
    GpuLiveCascadeConstants result;
    result.centerRadius = {cascade.snappedCenter.x, cascade.snappedCenter.y,
                           cascade.snappedCenter.z, cascade.radiusMeters};
    result.lightRight = {cascade.lightRight.x, cascade.lightRight.y, cascade.lightRight.z, 0.0F};
    result.lightUp = {cascade.lightUp.x, cascade.lightUp.y, cascade.lightUp.z, 0.0F};
    result.lightForward = {cascade.lightForward.x, cascade.lightForward.y,
                           cascade.lightForward.z, 0.0F};
    result.depthAndAtlas = {cascade.minimumLightDepth, cascade.maximumLightDepth,
                            static_cast<float>(cascade.index), 0.0F};
    const float atlasWidth = static_cast<float>(std::max(plan.atlasWidth, 1U));
    const float atlasHeight = static_cast<float>(std::max(plan.atlasHeight, 1U));
    result.atlasScaleOffset = {
        static_cast<float>(cascade.atlas.width) / atlasWidth,
        static_cast<float>(cascade.atlas.height) / atlasHeight,
        static_cast<float>(cascade.atlas.x) / atlasWidth,
        static_cast<float>(cascade.atlas.y) / atlasHeight};
    return result;
}

bool create_range_group(rhi::IDevice& device, rhi::BindGroupLayoutHandle layout,
                        rhi::BufferHandle buffer, std::size_t offset, std::size_t bytes,
                        std::string debugName, rhi::BindGroupHandle& output,
                        std::string* error) {
    rhi::BindGroupDesc desc;
    desc.layout = layout;
    desc.debugName = std::move(debugName);
    desc.entries = {{0U, buffer, {}, offset, bytes, {}}};
    output = device.create_bind_group(desc, error);
    return static_cast<bool>(output);
}

bool bind_mesh(rhi::IDevice& device, rhi::CommandListHandle commands,
               const LivePolygonDraw& draw, std::string* error) {
    if (!draw.mirror || !draw.asset || draw.instanceCount == 0U ||
        !draw.mirror->vertex_buffer() || !draw.mirror->index_buffer()) {
        set_error(error, "live environment polygon draw is incomplete");
        return false;
    }
    return device.bind_vertex_buffer(commands, 0U, draw.mirror->vertex_buffer(), 0U,
                                     static_cast<std::uint32_t>(sizeof(GpuPolygonVertex)), error) &&
           device.bind_index_buffer(commands, draw.mirror->index_buffer(), 0U,
                                    rhi::IndexFormat::Uint32, error);
}

Float3 dashr_cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float dashr_dot(Float3 a, Float3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }

bool make_dashr_shadow_draw(const PreparedSubmesh& item,
                            const CascadedShadowCascade& cascade,
                            DashrShadowDraw& out, std::string* error) {
    const auto& m = item.draw->objectToWorld;
    const Float3 x{m[0],m[1],m[2]}, y{m[4],m[5],m[6]}, z{m[8],m[9],m[10]};
    const float determinant = dashr_dot(x,dashr_cross(y,z));
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8F) {
        set_error(error, "DASHR shadow object transform is singular");
        return false;
    }
    const Float3 forward = cascade.lightForward;
    out.lightRayDirectionObject = {
        dashr_dot(dashr_cross(y,z),forward)/determinant,
        dashr_dot(dashr_cross(z,x),forward)/determinant,
        dashr_dot(dashr_cross(x,y),forward)/determinant};
    const float radius = std::max(cascade.radiusMeters,1.0e-5F);
    const float depthRange = std::max(
        cascade.maximumLightDepth-cascade.minimumLightDepth,1.0e-5F);
    Matrix4 worldToClip = Matrix4::identity();
    const Float3 right = cascade.lightRight, up = cascade.lightUp;
    const Float3 center = cascade.snappedCenter;
    worldToClip.values = {
        right.x/radius,-up.x/radius,forward.x/depthRange,0,
        right.y/radius,-up.y/radius,forward.y/depthRange,0,
        right.z/radius,-up.z/radius,forward.z/depthRange,0,
        -dashr_dot(center,right)/radius,dashr_dot(center,up)/radius,
        -cascade.minimumLightDepth/depthRange,1};
    Matrix4 objectToWorld;
    objectToWorld.values = m;
    out.objectToLightClip = multiply(worldToClip,objectToWorld).values;
    out.shell = &item.draw->dashr->shell();
    out.atlas = &item.draw->dashr->atlas();
    out.shellSubmeshIndex = item.submeshIndex;
    const auto heightIndex = static_cast<std::uint32_t>(MainMaterialTextureChannel::Height);
    out.heightView = item.mainDescriptor->textureViews[heightIndex];
    out.heightSampler = item.mainDescriptor->samplers[heightIndex];
    const auto& transform = item.draw->asset->materialBindings[
        item.submesh->materialIndex].mapping.baseTransform;
    out.heightUvScale = transform.scale;
    out.heightUvOffset = transform.offset;
    out.heightUvRotationRadians = transform.rotationRadians;
    out.settings = item.draw->dashr->settings();
    return true;
}
} // namespace

bool LiveEnvironmentShaderBytecode::valid() const noexcept {
    return !skyboxVertex.empty() && !skyboxFragment.empty() &&
           !materialVertex.empty() && !materialFragment.empty() &&
           !shadowVertex.empty();
}

bool LiveEnvironmentRendererResources::valid() const noexcept {
    return frameConstants && objectConstants && cascadeConstants &&
           frameLayout && objectLayout && cascadeLayout && frameGroup &&
           skyboxPipeline && materialPipeline && shadowOpaquePipeline &&
           skyboxVertices && skyboxIndices && frameConstantCapacity > 0U &&
           objectConstantCapacity >= sizeof(GpuLiveObjectConstants) &&
           cascadeConstantCapacity >= sizeof(GpuLiveCascadeConstants) &&
           objectConstantStride >= sizeof(GpuLiveObjectConstants) &&
           cascadeConstantStride >= sizeof(GpuLiveCascadeConstants) &&
           materialResidency && shadowMaterials && shadowMaterials->valid() &&
           mainMaterials && mainMaterials->valid();
}

bool destroy_live_environment_renderer(rhi::IDevice& device,
                                       LiveEnvironmentRendererResources& r,
                                       std::string* error) {
    device.wait_idle();
    bool ok = true;
    std::string local;
    auto destroy = [&](bool result) {
        if (!result) {
            ok = false;
            if (error && error->empty()) *error = local;
        }
        local.clear();
    };
    for (auto& batch : r.retiredGroups)
        for (auto group : batch.groups)
            if (group) destroy(device.destroy_bind_group(group, &local));
    r.retiredGroups.clear();
    if (r.skyboxPipeline) destroy(device.destroy_graphics_pipeline(r.skyboxPipeline, &local));
    if (r.materialPipeline) destroy(device.destroy_graphics_pipeline(r.materialPipeline, &local));
    if (r.shadowOpaquePipeline) destroy(device.destroy_graphics_pipeline(r.shadowOpaquePipeline, &local));
    if (r.shadowMaskedPipeline) destroy(device.destroy_graphics_pipeline(r.shadowMaskedPipeline, &local));
    r.shadowMaterials.reset();
    r.mainMaterials.reset();
    r.materialResidency.reset();
    if (r.frameGroup) destroy(device.destroy_bind_group(r.frameGroup, &local));
    if (r.frameLayout) destroy(device.destroy_bind_group_layout(r.frameLayout, &local));
    if (r.objectLayout) destroy(device.destroy_bind_group_layout(r.objectLayout, &local));
    if (r.cascadeLayout) destroy(device.destroy_bind_group_layout(r.cascadeLayout, &local));
    if (r.frameConstants) destroy(device.destroy_buffer(r.frameConstants, &local));
    if (r.objectConstants) destroy(device.destroy_buffer(r.objectConstants, &local));
    if (r.cascadeConstants) destroy(device.destroy_buffer(r.cascadeConstants, &local));
    if (r.skyboxVertices) destroy(device.destroy_buffer(r.skyboxVertices, &local));
    if (r.skyboxIndices) destroy(device.destroy_buffer(r.skyboxIndices, &local));
    r = {};
    return ok;
}

bool create_live_environment_renderer(rhi::IDevice& device,
                                      const EnvironmentLightingGpuResources& lighting,
                                      const CascadedShadowAtlasResources& shadowAtlas,
                                      const LiveEnvironmentShaderBytecode& bytecode,
                                      std::size_t maximumFrameConstantBytes,
                                      rhi::TextureFormat colorFormat,
                                      LiveEnvironmentRendererResources& out,
                                      std::string* error) {
    if (!lighting.valid() || !shadowAtlas.valid() || !bytecode.valid() ||
        maximumFrameConstantBytes == 0U || colorFormat == rhi::TextureFormat::D32Float) {
        set_error(error, "live environment renderer creation arguments are invalid");
        return false;
    }
    LiveEnvironmentRendererResources r;
    std::string local;
    auto fail = [&](std::string message) {
        std::string ignored;
        (void)destroy_live_environment_renderer(device, r, &ignored);
        set_error(error, std::move(message));
        return false;
    };
    r.materialResidency = std::make_unique<MaterialResourceResidency>(device);
    r.shadowMaterials = std::make_unique<ShadowMaterialDescriptorTable>(device, *r.materialResidency);
    if (!r.shadowMaterials->initialize(&local)) return fail(local);
    r.mainMaterials = std::make_unique<MainMaterialDescriptorTable>(device, *r.materialResidency);
    if (!r.mainMaterials->initialize(shadowAtlas.depthAtlasView,
                                     shadowAtlas.dynamicDepthAtlasView,
                                     shadowAtlas.comparisonSampler, &local)) return fail(local);
    const std::size_t uniformAlignment = std::max<std::size_t>(
        16U, device.capabilities().minUniformBufferOffsetAlignment);
    r.frameConstantCapacity = maximumFrameConstantBytes;
    r.objectConstantStride = align_up(sizeof(GpuLiveObjectConstants), uniformAlignment);
    r.cascadeConstantStride = align_up(sizeof(GpuLiveCascadeConstants), uniformAlignment);
    r.objectConstantCapacity = align_up(kDefaultObjectConstantCapacity, r.objectConstantStride);
    r.cascadeConstantCapacity = align_up(kDefaultCascadeConstantCapacity, r.cascadeConstantStride);

    auto create_constant_buffer = [&](std::size_t bytes, std::string name) {
        rhi::BufferDesc desc;
        desc.bytes = bytes;
        desc.usage = rhi::BufferUsage::Constant | rhi::BufferUsage::CopyDestination;
        desc.memory = rhi::MemoryDomain::Upload;
        desc.initialState = rhi::ResourceState::ShaderRead;
        desc.debugName = std::move(name);
        return device.create_buffer(desc, &local);
    };
    r.frameConstants = create_constant_buffer(r.frameConstantCapacity, "Live environment frame constants");
    if (!r.frameConstants) return fail(local);
    r.objectConstants = create_constant_buffer(r.objectConstantCapacity, "Live environment object constants");
    if (!r.objectConstants) return fail(local);
    r.cascadeConstants = create_constant_buffer(r.cascadeConstantCapacity, "Live environment cascade constants");
    if (!r.cascadeConstants) return fail(local);

    rhi::BindGroupLayoutDesc frameLayout;
    frameLayout.debugName = "Live environment frame layout";
    frameLayout.bindings = {{0U, rhi::BindingType::UniformBuffer,
                             rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment}};
    r.frameLayout = device.create_bind_group_layout(frameLayout, &local);
    if (!r.frameLayout) return fail(local);
    rhi::BindGroupLayoutDesc objectLayout;
    objectLayout.debugName = "Live environment object layout";
    objectLayout.bindings = {{0U, rhi::BindingType::UniformBuffer,
                              rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment}};
    r.objectLayout = device.create_bind_group_layout(objectLayout, &local);
    if (!r.objectLayout) return fail(local);
    rhi::BindGroupLayoutDesc cascadeLayout;
    cascadeLayout.debugName = "Live environment cascade layout";
    cascadeLayout.bindings = {{0U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Vertex}};
    r.cascadeLayout = device.create_bind_group_layout(cascadeLayout, &local);
    if (!r.cascadeLayout) return fail(local);

    rhi::BindGroupDesc frameGroup;
    frameGroup.layout = r.frameLayout;
    frameGroup.debugName = "Live environment frame group";
    frameGroup.entries = {{0U, r.frameConstants, {}, 0U, maximumFrameConstantBytes, {}}};
    r.frameGroup = device.create_bind_group(frameGroup, &local);
    if (!r.frameGroup) return fail(local);

    rhi::BufferDesc vertices;
    vertices.bytes = 16U;
    vertices.usage = rhi::BufferUsage::Vertex;
    vertices.memory = rhi::MemoryDomain::DeviceLocal;
    vertices.initialState = rhi::ResourceState::ShaderRead;
    vertices.debugName = "Skybox procedural vertex placeholder";
    r.skyboxVertices = device.create_buffer(vertices, &local);
    if (!r.skyboxVertices) return fail(local);
    const std::array<std::uint32_t, 3> indices{0U, 1U, 2U};
    rhi::BufferDesc indexDesc;
    indexDesc.bytes = sizeof(indices);
    indexDesc.usage = rhi::BufferUsage::Index;
    indexDesc.memory = rhi::MemoryDomain::Upload;
    indexDesc.initialState = rhi::ResourceState::ShaderRead;
    indexDesc.debugName = "Skybox fullscreen triangle indices";
    r.skyboxIndices = device.create_buffer(indexDesc, &local);
    if (!r.skyboxIndices) return fail(local);
    if (!device.write_buffer(r.skyboxIndices, 0U, std::as_bytes(std::span(indices)), &local))
        return fail(local);

    auto make_pipeline = [&](std::string name, const std::vector<std::byte>& vs,
                             const std::vector<std::byte>& ps,
                             std::vector<rhi::BindGroupLayoutHandle> layouts,
                             std::optional<rhi::TextureFormat> pipelineColor,
                             bool depthTest, bool depthWrite, rhi::CullMode cull) {
        rhi::GraphicsPipelineDesc desc;
        desc.debugName = std::move(name);
        desc.vertexBytecode = vs;
        desc.fragmentBytecode = ps;
        if (ps.empty()) desc.fragmentEntryPoint.clear();
        desc.bindGroupLayouts = std::move(layouts);
        desc.colorFormat = pipelineColor;
        desc.depthFormat = rhi::TextureFormat::D32Float;
        desc.depthTest = depthTest;
        desc.depthWrite = depthWrite;
        desc.depthCompare = rhi::CompareOp::LessEqual;
        desc.cullMode = cull;
        return device.create_graphics_pipeline(desc, &local);
    };
    r.skyboxPipeline = make_pipeline("Live environment skybox", bytecode.skyboxVertex,
        bytecode.skyboxFragment, {r.frameLayout, lighting.bindGroupLayout}, colorFormat,
        false, false, rhi::CullMode::Disabled);
    if (!r.skyboxPipeline) return fail(local);
    r.materialPipeline = make_pipeline("Live IBL material", bytecode.materialVertex,
        bytecode.materialFragment,
        {r.frameLayout, r.objectLayout, lighting.bindGroupLayout, r.mainMaterials->layout()},
        colorFormat, true, true, rhi::CullMode::Disabled);
    if (!r.materialPipeline) return fail(local);
    r.shadowOpaquePipeline = make_pipeline("Live opaque depth-only shadow caster",
        bytecode.shadowVertex, {}, {r.frameLayout, r.objectLayout, r.cascadeLayout},
        std::nullopt, true, true, rhi::CullMode::Disabled);
    if (!r.shadowOpaquePipeline) return fail(local);
    if (!bytecode.shadowFragment.empty()) {
        r.shadowMaskedPipeline = make_pipeline("Live alpha-masked depth-only shadow caster",
            bytecode.shadowVertex, bytecode.shadowFragment,
            {r.frameLayout, r.objectLayout, r.cascadeLayout, r.shadowMaterials->layout()},
            std::nullopt, true, true, rhi::CullMode::Disabled);
        if (!r.shadowMaskedPipeline) return fail(local);
    }
    if (out.valid()) {
        std::string ignored;
        (void)destroy_live_environment_renderer(device, out, &ignored);
    }
    out = std::move(r);
    return true;
}

bool record_live_environment_frame(rhi::IDevice& device,
                                   const EnvironmentLightingGpuResources& lighting,
                                   const CascadedShadowPlan& shadowPlan,
                                   const CascadedShadowAtlasResources& shadowAtlas,
                                   LiveEnvironmentRendererResources& renderer,
                                   const LiveEnvironmentFrameDesc& frame,
                                   LiveEnvironmentFrameStats& stats,
                                   rhi::FenceHandle* fence,
                                   std::string* error) {
    stats = {};
    if (!lighting.valid() || !shadowPlan.validate(error) || !shadowAtlas.valid() ||
        !renderer.valid() || !frame.colorTarget || !frame.depthTarget ||
        frame.width == 0U || frame.height == 0U || frame.frameConstants.empty() ||
        frame.frameConstants.size() > renderer.frameConstantCapacity) {
        set_error(error, "live environment frame description is invalid");
        return false;
    }
    if (!renderer.retiredGroups.empty()) {
        device.wait_idle();
        for (auto& batch : renderer.retiredGroups) {
            for (auto group : batch.groups) {
                if (group && !device.destroy_bind_group(group, error)) return false;
            }
        }
        renderer.retiredGroups.clear();
    }
    if (!device.write_buffer(renderer.frameConstants, 0U, frame.frameConstants, error)) return false;

    std::vector<PreparedSubmesh> prepared;
    std::unordered_set<DashrLiveSurfaceInstance*> usedDashrInstances;
    std::vector<rhi::BindGroupHandle> transientGroups;
    std::vector<rhi::BindGroupHandle> dashrShadowGroups;
    std::size_t dashrShadowConstantOffset = 0U;
    std::size_t objectOffset = 0U;
    auto cleanup_groups = [&]() {
        std::string ignored;
        for (auto it = transientGroups.rbegin(); it != transientGroups.rend(); ++it)
            if (*it) (void)device.destroy_bind_group(*it, &ignored);
        transientGroups.clear();
        for (auto it = dashrShadowGroups.rbegin(); it != dashrShadowGroups.rend(); ++it)
            if (*it) (void)device.destroy_bind_group(*it, &ignored);
        dashrShadowGroups.clear();
    };
    auto fail = [&]() { cleanup_groups(); return false; };

    if (frame.dashrRenderer && !frame.dashrRenderer->retiredBindGroups.empty())
        device.wait_idle();

    for (const auto& draw : frame.polygonDraws) {
        if (!draw.mirror || !draw.asset || draw.instanceCount == 0U ||
            !draw.mirror->vertex_buffer() || !draw.mirror->index_buffer()) {
            set_error(error, "live environment polygon draw is incomplete");
            return fail();
        }
        if (!draw.dashrSubmeshIndices.empty()) {
            if (!draw.dashr || !frame.dashrRenderer ||
                !frame.dashrRenderer->pbr_valid() || draw.instanceCount != 1U ||
                frame.frameConstants.size() < 80U ||
                draw.dashr->asset_content_hash() != draw.asset->contentHash ||
                !usedDashrInstances.insert(draw.dashr).second ||
                std::any_of(draw.dashrSubmeshIndices.begin(), draw.dashrSubmeshIndices.end(),
                    [&](std::uint32_t index) { return index >= draw.asset->submeshes.size(); }) ||
                !draw.dashr->update_pose(draw.dashrPose, error)) {
                if (error && error->empty())
                    set_error(error, "DASHR scene draw requires one valid posed instance and renderer");
                return fail();
            }
            for (std::size_t index = 0U; index < draw.dashrSubmeshIndices.size(); ++index)
                if (std::find(draw.dashrSubmeshIndices.begin() +
                                  static_cast<std::ptrdiff_t>(index + 1U),
                              draw.dashrSubmeshIndices.end(),
                              draw.dashrSubmeshIndices[index]) !=
                    draw.dashrSubmeshIndices.end()) {
                    set_error(error, "DASHR submesh index is selected more than once");
                    return fail();
                }
        }
        for (std::uint32_t submeshIndex = 0U; submeshIndex < draw.asset->submeshes.size();
             ++submeshIndex) {
            const auto& submesh = draw.asset->submeshes[submeshIndex];
            const bool dashr = std::find(draw.dashrSubmeshIndices.begin(),
                                         draw.dashrSubmeshIndices.end(), submeshIndex) !=
                               draw.dashrSubmeshIndices.end();
            if (dashr) {
                const auto& material = draw.asset->materials[submesh.materialIndex];
                const auto& binding = draw.asset->materialBindings[submesh.materialIndex];
                if (material.blendMode != MaterialBlendMode::Opaque ||
                    binding.mapping.mappingMode != MaterialMappingMode::UV0 ||
                    !binding.height.texture || binding.height.texcoord != 0U) {
                    set_error(error, "DASHR camera path requires opaque UV0 material with a height texture");
                    return fail();
                }
            }
            if (objectOffset + sizeof(GpuLiveObjectConstants) > renderer.objectConstantCapacity) {
                set_error(error, "live environment object constant capacity exceeded");
                return fail();
            }
            const auto constants = make_object_constants(draw, submesh);
            if (!device.write_buffer(renderer.objectConstants, objectOffset,
                                     std::as_bytes(std::span(&constants, 1U)), error)) return fail();
            rhi::BindGroupHandle group;
            if (!create_range_group(device, renderer.objectLayout, renderer.objectConstants,
                                    objectOffset, sizeof(constants),
                                    "Live object constant range", group, error)) return fail();
            transientGroups.push_back(group);
            const bool masked = (constants.flags & kLiveObjectFlagAlphaMasked) != 0U;
            const bool alphaTexture = (constants.flags & kLiveObjectFlagAlphaTexturePresent) != 0U;
            const bool baseColorTexture = (constants.flags & kLiveObjectFlagBaseColorTexturePresent) != 0U;
            const bool opacityTexture = (constants.flags & kLiveObjectFlagOpacityTexturePresent) != 0U;
            rhi::BindGroupHandle shadowMaterialGroup = renderer.shadowMaterials->fallback_group();
            const auto mainBefore = renderer.mainMaterials->stats().descriptorCacheHits;
            if (!renderer.mainMaterials->ensure_material(*draw.asset, submesh.materialIndex, error))
                return fail();
            const auto* mainDescriptor = renderer.mainMaterials->find(
                draw.asset->contentHash, submesh.materialIndex);
            if (!mainDescriptor || !mainDescriptor->bindGroup ||
                !mainDescriptor->materialRecordBuffer || !mainDescriptor->mappingRecordBuffer) {
                set_error(error, "main material descriptor and GPU records were not published");
                return fail();
            }
            if (renderer.mainMaterials->stats().descriptorCacheHits > mainBefore)
                ++stats.persistentMainMaterialDescriptorHits;
            if (masked) {
                const auto before = renderer.shadowMaterials->stats().descriptorCacheHits;
                if (!renderer.shadowMaterials->ensure_material(*draw.asset, submesh.materialIndex, error))
                    return fail();
                const auto* descriptor = renderer.shadowMaterials->find(
                    draw.asset->contentHash, submesh.materialIndex);
                if (!descriptor || !descriptor->bindGroup) {
                    set_error(error, "shadow material descriptor was not published");
                    return fail();
                }
                shadowMaterialGroup = descriptor->bindGroup;
                if (renderer.shadowMaterials->stats().descriptorCacheHits > before)
                    ++stats.persistentMaterialDescriptorHits;
            }
            prepared.push_back({&draw, &submesh, group, shadowMaterialGroup,
                                mainDescriptor->bindGroup, masked, alphaTexture,
                                baseColorTexture, opacityTexture, dashr, submeshIndex,
                                mainDescriptor});
            objectOffset += renderer.objectConstantStride;
            ++stats.objectConstantRanges;
        }
    }

    std::vector<rhi::BindGroupHandle> cascadeGroups(shadowPlan.cascades.size());
    std::size_t cascadeOffset = 0U;
    for (const auto& cascade : shadowPlan.cascades) {
        if (cascadeOffset + sizeof(GpuLiveCascadeConstants) > renderer.cascadeConstantCapacity) {
            set_error(error, "live environment cascade constant capacity exceeded");
            return fail();
        }
        const auto constants = make_cascade_constants(shadowPlan, cascade);
        if (!device.write_buffer(renderer.cascadeConstants, cascadeOffset,
                                 std::as_bytes(std::span(&constants, 1U)), error)) return fail();
        rhi::BindGroupHandle group;
        if (!create_range_group(device, renderer.cascadeLayout, renderer.cascadeConstants,
                                cascadeOffset, sizeof(constants),
                                "Live cascade constant range", group, error)) return fail();
        cascadeGroups[cascade.index] = group;
        transientGroups.push_back(group);
        cascadeOffset += renderer.cascadeConstantStride;
        ++stats.cascadeConstantRanges;
    }

    auto commands = device.begin_commands(rhi::QueueKind::Graphics, "Live environment frame", error);
    if (!commands) return fail();

    if (frame.drawShadowCasters) {
        const auto& staticDirty = frame.staticDirtyCascades.empty()
            ? frame.dirtyCascades : frame.staticDirtyCascades;
        const auto& dynamicDirty = frame.dynamicDirtyCascades.empty()
            ? frame.dirtyCascades : frame.dynamicDirtyCascades;

        auto render_shadow_layer = [&](CascadedShadowLayer layer,
                                       const std::vector<std::uint32_t>& dirty,
                                       bool renderStaticCasters) -> bool {
            const auto shadowFrame = make_cascaded_shadow_atlas_frame_plan(
                shadowPlan, shadowAtlas, dirty, layer);
            if (!shadowFrame.validate(shadowPlan, error)) return false;
            if (!device.begin_render_pass(commands, shadowFrame.renderPass, error)) return false;
            if (layer == CascadedShadowLayer::Static) ++stats.staticAtlasPasses;
            else ++stats.dynamicAtlasPasses;
            for (const auto& region : shadowFrame.regions) {
                if (!region.dirty) continue;
                if (!device.set_viewport(commands, region.viewport, error) ||
                    !device.set_scissor(commands, region.scissor, error)) return false;
                if (region.clearBeforeDraw) {
                    if (!device.clear_depth_region(commands, 1.0F, region.scissor, error))
                        return false;
                    ++stats.shadowDepthRegionsCleared;
                }
                for (const auto& item : prepared) {
                    if (item.dashr) continue;
                    if (!item.draw->castsShadow ||
                        item.draw->staticShadowCaster != renderStaticCasters) continue;
                    const auto pipeline = item.masked && renderer.shadowMaskedPipeline
                        ? renderer.shadowMaskedPipeline : renderer.shadowOpaquePipeline;
                    if (!device.bind_graphics_pipeline(commands, pipeline, error) ||
                        !device.bind_graphics_bind_group(commands, 0U, renderer.frameGroup, error) ||
                        !device.bind_graphics_bind_group(commands, 1U, item.objectGroup, error) ||
                        !device.bind_graphics_bind_group(
                            commands, 2U, cascadeGroups[region.cascadeIndex], error) ||
                        (item.masked && !device.bind_graphics_bind_group(
                            commands, 3U, item.shadowMaterialGroup, error)) ||
                        !bind_mesh(device, commands, *item.draw, error) ||
                        !device.draw_indexed(commands, item.submesh->indexCount,
                                             item.draw->instanceCount,
                                             item.submesh->firstIndex, 0, 0U, error)) return false;
                    ++stats.shadowDraws;
                    stats.shadowTriangles +=
                        static_cast<std::uint64_t>(item.submesh->indexCount / 3U) *
                        item.draw->instanceCount;
                    if (item.masked) ++stats.alphaMaskedShadowDraws;
                    if (item.masked && item.alphaTexturePresent)
                        ++stats.texturedAlphaShadowDraws;
                    if (item.masked && item.baseColorTexturePresent)
                        ++stats.baseColorAlphaShadowDraws;
                    if (item.masked && item.opacityTexturePresent)
                        ++stats.opacityTextureShadowDraws;
                    if (renderStaticCasters) ++stats.staticShadowDraws;
                    else ++stats.dynamicShadowDraws;
                }
                std::vector<DashrShadowDraw> dashrCasters;
                for (const auto& item : prepared) {
                    if (!item.dashr || !item.draw->castsShadow ||
                        item.draw->staticShadowCaster != renderStaticCasters) continue;
                    DashrShadowDraw dashrDraw;
                    if (!make_dashr_shadow_draw(item,
                            shadowPlan.cascades[region.cascadeIndex],dashrDraw,error)) return false;
                    dashrCasters.push_back(dashrDraw);
                }
                if (!dashrCasters.empty()) {
                    DashrShadowFrameStats dashrStats;
                    if (!record_dashr_shadow_draws_in_pass(device,commands,*frame.dashrRenderer,
                            dashrCasters,dashrShadowConstantOffset,dashrShadowGroups,
                            dashrStats,error)) return false;
                    dashrShadowConstantOffset += dashrStats.draws *
                        frame.dashrRenderer->constantStride;
                    stats.dashrShadowDraws += dashrStats.draws;
                    stats.shadowDraws += dashrStats.draws;
                    stats.shadowTriangles += dashrStats.shellTriangles;
                    if (renderStaticCasters) stats.staticShadowDraws += dashrStats.draws;
                    else stats.dynamicShadowDraws += dashrStats.draws;
                }
                ++stats.cascadesRendered;
                if (renderStaticCasters) ++stats.staticCascadesRendered;
                else ++stats.dynamicCascadesRendered;
            }
            return device.end_render_pass(commands, error);
        };

        if (frame.refreshStaticShadowCasters) {
            if (!render_shadow_layer(CascadedShadowLayer::Static, staticDirty, true)) return fail();
        } else {
            const std::uint64_t affectedCascadeCount = staticDirty.empty()
                ? shadowPlan.cascades.size() : staticDirty.size();
            for (const auto& item : prepared)
                if (item.draw->castsShadow && item.draw->staticShadowCaster)
                    stats.staticShadowDrawsSkipped += affectedCascadeCount;
        }
        // Dynamic depth is independent from static depth. Dirty regions are cleared before redraw,
        // so moved or removed dynamic casters cannot leave stale occlusion behind.
        if (!render_shadow_layer(CascadedShadowLayer::Dynamic, dynamicDirty, false)) return fail();
    }

    rhi::RenderPassDesc pass;
    pass.debugName = "Live environment main pass";
    pass.colors.push_back({frame.colorTarget, true, 0.0F, 0.0F, 0.0F, 1.0F});
    pass.depth = rhi::RenderPassDepthAttachment{frame.depthTarget, true, 1.0F};
    if (!device.begin_render_pass(commands, pass, error) ||
        !device.set_viewport(commands, {0.0F, 0.0F, static_cast<float>(frame.width),
                                        static_cast<float>(frame.height), 0.0F, 1.0F}, error) ||
        !device.set_scissor(commands, {0, 0, frame.width, frame.height}, error)) return fail();
    if (frame.drawSkybox) {
        if (!device.bind_graphics_pipeline(commands, renderer.skyboxPipeline, error) ||
            !device.bind_graphics_bind_group(commands, 0U, renderer.frameGroup, error) ||
            !device.bind_graphics_bind_group(commands, 1U, lighting.bindGroup, error) ||
            !device.bind_vertex_buffer(commands, 0U, renderer.skyboxVertices, 0U, 4U, error) ||
            !device.bind_index_buffer(commands, renderer.skyboxIndices, 0U,
                                      rhi::IndexFormat::Uint32, error) ||
            !device.draw_indexed(commands, 3U, 1U, 0U, 0, 0U, error)) return fail();
        ++stats.skyboxDraws;
    }
    if (frame.drawMaterials && !prepared.empty()) {
        if (!device.bind_graphics_pipeline(commands, renderer.materialPipeline, error) ||
            !device.bind_graphics_bind_group(commands, 0U, renderer.frameGroup, error) ||
            !device.bind_graphics_bind_group(commands, 2U, lighting.bindGroup, error)) return fail();
        for (const auto& item : prepared) {
            if (item.dashr) continue;
            if (!device.bind_graphics_bind_group(commands, 1U, item.objectGroup, error) ||
                !device.bind_graphics_bind_group(commands, 3U, item.mainMaterialGroup, error) ||
                !bind_mesh(device, commands, *item.draw, error) ||
                !device.draw_indexed(commands, item.submesh->indexCount,
                                     item.draw->instanceCount, item.submesh->firstIndex,
                                     0, 0U, error)) return fail();
            ++stats.materialDraws;
            ++stats.mainMaterialDescriptorsBound;
            ++stats.gpuMaterialRecordsBound;
            ++stats.gpuMappingRecordsBound;
            stats.materialTriangles += static_cast<std::uint64_t>(item.submesh->indexCount / 3U) *
                                       item.draw->instanceCount;
        }
    }
    if (!device.end_render_pass(commands, error)) return fail();
    const auto submitted = device.submit(commands, error);
    if (!submitted) return fail();
    renderer.retiredGroups.push_back({submitted,std::move(transientGroups)});
    if (!dashrShadowGroups.empty()) {
        frame.dashrRenderer->retiredBindGroups.push_back(
            {submitted,std::move(dashrShadowGroups)});
    }

    std::vector<DashrShellDraw> dashrDraws;
    for (const auto& item : prepared) {
        if (!item.dashr || !frame.drawMaterials) continue;
        Matrix4 viewProjection{};
        std::memcpy(viewProjection.values.data(), frame.frameConstants.data(),
                    sizeof(viewProjection.values));
        Matrix4 objectToWorld{};
        objectToWorld.values = item.draw->objectToWorld;
        const auto objectToClip = multiply(viewProjection, objectToWorld);
        const auto& transform = item.draw->asset->materialBindings[
            item.submesh->materialIndex].mapping.baseTransform;
        DashrShellDraw draw;
        draw.shell = &item.draw->dashr->shell();
        draw.atlas = &item.draw->dashr->atlas();
        draw.shellSubmeshIndex = item.submeshIndex;
        draw.objectToClip = objectToClip.values;
        draw.objectToWorld = item.draw->objectToWorld;
        draw.cameraObjectPosition = item.draw->dashrCameraObjectPosition;
        draw.cameraWorldPosition = frame.cameraWorldPosition;
        std::memcpy(draw.environmentParameters.data(), frame.frameConstants.data() + 64U,
                    sizeof(draw.environmentParameters));
        draw.heightUvScale = transform.scale;
        draw.heightUvOffset = transform.offset;
        draw.heightUvRotationRadians = transform.rotationRadians;
        draw.settings = item.draw->dashr->settings();
        draw.usePbr = true;
        draw.material = item.mainDescriptor;
        draw.lighting = &lighting;
        draw.shadows = &shadowAtlas;
        dashrDraws.push_back(draw);
    }
    if (!dashrDraws.empty()) {
        DashrShellFrameDesc dashrFrame{frame.colorTarget, frame.depthTarget,
                                      frame.width, frame.height, dashrDraws};
        DashrShellFrameStats dashrStats;
        rhi::FenceHandle dashrFence;
        if (!record_dashr_shell_frame(device, *frame.dashrRenderer, dashrFrame,
                                      dashrStats, &dashrFence, error)) return false;
        stats.dashrDraws = dashrStats.draws;
        stats.dashrTriangles = dashrStats.shellTriangles;
        if (fence) *fence = dashrFence;
    } else if (fence) {
        *fence = submitted;
    }
    return true;
}

} // namespace dve::render
