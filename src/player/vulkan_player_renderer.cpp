#include "dve/player/vulkan_player_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dve/environment_lighting.hpp"
#include "dve/game_world.hpp"
#include "dve/gpu_render_environment.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/cascaded_shadow_map.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/render/live_environment_renderer.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/render/scene_voxel_grid.hpp"
#include "dve/render/voxel_surface_mesher.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include "dve/transform.hpp"
#include "player_shader_blobs.hpp"

namespace dve::player {
namespace {

using Clock = std::chrono::steady_clock;
using rhi::ResourceState;

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::vector<std::byte> copy_bytes(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

Float3 vsub(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float vdot(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 vcross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 unit(Float3 v, Float3 fallback) noexcept {
    const float length = std::sqrt(vdot(v, v));
    return length > 1.0e-20F ? Float3{v.x / length, v.y / length, v.z / length} : fallback;
}

// Column-major 4x4 (element [column * 4 + row]), the layout HLSL float4x4 reads with mul(M, v).
using Matrix = std::array<float, 16>;

Matrix multiply(const Matrix& a, const Matrix& b) noexcept {
    Matrix result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) sum += a[static_cast<std::size_t>(k * 4 + row)] * b[static_cast<std::size_t>(column * 4 + k)];
            result[static_cast<std::size_t>(column * 4 + row)] = sum;
        }
    return result;
}

// General 4x4 inverse (column-major). A singular matrix returns identity; callers only invert
// view-projection matrices built from a valid camera.
Matrix invert(const Matrix& m) noexcept {
    Matrix inv{};
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float determinant = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::abs(determinant) < 1.0e-20F)
        return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float scale = 1.0F / determinant;
    for (float& value : inv) value *= scale;
    return inv;
}

struct CameraFrame {
    Matrix viewProjection{};
    Float3 position{};
    Float3 forward{0.0F, 0.0F, -1.0F};
};

// Right-handed view looking down -Z; Vulkan clip space: depth 0 (near) .. 1 (far) and NDC +Y
// pointing down, so the projection negates Y (positive-height viewports throughout).
CameraFrame camera_frame(const render::PolygonCamera& camera, float aspect) noexcept {
    CameraFrame frame;
    frame.position = camera.position;
    const Float3 forward = unit(vsub(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    const Float3 right = unit(vcross(forward, camera.up), {1.0F, 0.0F, 0.0F});
    const Float3 up = vcross(right, forward);
    frame.forward = forward;
    const Matrix view{
        right.x, up.x, -forward.x, 0.0F,
        right.y, up.y, -forward.y, 0.0F,
        right.z, up.z, -forward.z, 0.0F,
        -vdot(right, camera.position), -vdot(up, camera.position), vdot(forward, camera.position), 1.0F};
    const float nearPlane = std::max(camera.nearPlane, 1.0e-4F);
    const float farPlane = std::max(camera.farPlane, nearPlane * 1.001F);
    const float f = 1.0F / std::tan(std::clamp(camera.verticalFieldOfViewRadians, 1.0e-3F, 3.1F) * 0.5F);
    Matrix projection{};
    projection[0] = f / std::max(aspect, 1.0e-6F);
    projection[5] = -f;
    projection[10] = farPlane / (nearPlane - farPlane);
    projection[11] = -1.0F;
    projection[14] = nearPlane * farPlane / (nearPlane - farPlane);
    frame.viewProjection = multiply(projection, view);
    return frame;
}

std::array<float, 16> rigid_matrix(const RigidTransform& transform) noexcept {
    const Float3 x = transform_vector(transform, {1.0F, 0.0F, 0.0F});
    const Float3 y = transform_vector(transform, {0.0F, 1.0F, 0.0F});
    const Float3 z = transform_vector(transform, {0.0F, 0.0F, 1.0F});
    return {x.x, x.y, x.z, 0.0F, y.x, y.y, y.z, 0.0F, z.x, z.y, z.z, 0.0F,
            transform.position.x, transform.position.y, transform.position.z, 1.0F};
}

// Sky light from the environment's sky/ground colours: the same hemisphere the CPU renderer
// uses for ambient (ShadeAmbient), as a small cube the IBL baker integrates.
EnvironmentLightingAsset sky_environment_asset(Float3 sky, Float3 ground) {
    constexpr std::uint32_t kResolution = 16U;
    EnvironmentLightingAsset asset;
    asset.name = "Player sky";
    asset.sourceRadiance.resolution = kResolution;
    asset.sourceRadiance.texels.resize(6ULL * kResolution * kResolution);
    for (std::uint32_t face = 0; face < 6U; ++face)
        for (std::uint32_t y = 0; y < kResolution; ++y)
            for (std::uint32_t x = 0; x < kResolution; ++x) {
                const Float3 d = cube_lookup_to_direction(static_cast<CubeFace>(face),
                    {(static_cast<float>(x) + 0.5F) / kResolution, (static_cast<float>(y) + 0.5F) / kResolution});
                const float t = std::clamp(0.5F + 0.5F * d.y, 0.0F, 1.0F);
                asset.sourceRadiance.texels[asset.sourceRadiance.face_offset(static_cast<CubeFace>(face)) +
                                            static_cast<std::size_t>(y) * kResolution + x] = {
                    ground.x + (sky.x - ground.x) * t, ground.y + (sky.y - ground.y) * t,
                    ground.z + (sky.z - ground.z) * t};
            }
    IblBakeSettings settings;
    settings.irradianceResolution = 8U;
    settings.specularResolution = 16U;
    settings.specularMipLevels = 5U;
    settings.sampleCount = 64U;
    settings.brdfResolution = 32U;
    asset.baked = bake_image_based_lighting(asset.sourceRadiance, settings);
    asset.recompute_content_hash();
    return asset;
}

const std::array<VoxelMaterialDefinition, 256>& default_palette() {
    static const std::array<VoxelMaterialDefinition, 256> palette = [] {
        std::array<VoxelMaterialDefinition, 256> result{};
        for (std::size_t id = 0; id < result.size(); ++id) result[id] = default_voxel_material(static_cast<MaterialId>(id));
        return result;
    }();
    return palette;
}

class VulkanPlayerRenderer final : public IPlayerRenderer {
public:
    explicit VulkanPlayerRenderer(VulkanPlayerRendererOptions options) : options_(std::move(options)) {}

    ~VulkanPlayerRenderer() override {
        if (!device_) return;
        device_->wait_idle();
        std::string ignored;
        voxelMeshes_.clear();
        polygonMeshes_.clear();
        destroy_targets();
        if (presentPipelineSwapchain_) (void)device_->destroy_graphics_pipeline(presentPipelineSwapchain_, &ignored);
        if (presentPipelineOffscreen_) (void)device_->destroy_graphics_pipeline(presentPipelineOffscreen_, &ignored);
        if (presentLayout_) (void)device_->destroy_bind_group_layout(presentLayout_, &ignored);
        if (presentSampler_) (void)device_->destroy_sampler(presentSampler_, &ignored);
        if (depthSampler_) (void)device_->destroy_sampler(depthSampler_, &ignored);
        if (giPipeline_) (void)device_->destroy_graphics_pipeline(giPipeline_, &ignored);
        if (gridPalette_) (void)device_->destroy_buffer(gridPalette_, &ignored);
        if (tracedAoPipeline_) (void)device_->destroy_graphics_pipeline(tracedAoPipeline_, &ignored);
        if (aoPipeline_) (void)device_->destroy_graphics_pipeline(aoPipeline_, &ignored);
        if (aoLayout_) (void)device_->destroy_bind_group_layout(aoLayout_, &ignored);
        if (gridBuffer_) (void)device_->destroy_buffer(gridBuffer_, &ignored);
        if (aoConstants_) (void)device_->destroy_buffer(aoConstants_, &ignored);
        if (presentConstants_) (void)device_->destroy_buffer(presentConstants_, &ignored);
        if (triangleIndices_) (void)device_->destroy_buffer(triangleIndices_, &ignored);
        if (live_.valid()) (void)render::destroy_live_environment_renderer(*device_, live_, &ignored);
        if (lighting_.valid()) (void)render::destroy_environment_lighting(*device_, lighting_, &ignored);
        if (atlas_.valid()) (void)render::destroy_cascaded_shadow_atlas(*device_, atlas_, &ignored);
        if (swapchain_) (void)device_->destroy_swapchain(swapchain_, &ignored);
    }

    bool initialize(std::string* error) {
        if (!shader_blobs::available()) {
            set_error(error, "this build has no compiled Vulkan player shaders (DXC with SPIR-V output was not found at configure time)");
            return false;
        }
        device_ = std::make_unique<rhi::VulkanDevice>();
        if (device_->status() != rhi::DeviceStatus::Ready) {
            set_error(error, "Vulkan device unavailable: " + std::string(device_->device_loss_reason()));
            return false;
        }
        if (options_.surface.kind != platform::NativeSurfaceKind::Unavailable) {
            rhi::SwapchainDesc desc;
            desc.surface = options_.surface;
            desc.width = std::max(options_.drawableWidth, 1U);
            desc.height = std::max(options_.drawableHeight, 1U);
            desc.imageCount = 3U;
            desc.presentMode = options_.presentMode;
            desc.debugName = "dve_player swapchain";
            swapchain_ = device_->create_swapchain(desc, error);
            if (!swapchain_) return false;
            swapchainFormat_ = device_->swapchain_format(swapchain_);
        }

        rhi::BufferDesc indices{3U * sizeof(std::uint32_t), rhi::BufferUsage::Index, rhi::MemoryDomain::Upload,
                                "player fullscreen triangle", ResourceState::ShaderRead};
        triangleIndices_ = device_->create_buffer(indices, error);
        if (!triangleIndices_) return false;
        const std::array<std::uint32_t, 3> triangle{0U, 1U, 2U};
        if (!device_->write_buffer(triangleIndices_, 0U, std::as_bytes(std::span(triangle)), error)) return false;
        presentConstants_ = device_->create_buffer({256U, rhi::BufferUsage::Constant, rhi::MemoryDomain::Upload,
                                                    "player present constants", ResourceState::ShaderRead}, error);
        if (!presentConstants_) return false;
        rhi::SamplerDesc sampler;
        sampler.addressU = sampler.addressV = sampler.addressW = rhi::AddressMode::ClampToEdge;
        sampler.mipmapFilter = rhi::MipmapFilterMode::Nearest;
        sampler.debugName = "player present sampler";
        presentSampler_ = device_->create_sampler(sampler, error);
        if (!presentSampler_) return false;
        sampler.minFilter = sampler.magFilter = rhi::FilterMode::Nearest;
        sampler.debugName = "player depth sampler";
        depthSampler_ = device_->create_sampler(sampler, error);
        if (!depthSampler_) return false;
        rhi::BindGroupLayoutDesc layout;
        layout.debugName = "player present layout";
        layout.bindings = {{0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
                           {1U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Fragment},
                           {2U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment}};
        presentLayout_ = device_->create_bind_group_layout(layout, error);
        if (!presentLayout_) return false;
        aoConstants_ = device_->create_buffer({512U, rhi::BufferUsage::Constant, rhi::MemoryDomain::Upload,
                                               "player SSAO constants", ResourceState::ShaderRead}, error);
        if (!aoConstants_) return false;
        // Scene voxel grid for traced AO; a 16-byte placeholder keeps the layout valid otherwise.
        gridPalette_ = device_->create_buffer(
            {256U * sizeof(render::SceneGridMaterial), rhi::BufferUsage::Storage, rhi::MemoryDomain::Upload,
             "player scene voxel palette", ResourceState::ShaderRead}, error);
        if (!gridPalette_) return false;
        if (!ensure_grid_buffer(16U, error)) return false;
        rhi::BindGroupLayoutDesc aoLayout;
        aoLayout.debugName = "player SSAO layout";
        aoLayout.bindings = {{0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
                             {1U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Fragment},
                             {2U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Fragment},
                             {3U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Fragment}};
        aoLayout_ = device_->create_bind_group_layout(aoLayout, error);
        if (!aoLayout_) return false;
        {
            rhi::GraphicsPipelineDesc desc;
            desc.debugName = "player SSAO";
            desc.vertexBytecode = copy_bytes(shader_blobs::player_present_vs());
            desc.fragmentBytecode = copy_bytes(shader_blobs::player_ssao_ps());
            desc.bindGroupLayouts = {aoLayout_};
            desc.colorFormat = rhi::TextureFormat::RGBA8Unorm;
            desc.depthFormat = std::nullopt;
            desc.cullMode = rhi::CullMode::Disabled;
            desc.depthTest = false;
            desc.depthWrite = false;
            aoPipeline_ = device_->create_graphics_pipeline(desc, error);
            if (!aoPipeline_) return false;
            desc.debugName = "player traced AO";
            desc.fragmentBytecode = copy_bytes(shader_blobs::player_traced_ao_ps());
            tracedAoPipeline_ = device_->create_graphics_pipeline(desc, error);
            if (!tracedAoPipeline_) return false;
            desc.debugName = "player traced GI";
            desc.fragmentBytecode = copy_bytes(shader_blobs::player_traced_gi_ps());
            desc.colorFormat = rhi::TextureFormat::RGBA16Float;
            giPipeline_ = device_->create_graphics_pipeline(desc, error);
            if (!giPipeline_) return false;
        }
        const auto present_pipeline = [&](rhi::TextureFormat format, const char* name) {
            rhi::GraphicsPipelineDesc desc;
            desc.debugName = name;
            desc.vertexBytecode = copy_bytes(shader_blobs::player_present_vs());
            desc.fragmentBytecode = copy_bytes(shader_blobs::player_present_ps());
            desc.bindGroupLayouts = {presentLayout_};
            desc.colorFormat = format;
            desc.depthFormat = std::nullopt;
            desc.cullMode = rhi::CullMode::Disabled;
            desc.depthTest = false;
            desc.depthWrite = false;
            return device_->create_graphics_pipeline(desc, error);
        };
        presentPipelineOffscreen_ = present_pipeline(rhi::TextureFormat::RGBA8Unorm, "player present (offscreen)");
        if (!presentPipelineOffscreen_) return false;
        if (swapchain_) {
            presentPipelineSwapchain_ = present_pipeline(swapchainFormat_, "player present (swapchain)");
            if (!presentPipelineSwapchain_) return false;
        }
        return true;
    }

    [[nodiscard]] std::string_view name() const noexcept override { return "vulkan"; }

    bool resize(std::uint32_t width, std::uint32_t height, std::string* error) override {
        if (width == 0U || height == 0U || width > 8192U || height > 8192U) {
            set_error(error, "Vulkan render size must be between 1x1 and 8192x8192");
            return false;
        }
        if (width == width_ && height == height_ && hdr_) return true;
        device_->wait_idle();
        destroy_targets();
        width_ = width;
        height_ = height;
        rhi::TextureDesc color;
        color.format = rhi::TextureFormat::RGBA16Float;
        color.width = width; color.height = height;
        color.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        color.initialState = ResourceState::RenderTarget;
        color.debugName = "player HDR scene colour";
        hdr_ = device_->create_texture(color, error);
        if (!hdr_) return false;
        hdrView_ = device_->create_texture_view({hdr_, 0U, 1U, 0U, 1U, "player HDR view"}, error);
        if (!hdrView_) return false;
        rhi::TextureDesc depth;
        depth.format = rhi::TextureFormat::D32Float;
        depth.width = width; depth.height = height;
        depth.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        depth.initialState = ResourceState::DepthWrite;
        depth.debugName = "player depth";
        depth_ = device_->create_texture(depth, error);
        if (!depth_) return false;
        depthView_ = device_->create_texture_view({depth_, 0U, 1U, 0U, 1U, "player depth view"}, error);
        if (!depthView_) return false;
        rhi::TextureDesc ldr;
        ldr.format = rhi::TextureFormat::RGBA8Unorm;
        ldr.width = width; ldr.height = height;
        ldr.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
        ldr.initialState = ResourceState::RenderTarget;
        ldr.debugName = "player capture";
        capture_ = device_->create_texture(ldr, error);
        if (!capture_) return false;
        rhi::BindGroupDesc group;
        group.layout = presentLayout_;
        group.debugName = "player present group";
        group.entries = {{0U, {}, hdrView_, 0U, 0U, presentSampler_}, {1U, presentConstants_, {}, 0U, 256U, {}},
                         {2U, {}, depthView_, 0U, 0U, depthSampler_}};
        presentGroup_ = device_->create_bind_group(group, error);
        if (!presentGroup_) return false;
        rhi::TextureDesc ao;
        ao.format = rhi::TextureFormat::RGBA8Unorm;
        ao.width = width; ao.height = height;
        ao.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        ao.initialState = ResourceState::ShaderRead;   // resting state: sampled by the main pass
        ao.debugName = "player ambient occlusion";
        aoTexture_ = device_->create_texture(ao, error);
        if (!aoTexture_) return false;
        aoView_ = device_->create_texture_view({aoTexture_, 0U, 1U, 0U, 1U, "player AO view"}, error);
        if (!aoView_) return false;
        rhi::TextureDesc gi = ao;
        gi.format = rhi::TextureFormat::RGBA16Float;
        gi.debugName = "player indirect light";
        giTexture_ = device_->create_texture(gi, error);
        if (!giTexture_) return false;
        giView_ = device_->create_texture_view({giTexture_, 0U, 1U, 0U, 1U, "player indirect light view"}, error);
        if (!giView_) return false;
        if (!rebuild_ao_group(error)) return false;
        frame_.width = width; frame_.height = height;
        frame_.pixels.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
        rendered_ = false;
        return true;
    }

    void window_resized(std::uint32_t drawableWidth, std::uint32_t drawableHeight) override {
        options_.drawableWidth = drawableWidth;
        options_.drawableHeight = drawableHeight;
        swapchainStale_ = true;
    }

    bool render(const PlayerRenderView& view, std::string* error) override {
        if (!hdr_) { set_error(error, "renderer has no size; call resize() first"); return false; }
        const auto start = Clock::now();
        stats_ = {};
        ++frameIndex_;
        const RenderEnvironment& environment = view.environment;
        if (!environment.validate(error)) return false;
        if (!ensure_environment(environment, error)) return false;
        // Same display transform as the CPU resolve (resolve_polygon_render_rgba8).
        exposure_ = options_.exposure * (environment.exposure > 0.0F ? environment.exposure : 1.0F);
        tonemapOperator_ = static_cast<std::uint32_t>(environment.tonemapOperator);
        skyColor_ = environment.skyColor;

        const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
        const CameraFrame camera = camera_frame(view.camera, aspect);

        // Geometry: voxel objects -> cached greedy meshes, polygons -> cached uploads.
        draws_.clear();
        for (const GameRenderObject& object : view.objects) {
            if (!object.enabled) continue;
            if (object.kind == GameGeometryKind::Voxel && object.voxels != nullptr) {
                const float voxelSize = object.voxelSizeMeters > 0.0F ? object.voxelSizeMeters : kDefaultMetersPerVoxel;
                const Float3 cell{object.voxelScale.x * voxelSize, object.voxelScale.y * voxelSize,
                                  object.voxelScale.z * voxelSize};
                auto* mesh = voxel_mesh(object, cell, error);
                if (mesh == nullptr) return false;
                ++stats_.voxelInstances;
                if (mesh->asset.vertices.empty()) continue;
                render::LivePolygonDraw draw{mesh->mirror.get(), &mesh->asset, 1U};
                draw.objectToWorld = rigid_matrix(object.transform);
                draw.castsShadow = true;
                draws_.push_back(draw);
            } else if (object.kind == GameGeometryKind::Polygon && object.polygon != nullptr) {
                auto* mesh = polygon_mesh(*object.polygon, error);
                if (mesh == nullptr) return false;
                ++stats_.polygonInstances;
                render::LivePolygonDraw draw{mesh->mirror.get(), object.polygon, 1U};
                draw.objectToWorld = rigid_matrix(object.transform);
                draw.castsShadow = true;
                draws_.push_back(draw);
            } else if (object.kind == GameGeometryKind::Voxel || object.kind == GameGeometryKind::Polygon) {
                ++stats_.skippedObjects;
            }
        }
        prune_caches();

        // Sun shadows follow the camera; atlas size is fixed by the settings.
        render::CascadedShadowPlan plan;
        const bool sunOn = environment.sunIntensity > 0.0F;
        const bool shadowsOn = sunOn && options_.shadowCascades > 0U && environment.shadowMode != ShadowMode::Off &&
                               environment.shadowStrength > 0.0F;
        camera::CameraPose pose;
        pose.position = view.camera.position;
        pose.target = view.camera.target;
        pose.worldUp = view.camera.up;
        pose.lens.verticalFieldOfViewRadians = view.camera.verticalFieldOfViewRadians;
        pose.lens.nearPlaneMeters = std::max(view.camera.nearPlane, 1.0e-3F);
        pose.lens.farPlaneMeters = std::max(view.camera.farPlane, pose.lens.nearPlaneMeters * 2.0F);
        pose.lens.aspectRatio = aspect;
        render::CascadedShadowSettings shadowSettings;
        shadowSettings.cascadeCount = std::clamp(options_.shadowCascades, 1U, 4U);
        shadowSettings.cascadeResolution = options_.shadowCascadeResolution;
        shadowSettings.maximumDistanceMeters = std::min(options_.shadowDistanceMeters, pose.lens.farPlaneMeters);
        plan = render::make_cascaded_shadow_plan(pose, environment.sunDirection, shadowSettings);
        if (!atlas_.valid() && !render::create_cascaded_shadow_atlas(*device_, plan, atlas_, error))
            return stage_failed("shadow atlas creation", error);
        if (!live_.valid() && !create_live(error)) return stage_failed("live renderer creation", error);

        std::array<std::byte, render::kLiveFrameConstantBytes> constants{};
        std::array<float, 24> head{};
        std::copy(camera.viewProjection.begin(), camera.viewProjection.end(), head.begin());
        head[16] = 0.0F;                                         // environment rotation
        // Ambient (image-based) light strength: the CPU scales its sky/ground ambient by
        // globalIlluminationIntensity, and has none with global illumination off.
        head[17] = environment.globalIlluminationMode == GlobalIlluminationMode::Off
                       ? 0.0F : environment.globalIlluminationIntensity;
        head[18] = 0.0F;                                         // exposure EV (applied at present)
        head[19] = static_cast<float>(std::max(lighting_.specularMipLevels, 1U) - 1U);
        head[20] = camera.position.x; head[21] = camera.position.y; head[22] = camera.position.z; head[23] = 1.0F;
        std::memcpy(constants.data(), head.data(), sizeof(head));
        const render::GpuLiveSunConstants sun = render::make_live_sun_constants(
            shadowsOn ? &plan : nullptr, environment.sunDirection, environment.sunColor,
            sunOn ? environment.sunIntensity * std::numbers::pi_v<float> : 0.0F,
            environment.shadowStrength, camera.forward);
        std::memcpy(constants.data() + render::kLiveSunConstantsOffset, &sun, sizeof(sun));

        // Screen-space ambient occlusion: depth pre-pass -> AO image the main pass samples.
        const bool aoOn = options_.ambientOcclusion && environment.ambientOcclusionSamples > 0U &&
                          environment.ambientOcclusionMaxDistanceMeters > 0.0F &&
                          environment.globalIlluminationMode != GlobalIlluminationMode::Off && !draws_.empty();
        const bool aoTraced = aoOn && options_.ambientOcclusionMode == PlayerAmbientOcclusionMode::VoxelTraced &&
                              shader_blobs::player_traced_ao_ps().size() > 0U;
        // Traced indirect light (Colours sky only: the traced sky is the sky/ground blend).
        const bool giTraced = options_.globalIllumination != PlayerGlobalIlluminationMode::Ambient &&
                              environment.globalIlluminationMode != GlobalIlluminationMode::Off &&
                              environment.globalIlluminationIntensity > 0.0F &&
                              environment.skyMode == SkyMode::Colors && !draws_.empty() &&
                              shader_blobs::player_traced_gi_ps().size() > 0U;
        if ((aoTraced || giTraced) && !update_scene_grid(view, view.camera.position, error))
            return stage_failed("scene voxel grid", error);
        if ((aoOn || giTraced) &&
            !record_screen_passes(environment, camera, constants, plan, aoOn, aoTraced, giTraced, error))
            return stage_failed("ambient occlusion / indirect light", error);

        render::LiveEnvironmentFrameDesc frame;
        frame.ambientOcclusion = aoOn ? aoView_ : rhi::TextureViewHandle{};
        frame.indirectLight = giTraced ? giView_ : rhi::TextureViewHandle{};
        frame.colorTarget = hdr_;
        frame.depthTarget = depth_;
        frame.width = width_;
        frame.height = height_;
        frame.frameConstants = constants;
        frame.polygonDraws = draws_;
        frame.cameraWorldPosition = camera.position;
        // The background is drawn by the present pass (CPU-matching sky, see player_present_ps).
        frame.drawSkybox = false;
        frame.drawMaterials = true;
        frame.drawShadowCasters = shadowsOn;
        frame.refreshStaticShadowCasters = true;
        render::LiveEnvironmentFrameStats liveStats;
        rhi::FenceHandle fence;
        if (!render::record_live_environment_frame(*device_, lighting_, plan, atlas_, live_, frame, liveStats,
                                                   &fence, error)) return stage_failed("live frame recording", error);
        stats_.polygons.submittedInstances = draws_.size();
        stats_.polygons.submittedTriangles = liveStats.materialTriangles;
        stats_.renderMilliseconds = milliseconds_since(start);
        rendered_ = true;
        return true;
    }

    bool ensure_grid_buffer(std::size_t bytes, std::string* error) {
        bytes = std::max<std::size_t>((bytes + 15U) & ~std::size_t{15U}, 16U);
        if (gridBuffer_ && bytes <= gridCapacity_) return true;
        const std::size_t capacity = std::max(bytes, gridCapacity_ * 2U);
        const rhi::BufferHandle replacement = device_->create_buffer(
            {capacity, rhi::BufferUsage::Storage, rhi::MemoryDomain::Upload, "player scene voxel grid",
             ResourceState::ShaderRead}, error);
        if (!replacement) return false;
        std::string ignored;
        if (gridBuffer_) {
            device_->wait_idle();
            if (aoGroup_) { (void)device_->destroy_bind_group(aoGroup_, &ignored); aoGroup_ = {}; }
            (void)device_->destroy_buffer(gridBuffer_, &ignored);
        }
        gridBuffer_ = replacement;
        gridCapacity_ = capacity;
        gridUploaded_ = false;
        return !aoLayout_ || !depthView_ || !gridPalette_ || rebuild_ao_group(error);
    }

    bool rebuild_ao_group(std::string* error) {
        std::string ignored;
        if (aoGroup_) { (void)device_->destroy_bind_group(aoGroup_, &ignored); aoGroup_ = {}; }
        rhi::BindGroupDesc group;
        group.layout = aoLayout_;
        group.debugName = "player AO group";
        group.entries = {{0U, {}, depthView_, 0U, 0U, depthSampler_}, {1U, aoConstants_, {}, 0U, 512U, {}},
                         {2U, gridBuffer_, {}, 0U, gridCapacity_, {}},
                         {3U, gridPalette_, {}, 0U, 256U * sizeof(render::SceneGridMaterial), {}}};
        aoGroup_ = device_->create_bind_group(group, error);
        return static_cast<bool>(aoGroup_);
    }

    // Builds the scene voxel grid from this frame's voxel objects and uploads it when it changed.
    bool update_scene_grid(const PlayerRenderView& view, Float3 focus, std::string* error) {
        gridObjects_.clear();
        for (const GameRenderObject& object : view.objects) {
            if (!object.enabled || object.kind != GameGeometryKind::Voxel || object.voxels == nullptr) continue;
            const float voxelSize = object.voxelSizeMeters > 0.0F ? object.voxelSizeMeters : kDefaultMetersPerVoxel;
            render::SceneGridObject entry;
            entry.id = object.id;
            entry.voxels = object.voxels;
            entry.transform = object.transform;
            entry.cellMeters = {object.voxelScale.x * voxelSize, object.voxelScale.y * voxelSize,
                                object.voxelScale.z * voxelSize};
            entry.materials = object.materials;
            gridObjects_.push_back(entry);
        }
        render::SceneVoxelGridSettings settings;
        settings.cellMeters = options_.tracedAoCellMeters;
        settings.maximumDimension = options_.tracedAoMaximumDimension;
        const bool changed = grid_.update(gridObjects_, settings, focus);
        if (grid_.empty()) return true;
        if (!changed && gridUploaded_) return true;
        const auto cells = grid_.cells();
        if (!ensure_grid_buffer(cells.size(), error)) return false;
        std::vector<std::uint8_t> padded(cells.begin(), cells.end());
        padded.resize((padded.size() + 3U) & ~std::size_t{3U});
        if (!device_->write_buffer(gridBuffer_, 0U, std::as_bytes(std::span(padded)), error)) return false;
        const auto palette = grid_.palette();
        if (!palette.empty() &&
            !device_->write_buffer(gridPalette_, 0U, std::as_bytes(palette.first(std::min<std::size_t>(palette.size(), 255U))), error))
            return false;
        gridUploaded_ = true;
        return true;
    }

    // Depth pre-pass, then the screen-sized passes that read it: ambient occlusion (aoTexture_)
    // and traced indirect light (giTexture_).
    bool record_screen_passes(const RenderEnvironment& environment, const CameraFrame& camera,
                              std::span<const std::byte> frameConstants,
                              const render::CascadedShadowPlan& plan, bool aoOn, bool aoTraced, bool giTraced,
                              std::string* error) {
        render::LiveEnvironmentFrameDesc prepass;
        prepass.depthTarget = depth_;
        prepass.width = width_;
        prepass.height = height_;
        prepass.frameConstants = frameConstants;
        prepass.polygonDraws = draws_;
        prepass.cameraWorldPosition = camera.position;
        prepass.drawSkybox = false;
        prepass.drawMaterials = false;
        prepass.drawShadowCasters = false;
        prepass.depthPrepassOnly = true;
        render::LiveEnvironmentFrameStats prepassStats;
        if (!render::record_live_environment_frame(*device_, lighting_, plan, atlas_, live_, prepass, prepassStats,
                                                   nullptr, error)) return false;
        struct alignas(16) SsaoConstants {
            Matrix viewProjection;
            Matrix inverseViewProjection;
            std::array<float, 4> cameraPosition;
            std::array<float, 4> params;
            std::array<float, 4> target;
            std::array<float, 4> gridOrigin;   // xyz minimum corner, w cell size (0: none)
            std::array<float, 4> gridSize;     // xyz cells per axis
            std::array<float, 4> sunDirection; // xyz toward the sun, w intensity
            std::array<float, 4> sunColor;     // rgb, w sun-visibility ray length m
            std::array<float, 4> skyColor;     // rgb, w indirect ray length m
            std::array<float, 4> groundColor;  // rgb, w indirect sample count
        } constants{};
        constants.viewProjection = camera.viewProjection;
        constants.inverseViewProjection = invert(camera.viewProjection);
        constants.cameraPosition = {camera.position.x, camera.position.y, camera.position.z, 1.0F};
        const std::uint32_t seed = 1U;
        float seedBits{};
        std::memcpy(&seedBits, &seed, sizeof(seedBits));
        constants.params = {static_cast<float>(std::min(environment.ambientOcclusionSamples, 64U)),
                            environment.ambientOcclusionMaxDistanceMeters, std::max(environment.shadowBiasMeters, 1.0e-3F),
                            seedBits};
        constants.target = {static_cast<float>(width_), static_cast<float>(height_), 1.0F / static_cast<float>(width_),
                            1.0F / static_cast<float>(height_)};
        {
            const Float3 sun = unit(environment.sunDirection, {0.0F, 1.0F, 0.0F});
            constants.sunDirection = {sun.x, sun.y, sun.z, environment.sunIntensity};
            constants.sunColor = {environment.sunColor.x, environment.sunColor.y, environment.sunColor.z,
                                  std::max(environment.shadowMaxDistanceMeters, 0.1F)};
            constants.skyColor = {environment.skyColor.x, environment.skyColor.y, environment.skyColor.z,
                                  environment.globalIlluminationMaxDistanceMeters};
            constants.groundColor = {environment.groundColor.x, environment.groundColor.y, environment.groundColor.z,
                                     static_cast<float>(std::clamp(environment.globalIlluminationSamples, 1U, 16U))};
            if (!aoOn) constants.params[2] = std::max(environment.shadowBiasMeters, 1.0e-3F);
        }
        if ((aoTraced || giTraced) && !grid_.empty()) {
            const Float3 origin = grid_.origin();
            const auto dims = grid_.dimensions();
            constants.gridOrigin = {origin.x, origin.y, origin.z, grid_.cell_meters()};
            constants.gridSize = {static_cast<float>(dims[0]), static_cast<float>(dims[1]), static_cast<float>(dims[2]), 0.0F};
        }
        if (!device_->write_buffer(aoConstants_, 0U, std::as_bytes(std::span(&constants, 1U)), error)) return false;
        const rhi::CommandListHandle commands = device_->begin_commands(rhi::QueueKind::Graphics, "player ambient occlusion", error);
        if (!commands) return false;
        if (!device_->transition_texture(commands, depth_, ResourceState::DepthWrite, ResourceState::ShaderRead, error))
            return false;
        const auto fullscreen_pass = [&](rhi::TextureHandle target, rhi::GraphicsPipelineHandle pipeline,
                                         const char* name, float clear) {
            if (!device_->transition_texture(commands, target, ResourceState::ShaderRead, ResourceState::RenderTarget, error))
                return false;
            rhi::RenderPassDesc pass;
            pass.debugName = name;
            pass.colors.push_back({target, true, clear, clear, clear, clear});
            if (!device_->begin_render_pass(commands, pass, error) ||
                !device_->bind_graphics_pipeline(commands, pipeline, error) ||
                !device_->bind_graphics_bind_group(commands, 0U, aoGroup_, error) ||
                !device_->set_viewport(commands, {0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F}, error) ||
                !device_->set_scissor(commands, {0, 0, width_, height_}, error) ||
                !device_->bind_index_buffer(commands, triangleIndices_, 0U, rhi::IndexFormat::Uint32, error) ||
                !device_->draw_indexed(commands, 3U, 1U, 0U, 0, 0U, error) ||
                !device_->end_render_pass(commands, error)) return false;
            return device_->transition_texture(commands, target, ResourceState::RenderTarget, ResourceState::ShaderRead, error);
        };
        if (aoOn && !fullscreen_pass(aoTexture_, aoTraced ? tracedAoPipeline_ : aoPipeline_, "player ambient occlusion", 1.0F))
            return false;
        if (giTraced && !fullscreen_pass(giTexture_, giPipeline_, "player traced indirect light", 0.0F)) return false;
        if (!device_->transition_texture(commands, depth_, ResourceState::ShaderRead, ResourceState::DepthWrite, error))
            return false;
        return static_cast<bool>(device_->submit(commands, error));
    }

    bool present(std::string* error) override {
        if (!rendered_) { set_error(error, "present() before render()"); return false; }
        const auto start = Clock::now();
        const bool capture = !swapchain_ || options_.captureFrames;
        if (capture && !tonemap_into(capture_, ResourceState::RenderTarget, presentPipelineOffscreen_,
                                     width_, height_, true, error)) return false;
        stats_.resolveMilliseconds = milliseconds_since(start);
        if (swapchain_) {
            const auto presentStart = Clock::now();
            if (!present_swapchain(error)) return false;
            stats_.presentMilliseconds = milliseconds_since(presentStart);
        }
        return true;
    }

    [[nodiscard]] const Rgba8Image* readback() const noexcept override {
        return rendered_ && captured_ ? &frame_ : nullptr;
    }
    [[nodiscard]] PlayerRenderStats last_stats() const noexcept override { return stats_; }

private:
    struct VoxelMesh {
        const VoxelObject* object{};
        std::uint64_t revision{};
        Float3 cell{};
        const VoxelMaterialDefinition* materials{};
        std::size_t materialCount{};
        CookedPolygonAsset asset;
        std::unique_ptr<render::MeshRhiMirror> mirror;
        std::uint64_t lastFrame{};
    };
    struct PolygonMesh {
        std::uint64_t contentHash{};
        std::unique_ptr<render::MeshRhiMirror> mirror;
        std::uint64_t lastFrame{};
    };

    static bool stage_failed(const char* stage, std::string* error) {
        if (error && error->empty()) *error = std::string("Vulkan player renderer: ") + stage + " failed";
        return false;
    }

    bool create_live(std::string* error) {
        render::LiveEnvironmentShaderBytecode bytecode;
        bytecode.materialVertex = copy_bytes(shader_blobs::live_ibl_material_vs());
        bytecode.materialFragment = copy_bytes(shader_blobs::live_ibl_material_ps());
        bytecode.skyboxVertex = copy_bytes(shader_blobs::live_environment_skybox_vs());
        bytecode.skyboxFragment = copy_bytes(shader_blobs::live_environment_skybox_ps());
        bytecode.shadowVertex = copy_bytes(shader_blobs::live_csm_caster_vs());
        bytecode.shadowFragment = copy_bytes(shader_blobs::live_csm_caster_ps());
        return render::create_live_environment_renderer(*device_, lighting_, atlas_, bytecode,
            render::kLiveFrameConstantBytes, rhi::TextureFormat::RGBA16Float, live_, error, {},
            device_->capabilities().debugLabels);
    }

    // The sky light is baked from the environment's sky and ground colours; the live renderer's
    // bind groups reference the IBL textures, so a new sky rebuilds both.
    bool ensure_environment(const RenderEnvironment& environment, std::string* error) {
        const std::array<float, 6> key{environment.skyColor.x, environment.skyColor.y, environment.skyColor.z,
                                       environment.groundColor.x, environment.groundColor.y, environment.groundColor.z};
        if (lighting_.valid() && key == skyKey_) return true;
        device_->wait_idle();
        if (live_.valid() && !render::destroy_live_environment_renderer(*device_, live_, error)) return false;
        if (lighting_.valid() && !render::destroy_environment_lighting(*device_, lighting_, error)) return false;
        // Cached meshes keep their GPU buffers; only material descriptor tables lived in live_,
        // and the new live renderer rebuilds those on first use.
        const EnvironmentLightingAsset asset = sky_environment_asset(environment.skyColor, environment.groundColor);
        if (!render::upload_environment_lighting(*device_, asset, lighting_, error)) return false;
        skyKey_ = key;
        return true;
    }

    VoxelMesh* voxel_mesh(const GameRenderObject& object, Float3 cell, std::string* error) {
        std::span<const VoxelMaterialDefinition> materials = object.materials;
        if (materials.empty()) materials = default_palette();
        auto& mesh = voxelMeshes_[object.id];
        mesh.lastFrame = frameIndex_;
        const bool stale = !mesh.mirror || mesh.object != object.voxels || mesh.revision != object.voxels->revision() ||
                           mesh.cell.x != cell.x || mesh.cell.y != cell.y || mesh.cell.z != cell.z ||
                           mesh.materials != materials.data() || mesh.materialCount != materials.size();
        if (!stale) return &mesh;
        const std::uint64_t previousHash = mesh.asset.contentHash;
        render::VoxelSurfaceMeshOptions options;
        options.cellSizeMeters = cell;
        options.objectId = object.id;
        if (!render::build_voxel_surface_mesh(*object.voxels, materials, options, mesh.asset, nullptr, error))
            return nullptr;
        if (previousHash != 0U && previousHash != mesh.asset.contentHash) forget_asset(previousHash);
        if (!mesh.mirror) mesh.mirror = std::make_unique<render::MeshRhiMirror>(*device_);
        if (!mesh.asset.vertices.empty() && !mesh.mirror->upload(mesh.asset, error)) return nullptr;
        mesh.object = object.voxels;
        mesh.revision = object.voxels->revision();
        mesh.cell = cell;
        mesh.materials = materials.data();
        mesh.materialCount = materials.size();
        return &mesh;
    }

    PolygonMesh* polygon_mesh(const CookedPolygonAsset& asset, std::string* error) {
        auto& mesh = polygonMeshes_[&asset];
        mesh.lastFrame = frameIndex_;
        if (mesh.mirror && mesh.contentHash == asset.contentHash) return &mesh;
        if (mesh.contentHash != 0U) forget_asset(mesh.contentHash);
        if (!mesh.mirror) mesh.mirror = std::make_unique<render::MeshRhiMirror>(*device_);
        if (!mesh.mirror->upload(asset, error)) return nullptr;
        mesh.contentHash = asset.contentHash;
        return &mesh;
    }

    // Material descriptor tables are keyed by asset content hash: drop entries for assets that
    // no longer exist (re-meshed voxels, despawned objects) so they do not accumulate.
    void forget_asset(std::uint64_t contentHash) {
        if (!live_.valid()) return;
        std::string ignored;
        if (live_.mainMaterials) (void)live_.mainMaterials->invalidate_asset(contentHash, &ignored);
        if (live_.shadowMaterials) (void)live_.shadowMaterials->invalidate_asset(contentHash, &ignored);
    }

    void prune_caches() {
        bool idle = false;
        for (auto it = voxelMeshes_.begin(); it != voxelMeshes_.end();) {
            if (it->second.lastFrame == frameIndex_) { ++it; continue; }
            if (!idle) { device_->wait_idle(); idle = true; }
            forget_asset(it->second.asset.contentHash);
            it = voxelMeshes_.erase(it);
        }
        for (auto it = polygonMeshes_.begin(); it != polygonMeshes_.end();) {
            if (it->second.lastFrame == frameIndex_) { ++it; continue; }
            if (!idle) { device_->wait_idle(); idle = true; }
            forget_asset(it->second.contentHash);
            it = polygonMeshes_.erase(it);
        }
    }

    // HDR scene -> exposure/tonemap/sRGB into `target` (state `targetState`, left there).
    bool tonemap_into(rhi::TextureHandle target, ResourceState targetState, rhi::GraphicsPipelineHandle pipeline,
                      std::uint32_t width, std::uint32_t height, bool readBack, std::string* error) {
        struct alignas(16) PresentConstants {
            std::array<float, 4> tintExposure;
            std::array<std::uint32_t, 4> options;
            std::array<float, 4> skyColor;
        } constants{{1.0F, 1.0F, 1.0F, exposure_},
                    {tonemapOperator_, options_.skyGradient ? 2U : 1U, 0U, 0U},
                    {skyColor_.x, skyColor_.y, skyColor_.z, 0.0F}};
        if (!device_->write_buffer(presentConstants_, 0U, std::as_bytes(std::span(&constants, 1U)), error)) return false;
        const rhi::CommandListHandle commands = device_->begin_commands(rhi::QueueKind::Graphics, "player present", error);
        if (!commands) return false;
        if (!device_->transition_texture(commands, hdr_, ResourceState::RenderTarget, ResourceState::ShaderRead, error) ||
            !device_->transition_texture(commands, depth_, ResourceState::DepthWrite, ResourceState::ShaderRead, error))
            return false;
        if (targetState != ResourceState::RenderTarget &&
            !device_->transition_texture(commands, target, targetState, ResourceState::RenderTarget, error)) return false;
        rhi::RenderPassDesc pass;
        pass.debugName = "player tonemap";
        pass.colors.push_back({target, true, 0.0F, 0.0F, 0.0F, 1.0F});
        if (!device_->begin_render_pass(commands, pass, error) ||
            !device_->bind_graphics_pipeline(commands, pipeline, error) ||
            !device_->bind_graphics_bind_group(commands, 0U, presentGroup_, error) ||
            !device_->set_viewport(commands, {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F}, error) ||
            !device_->set_scissor(commands, {0, 0, width, height}, error) ||
            !device_->bind_index_buffer(commands, triangleIndices_, 0U, rhi::IndexFormat::Uint32, error) ||
            !device_->draw_indexed(commands, 3U, 1U, 0U, 0, 0U, error) ||
            !device_->end_render_pass(commands, error)) return false;
        if (!device_->transition_texture(commands, hdr_, ResourceState::ShaderRead, ResourceState::RenderTarget, error) ||
            !device_->transition_texture(commands, depth_, ResourceState::ShaderRead, ResourceState::DepthWrite, error))
            return false;
        const ResourceState after = readBack ? ResourceState::CopySource : targetState;
        if (after != ResourceState::RenderTarget &&
            !device_->transition_texture(commands, target, ResourceState::RenderTarget, after, error)) return false;
        const rhi::FenceHandle fence = device_->submit(commands, error);
        if (!fence) return false;
        lastFence_ = fence;
        if (!readBack) return true;
        if (!device_->read_texture(target, 0U, 0U, std::as_writable_bytes(std::span(frame_.pixels)),
                                   static_cast<std::size_t>(width) * 4U, error)) return false;
        captured_ = true;
        const rhi::CommandListHandle back = device_->begin_commands(rhi::QueueKind::Graphics, "capture reset", error);
        if (!back || !device_->transition_texture(back, target, ResourceState::CopySource, targetState, error))
            return false;
        return static_cast<bool>(device_->submit(back, error));
    }

    bool rebuild_swapchain(std::string* error) {
        if (options_.drawableWidth == 0U || options_.drawableHeight == 0U) return true; // minimized
        if (!device_->resize_swapchain(swapchain_, options_.drawableWidth, options_.drawableHeight, error)) {
            if (error && error->find("zero size") != std::string::npos) { error->clear(); return true; }
            return false;
        }
        swapchainStale_ = false;
        if (device_->swapchain_format(swapchain_) != swapchainFormat_) {
            set_error(error, "swapchain format changed on resize");
            return false;
        }
        return true;
    }

    bool present_swapchain(std::string* error) {
        if (swapchainStale_ && !rebuild_swapchain(error)) return false;
        if (swapchainStale_) return true; // minimized: skip presenting
        rhi::AcquiredSwapchainImage image = device_->acquire_next_image(swapchain_, error);
        if (image.outOfDate) {
            if (!rebuild_swapchain(error)) return false;
            if (swapchainStale_) return true;
            image = device_->acquire_next_image(swapchain_, error);
            if (image.outOfDate) { swapchainStale_ = true; return true; }
        }
        if (!image.texture) return false;
        // Swapchain extents follow the window; the HDR image is the internal render size.
        const auto extent = swapchain_extent();
        if (!tonemap_into(image.texture, ResourceState::Present, presentPipelineSwapchain_, extent.first,
                          extent.second, false, error)) return false;
        const rhi::PresentResult result = device_->present(swapchain_, lastFence_, error);
        if (result == rhi::PresentResult::OutOfDate || result == rhi::PresentResult::Suboptimal || image.suboptimal) {
            swapchainStale_ = true;
            return true;
        }
        return result == rhi::PresentResult::Presented;
    }

    std::pair<std::uint32_t, std::uint32_t> swapchain_extent() const {
        // The driver may pick an extent other than the drawable size (it reports the window's).
        std::uint32_t width = 0U, height = 0U;
        if (device_->swapchain_size(swapchain_, width, height)) return {width, height};
        return {std::max(options_.drawableWidth, 1U), std::max(options_.drawableHeight, 1U)};
    }

    void destroy_targets() {
        if (!device_) return;
        std::string ignored;
        if (aoGroup_) (void)device_->destroy_bind_group(aoGroup_, &ignored);
        if (aoView_) (void)device_->destroy_texture_view(aoView_, &ignored);
        if (aoTexture_) (void)device_->destroy_texture(aoTexture_, &ignored);
        aoGroup_ = {}; aoView_ = {}; aoTexture_ = {};
        if (giView_) (void)device_->destroy_texture_view(giView_, &ignored);
        if (giTexture_) (void)device_->destroy_texture(giTexture_, &ignored);
        giView_ = {}; giTexture_ = {};
        if (presentGroup_) (void)device_->destroy_bind_group(presentGroup_, &ignored);
        if (hdrView_) (void)device_->destroy_texture_view(hdrView_, &ignored);
        if (hdr_) (void)device_->destroy_texture(hdr_, &ignored);
        if (depthView_) (void)device_->destroy_texture_view(depthView_, &ignored);
        if (depth_) (void)device_->destroy_texture(depth_, &ignored);
        if (capture_) (void)device_->destroy_texture(capture_, &ignored);
        presentGroup_ = {}; hdrView_ = {}; hdr_ = {}; depthView_ = {}; depth_ = {}; capture_ = {};
    }

    VulkanPlayerRendererOptions options_;
    std::unique_ptr<rhi::VulkanDevice> device_;
    rhi::SwapchainHandle swapchain_{};
    rhi::TextureFormat swapchainFormat_{rhi::TextureFormat::BGRA8Unorm};
    bool swapchainStale_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    rhi::TextureHandle hdr_{};
    rhi::TextureViewHandle hdrView_{};
    rhi::TextureHandle depth_{};
    rhi::TextureViewHandle depthView_{};
    rhi::SamplerHandle depthSampler_{};
    rhi::TextureHandle capture_{};
    rhi::BufferHandle triangleIndices_{};
    rhi::BufferHandle presentConstants_{};
    rhi::SamplerHandle presentSampler_{};
    rhi::BindGroupLayoutHandle presentLayout_{};
    rhi::BindGroupHandle presentGroup_{};
    rhi::BufferHandle aoConstants_{};
    rhi::BindGroupLayoutHandle aoLayout_{};
    rhi::GraphicsPipelineHandle aoPipeline_{};
    rhi::GraphicsPipelineHandle tracedAoPipeline_{};
    rhi::GraphicsPipelineHandle giPipeline_{};
    rhi::BufferHandle gridPalette_{};
    rhi::TextureHandle giTexture_{};
    rhi::TextureViewHandle giView_{};
    rhi::BufferHandle gridBuffer_{};
    std::size_t gridCapacity_{};
    bool gridUploaded_{};
    render::SceneVoxelGrid grid_;
    std::vector<render::SceneGridObject> gridObjects_;
    rhi::TextureHandle aoTexture_{};
    rhi::TextureViewHandle aoView_{};
    rhi::BindGroupHandle aoGroup_{};
    rhi::GraphicsPipelineHandle presentPipelineOffscreen_{};
    rhi::GraphicsPipelineHandle presentPipelineSwapchain_{};
    rhi::FenceHandle lastFence_{};
    render::EnvironmentLightingGpuResources lighting_{};
    render::CascadedShadowAtlasResources atlas_{};
    render::LiveEnvironmentRendererResources live_{};
    std::array<float, 6> skyKey_{};
    std::unordered_map<GameObjectId, VoxelMesh> voxelMeshes_;
    std::unordered_map<const CookedPolygonAsset*, PolygonMesh> polygonMeshes_;
    std::vector<render::LivePolygonDraw> draws_;
    std::uint64_t frameIndex_{};
    float exposure_{1.0F};
    Float3 skyColor_{};
    std::uint32_t tonemapOperator_{};
    Rgba8Image frame_;
    bool rendered_{};
    bool captured_{};
    PlayerRenderStats stats_{};
};

} // namespace

bool vulkan_player_renderer_available() noexcept { return shader_blobs::available(); }

std::unique_ptr<IPlayerRenderer> make_vulkan_player_renderer(const VulkanPlayerRendererOptions& options,
                                                             std::string* error) {
    auto renderer = std::make_unique<VulkanPlayerRenderer>(options);
    if (!renderer->initialize(error)) return nullptr;
    return renderer;
}

} // namespace dve::player
