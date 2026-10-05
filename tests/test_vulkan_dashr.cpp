#include "dve/render/dashr_live_surface.hpp"
#include "dve/render/dashr_shell.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;
using namespace dve::rhi;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool required_gpu_test() {
    return std::getenv("DVE_REQUIRE_DASHR_GPU") != nullptr ||
           std::getenv("DVE_REQUIRE_VULKAN") != nullptr;
}

std::vector<std::byte> load_spirv(const std::filesystem::path& root,
                                  const std::string& name) {
    const auto path = root / name / (name + ".spv");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open DASHR SPIR-V: " + path.string());
    std::vector<char> raw{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    if (raw.size() < 20U || raw.size() % sizeof(std::uint32_t) != 0U)
        throw std::runtime_error("invalid DASHR SPIR-V length: " + path.string());
    std::vector<std::byte> result(raw.size());
    std::memcpy(result.data(), raw.data(), raw.size());
    return result;
}

CookedPolygonAsset triangle() {
    CookedPolygonAsset asset;
    asset.objectId = 731U;
    VoxelMaterialDefinition material;
    material.name = "Vulkan DASHR smoke test";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.images.push_back({"height", "image/raw", 1U, 1U,
                            {128U, 128U, 128U, 255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"height", 0U, 0U});
    asset.materialBindings[0].height.texture = 0U;
    asset.vertices = {
        {{0,0,0},{0,0,1},{1,0,0,1},{0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.1F,0.9F},{1,1,1,1},{0,0}},
    };
    asset.indices = {0U,1U,2U};
    asset.submeshes.push_back({"surface",0U,3U,0U});
    asset.bounds = {{0,0,0},{1,1,0}};
    asset.contentHash = polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)),
            "DASHR Vulkan fixture asset is invalid");
    return asset;
}

std::vector<std::byte> read_depth(IDevice& device, TextureHandle texture,
                                  std::uint32_t width, std::uint32_t height,
                                  std::string* error) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(width) * height * sizeof(float));
    require(device.read_texture(texture, 0U, 0U, bytes,
                                static_cast<std::size_t>(width) * sizeof(float),
                                error), error ? *error : "DASHR depth readback failed");
    return bytes;
}

} // namespace

int main() {
    try {
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (required_gpu_test())
                throw std::runtime_error(std::string(device.device_loss_reason()));
            std::cout << "DASHR Vulkan smoke test skipped: Vulkan device unavailable\n";
            return 0;
        }

        const char* shaderDirectory = std::getenv("DVE_DASHR_SHADER_DIR");
        if (!shaderDirectory || !*shaderDirectory) {
            if (required_gpu_test())
                throw std::runtime_error("DVE_DASHR_SHADER_DIR is required for this GPU test");
            std::cout << "DASHR Vulkan smoke test skipped: shader directory not configured\n";
            return 0;
        }
        const std::filesystem::path shaderRoot(shaderDirectory);
        std::string error;
        DashrAtlasShaderBytecode atlasCode{
            load_spirv(shaderRoot, "dashr_atlas_vs"),
            load_spirv(shaderRoot, "dashr_atlas_ps"),
            load_spirv(shaderRoot, "dashr_edge_fill")};
        DashrShellShaderBytecode shellCode;
        shellCode.vertex = load_spirv(shaderRoot, "dashr_shell_vs");
        shellCode.fragment = load_spirv(shaderRoot, "dashr_shell_ps");
        shellCode.shadowFragment = load_spirv(shaderRoot, "dashr_shell_shadow_ps");

        constexpr std::uint32_t extent = 96U;
        const auto asset = triangle();
        DashrLiveSurfaceInstance surface(device);
        require(surface.initialize(asset, {}, atlasCode, 64U, &error), error);
        require(surface.update_pose({{}, 1U}, &error), error);
        require(surface.published(), "DASHR atlas did not publish on Vulkan");

        DashrShellRendererResources shellRenderer;
        require(create_dashr_shell_renderer(device, shellCode, TextureFormat::RGBA8Unorm,
                                             4096U, shellRenderer, &error), error);

        TextureDesc heightDesc;
        heightDesc.format = TextureFormat::RGBA8Unorm;
        heightDesc.width = heightDesc.height = 4U;
        heightDesc.usage = TextureUsage::Sampled | TextureUsage::CopyDestination;
        heightDesc.initialState = ResourceState::Undefined;
        heightDesc.debugName = "DASHR smoke height";
        const auto height = device.create_texture(heightDesc, &error);
        require(static_cast<bool>(height), error);
        const std::array<std::byte,64> heightPixels = [] {
            std::array<std::byte,64> pixels{};
            for (std::size_t i=0; i<pixels.size(); i+=4U) {
                pixels[i]=pixels[i+1U]=pixels[i+2U]=std::byte{128};
                pixels[i+3U]=std::byte{255};
            }
            return pixels;
        }();
        require(device.write_texture(height,0U,0U,heightPixels,16U,&error),error);
        TextureViewDesc heightViewDesc;
        heightViewDesc.texture=height;
        const auto heightView=device.create_texture_view(heightViewDesc,&error);
        require(static_cast<bool>(heightView),error);
        SamplerDesc samplerDesc;
        samplerDesc.addressU=samplerDesc.addressV=AddressMode::ClampToEdge;
        const auto heightSampler=device.create_sampler(samplerDesc,&error);
        require(static_cast<bool>(heightSampler),error);

        TextureDesc colorDesc;
        colorDesc.format=TextureFormat::RGBA8Unorm;
        colorDesc.width=colorDesc.height=extent;
        colorDesc.usage=TextureUsage::RenderTarget|TextureUsage::CopySource;
        colorDesc.initialState=ResourceState::RenderTarget;
        const auto color=device.create_texture(colorDesc,&error);
        require(static_cast<bool>(color),error);
        TextureDesc depthDesc;
        depthDesc.format=TextureFormat::D32Float;
        depthDesc.width=depthDesc.height=extent;
        depthDesc.usage=TextureUsage::DepthStencil|TextureUsage::CopySource;
        depthDesc.initialState=ResourceState::DepthWrite;
        const auto depth=device.create_texture(depthDesc,&error);
        require(static_cast<bool>(depth),error);

        DashrShellDraw draw;
        draw.shell=&surface.shell();
        draw.atlas=&surface.atlas();
        draw.heightView=heightView;
        draw.heightSampler=heightSampler;
        draw.shellSubmeshIndex=0U;
        draw.cameraObjectPosition={0.5F,0.5F,1.0F};
        draw.cameraWorldPosition=draw.cameraObjectPosition;
        draw.settings=surface.settings();
        draw.debugMode=2U;
        DashrShellFrameDesc cameraFrame;
        cameraFrame.colorTarget=color;
        cameraFrame.depthTarget=depth;
        cameraFrame.width=cameraFrame.height=extent;
        cameraFrame.draws=std::span(&draw,1U);
        DashrShellFrameStats cameraStats;
        FenceHandle cameraFence;
        require(record_dashr_shell_frame(device,shellRenderer,cameraFrame,
                                         cameraStats,&cameraFence,&error),error);
        require(cameraFence && device.wait(cameraFence,&error),error);
        require(cameraStats.draws==1U && cameraStats.diagnosticDraws==1U,
                "DASHR camera shell draw was not recorded");

        DashrShadowDraw shadowDraw;
        shadowDraw.shell=&surface.shell();
        shadowDraw.atlas=&surface.atlas();
        shadowDraw.heightView=heightView;
        shadowDraw.heightSampler=heightSampler;
        shadowDraw.shellSubmeshIndex=0U;
        shadowDraw.lightRayDirectionObject={0,0,-1};
        shadowDraw.settings=surface.settings();
        DashrShadowFrameDesc shadowFrame;
        shadowFrame.depthTarget=depth;
        shadowFrame.viewport={0,0,static_cast<float>(extent),static_cast<float>(extent),0,1};
        shadowFrame.scissor={0U,0U,extent,extent};
        shadowFrame.clearDepthTarget=true;
        shadowFrame.draws=std::span(&shadowDraw,1U);
        DashrShadowFrameStats shadowStats;
        FenceHandle shadowFence;
        require(record_dashr_shadow_frame(device,shellRenderer,shadowFrame,
                                          shadowStats,&shadowFence,&error),error);
        require(shadowFence && device.wait(shadowFence,&error),error);
        require(shadowStats.draws==1U,"DASHR displaced shadow draw was not recorded");
        const auto depthPixels=read_depth(device,depth,extent,extent,&error);
        bool depthWritten=false;
        for(std::size_t offset=0; offset<depthPixels.size(); offset+=sizeof(float)) {
            float value=1.0F;
            std::memcpy(&value,depthPixels.data()+offset,sizeof(float));
            if(value<0.99F) { depthWritten=true; break; }
        }
        require(depthWritten,"DASHR shadow shader wrote no displaced depth");

        require(device.destroy_texture(color,&error),error);
        require(device.destroy_texture(depth,&error),error);
        require(device.destroy_sampler(heightSampler,&error),error);
        require(device.destroy_texture_view(heightView,&error),error);
        require(device.destroy_texture(height,&error),error);
        require(destroy_dashr_shell_renderer(device,shellRenderer,&error),error);
        surface.reset();
        std::cout << "DASHR Vulkan smoke test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DASHR Vulkan smoke test: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
