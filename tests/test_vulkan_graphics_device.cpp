#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/rhi/vulkan_device.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
          std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

std::vector<std::uint32_t> encoded_string(const char* text) {
    const std::size_t bytes = std::strlen(text) + 1U;
    std::vector<std::uint32_t> words((bytes + 3U) / 4U, 0U);
    std::memcpy(words.data(), text, bytes);
    return words;
}

void emit_entry_point(std::vector<std::uint32_t>& words, std::uint32_t model,
                      std::uint32_t function, const char* name,
                      std::initializer_list<std::uint32_t> interfaces) {
    auto stringWords = encoded_string(name);
    const std::uint32_t count = 3U + static_cast<std::uint32_t>(stringWords.size()) +
                                static_cast<std::uint32_t>(interfaces.size());
    words.push_back((count << 16U) | 15U); // OpEntryPoint
    words.push_back(model);
    words.push_back(function);
    words.insert(words.end(), stringWords.begin(), stringWords.end());
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}

std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}

std::vector<std::byte> vertex_shader() {
    // Descriptor-free SPIR-V 1.0 equivalent to a vertex-index generated triangle.
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 37U, 0U};
    emit(w, 17, {1});                    // OpCapability Shader
    emit(w, 14, {0, 1});                 // OpMemoryModel Logical GLSL450
    emit_entry_point(w, 0, 26, "main", {24, 25});
    emit(w, 71, {13, 2});                // OpDecorate gl_PerVertex Block
    emit(w, 72, {13, 0, 11, 0});         // OpMemberDecorate Position
    emit(w, 71, {24, 11, 42});           // OpDecorate VertexIndex
    emit(w, 19, {1});                    // void
    emit(w, 33, {2, 1});                 // function type
    emit(w, 22, {3, 32});                // float
    emit(w, 23, {4, 3, 2});              // vec2
    emit(w, 23, {5, 3, 4});              // vec4
    emit(w, 21, {6, 32, 0});             // uint
    emit(w, 21, {7, 32, 1});             // int
    emit(w, 43, {6, 8, 3});              // uint 3
    emit(w, 28, {9, 4, 8});              // array[3] vec2
    emit(w, 32, {10, 6, 9});             // ptr Private array
    emit(w, 32, {11, 6, 4});             // ptr Private vec2
    emit(w, 30, {13, 5});                // gl_PerVertex struct
    emit(w, 32, {12, 3, 13});            // ptr Output struct
    emit(w, 32, {14, 3, 5});             // ptr Output vec4
    emit(w, 32, {36, 1, 7});             // ptr Input int
    emit(w, 43, {3, 15, std::bit_cast<std::uint32_t>(-0.8F)});
    emit(w, 43, {3, 16, std::bit_cast<std::uint32_t>(0.8F)});
    emit(w, 43, {3, 17, std::bit_cast<std::uint32_t>(0.0F)});
    emit(w, 43, {3, 18, std::bit_cast<std::uint32_t>(1.0F)});
    emit(w, 44, {4, 19, 15, 15});        // (-.8,-.8)
    emit(w, 44, {4, 20, 16, 15});        // (.8,-.8)
    emit(w, 44, {4, 21, 17, 16});        // (0,.8)
    emit(w, 44, {9, 22, 19, 20, 21});    // positions array
    emit(w, 43, {7, 35, 0});             // int 0
    emit(w, 59, {10, 23, 6, 22});        // private positions
    emit(w, 59, {36, 24, 1});            // gl_VertexIndex
    emit(w, 59, {12, 25, 3});            // gl_PerVertex output
    emit(w, 54, {1, 26, 0, 2});          // function main
    emit(w, 248, {27});                  // label
    emit(w, 61, {7, 28, 24});            // load vertex index
    emit(w, 65, {11, 29, 23, 28});       // positions[index]
    emit(w, 61, {4, 30, 29});            // load vec2
    emit(w, 81, {3, 31, 30, 0});         // x
    emit(w, 81, {3, 32, 30, 1});         // y
    emit(w, 80, {5, 33, 31, 32, 17, 18});
    emit(w, 65, {14, 34, 25, 35});       // &gl_Position
    emit(w, 62, {34, 33});                // store
    emit(w, 253, {});                     // return
    emit(w, 56, {});                      // function end
    return as_bytes(w);
}

std::vector<std::byte> fragment_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 14U, 0U};
    emit(w, 17, {1});                    // Shader
    emit(w, 14, {0, 1});                 // Logical GLSL450
    emit_entry_point(w, 4, 11, "main", {6});
    emit(w, 16, {11, 7});                // OriginUpperLeft
    emit(w, 71, {6, 30, 0});             // Location 0
    emit(w, 19, {1});
    emit(w, 33, {2, 1});
    emit(w, 22, {3, 32});
    emit(w, 23, {4, 3, 4});
    emit(w, 32, {5, 3, 4});              // ptr Output vec4
    emit(w, 59, {5, 6, 3});              // output color
    emit(w, 43, {3, 7, std::bit_cast<std::uint32_t>(0.12F)});
    emit(w, 43, {3, 8, std::bit_cast<std::uint32_t>(0.68F)});
    emit(w, 43, {3, 9, std::bit_cast<std::uint32_t>(0.96F)});
    emit(w, 43, {3, 10, std::bit_cast<std::uint32_t>(1.0F)});
    emit(w, 44, {4, 12, 7, 8, 9, 10});
    emit(w, 54, {1, 11, 0, 2});
    emit(w, 248, {13});
    emit(w, 62, {6, 12});
    emit(w, 253, {});
    emit(w, 56, {});
    return as_bytes(w);
}
} // namespace

int main() {
    try {
        using namespace dve::rhi;
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error(std::string("required Vulkan device unavailable: ") +
                                         std::string(device.device_loss_reason()));
            std::cout << "Vulkan graphics test skipped: " << device.device_loss_reason() << '\n';
            return 0;
        }
        std::string error;
        constexpr std::uint32_t width = 96U;
        constexpr std::uint32_t height = 64U;

        const TextureHandle uploadTexture = device.create_texture({
            TextureDimension::Texture2D, TextureFormat::RGBA8Unorm, 4U, 4U, 1U, 1U, 1U,
            TextureUsage::CopySource | TextureUsage::CopyDestination,
            ResourceState::Undefined, "Vulkan upload/readback"}, &error);
        require(static_cast<bool>(uploadTexture), error.c_str());
        std::array<std::byte, 64> texels{};
        for (std::size_t i = 0; i < texels.size(); ++i)
            texels[i] = static_cast<std::byte>((i * 7U + 3U) & 0xFFU);
        require(device.write_texture(uploadTexture, 0U, 0U, texels, 16U, &error), error.c_str());
        std::array<std::byte, 64> copiedTexels{};
        require(device.read_texture(uploadTexture, 0U, 0U, copiedTexels, 16U, &error), error.c_str());
        require(copiedTexels == texels, "Vulkan texture upload/readback mismatch");

        const TextureHandle color = device.create_texture({
            TextureDimension::Texture2D, TextureFormat::RGBA8Unorm, width, height, 1U, 1U, 1U,
            TextureUsage::RenderTarget | TextureUsage::CopySource,
            ResourceState::RenderTarget, "Vulkan offscreen color"}, &error);
        const TextureHandle depth = device.create_texture({
            TextureDimension::Texture2D, TextureFormat::D32Float, width, height, 1U, 1U, 1U,
            TextureUsage::DepthStencil, ResourceState::DepthWrite,
            "Vulkan offscreen depth"}, &error);
        require(color && depth, error.c_str());

        const TextureHandle sampledTexture = device.create_texture({
            TextureDimension::Texture2D, TextureFormat::RGBA8Unorm, 4U, 4U, 1U, 1U, 1U,
            TextureUsage::Sampled | TextureUsage::CopyDestination,
            ResourceState::ShaderRead, "Vulkan sampled texture"}, &error);
        require(static_cast<bool>(sampledTexture), error.c_str());
        TextureViewDesc sampledViewDesc;
        sampledViewDesc.texture = sampledTexture;
        sampledViewDesc.debugName = "Vulkan sampled texture view";
        const TextureViewHandle sampledView = device.create_texture_view(sampledViewDesc, &error);
        require(static_cast<bool>(sampledView), error.c_str());
        SamplerDesc samplerDesc;
        samplerDesc.addressU = AddressMode::ClampToEdge;
        samplerDesc.addressV = AddressMode::ClampToEdge;
        samplerDesc.maximumLod = 0.0F;
        samplerDesc.debugName = "Vulkan graphics sampler";
        const SamplerHandle sampler = device.create_sampler(samplerDesc, &error);
        require(static_cast<bool>(sampler), error.c_str());
        BindGroupLayoutDesc sampledLayoutDesc;
        sampledLayoutDesc.debugName = "Vulkan sampled graphics layout";
        sampledLayoutDesc.bindings = {
            {0U, BindingType::SampledTexture, ShaderStage::Fragment}};
        const BindGroupLayoutHandle sampledLayout =
            device.create_bind_group_layout(sampledLayoutDesc, &error);
        require(static_cast<bool>(sampledLayout), error.c_str());
        BindGroupDesc sampledGroupDesc;
        sampledGroupDesc.layout = sampledLayout;
        sampledGroupDesc.debugName = "Vulkan sampled graphics group";
        sampledGroupDesc.entries = {{0U, {}, sampledView, 0U, 0U, sampler}};
        const BindGroupHandle sampledGroup = device.create_bind_group(sampledGroupDesc, &error);
        require(static_cast<bool>(sampledGroup), error.c_str());

        const TextureHandle cubeTexture = device.create_texture({
            TextureDimension::TextureCube, TextureFormat::RGBA8Unorm, 8U, 8U, 1U, 1U, 6U,
            TextureUsage::Sampled, ResourceState::ShaderRead, "Vulkan cube texture"}, &error);
        require(static_cast<bool>(cubeTexture), error.c_str());
        TextureViewDesc cubeViewDesc;
        cubeViewDesc.texture = cubeTexture;
        cubeViewDesc.layerCount = 6U;
        cubeViewDesc.dimension = TextureViewDimension::TextureCube;
        cubeViewDesc.debugName = "Vulkan cube view";
        const TextureViewHandle cubeView = device.create_texture_view(cubeViewDesc, &error);
        require(static_cast<bool>(cubeView), error.c_str());

        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.debugName = "embedded SPIR-V triangle";
        pipelineDesc.vertexBytecode = vertex_shader();
        pipelineDesc.fragmentBytecode = fragment_shader();
        pipelineDesc.colorFormat = TextureFormat::RGBA8Unorm;
        pipelineDesc.depthFormat = TextureFormat::D32Float;
        pipelineDesc.cullMode = CullMode::Disabled;
        pipelineDesc.bindGroupLayouts = {sampledLayout};
        const GraphicsPipelineHandle pipeline = device.create_graphics_pipeline(pipelineDesc, &error);
        require(static_cast<bool>(pipeline), error.c_str());

        const BufferHandle vertices = device.create_buffer({
            16U, BufferUsage::Vertex, MemoryDomain::DeviceLocal,
            "dummy vertex buffer", ResourceState::ShaderRead}, &error);
        const BufferHandle indices = device.create_buffer({
            3U * sizeof(std::uint32_t), BufferUsage::Index,
            MemoryDomain::Upload, "triangle indices", ResourceState::ShaderRead}, &error);
        require(vertices && indices, error.c_str());
        const std::array<std::uint32_t, 3> indexData{0U, 1U, 2U};
        require(device.write_buffer(indices, 0U,
            std::as_bytes(std::span<const std::uint32_t>(indexData)), &error), error.c_str());

        const CommandListHandle commands = device.begin_commands(QueueKind::Graphics,
                                                                  "offscreen indexed triangle", &error);
        require(static_cast<bool>(commands), error.c_str());
        RenderPassDesc pass;
        pass.colors.push_back({color, true, 0.02F, 0.03F, 0.05F, 1.0F});
        pass.depth = RenderPassDepthAttachment{depth, true, 1.0F};
        require(device.begin_render_pass(commands, pass, &error), error.c_str());
        require(device.bind_graphics_pipeline(commands, pipeline, &error), error.c_str());
        require(device.bind_graphics_bind_group(commands, 0U, sampledGroup, &error), error.c_str());
        require(device.set_viewport(commands, {0.0F, 0.0F, static_cast<float>(width),
                                                static_cast<float>(height), 0.0F, 1.0F}, &error),
                error.c_str());
        require(device.set_scissor(commands, {0, 0, width, height}, &error), error.c_str());
        require(device.bind_vertex_buffer(commands, 0U, vertices, 0U, 4U, &error), error.c_str());
        require(device.bind_index_buffer(commands, indices, 0U, IndexFormat::Uint32, &error),
                error.c_str());
        require(device.draw_indexed(commands, 3U, 1U, 0U, 0, 0U, &error), error.c_str());
        require(device.end_render_pass(commands, &error), error.c_str());
        const FenceHandle fence = device.submit(commands, &error);
        require(fence && device.fence_complete(fence), error.c_str());

        std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U);
        require(device.read_texture(color, 0U, 0U, pixels,
                                    static_cast<std::size_t>(width) * 4U, &error), error.c_str());
        const auto channel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t c) {
            return std::to_integer<std::uint8_t>(pixels[(static_cast<std::size_t>(y) * width + x) * 4U + c]);
        };
        require(channel(width / 2U, height / 2U, 1U) > 120U,
                "Vulkan indexed triangle did not reach the center pixel");
        require(channel(1U, 1U, 0U) < 20U && channel(1U, 1U, 1U) < 20U,
                "Vulkan render-pass clear color is incorrect");

        // Compare the software Vulkan readback against a deterministic CPU coverage oracle.
        const auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
            return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
        };
        const auto screen_x = [](float ndc, float extent) { return (ndc + 1.0F) * 0.5F * extent; };
        const float ax = screen_x(-0.8F, static_cast<float>(width));
        const float ay = screen_x(-0.8F, static_cast<float>(height));
        const float bx = screen_x( 0.8F, static_cast<float>(width));
        const float by = screen_x(-0.8F, static_cast<float>(height));
        const float cx = screen_x( 0.0F, static_cast<float>(width));
        const float cy = screen_x( 0.8F, static_cast<float>(height));
        std::uint64_t expectedCovered{}, actualCovered{}, mismatches{};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const float e0 = edge(ax, ay, bx, by, px, py);
                const float e1 = edge(bx, by, cx, cy, px, py);
                const float e2 = edge(cx, cy, ax, ay, px, py);
                const bool expected = (e0 <= 0.0F && e1 <= 0.0F && e2 <= 0.0F) ||
                                      (e0 >= 0.0F && e1 >= 0.0F && e2 >= 0.0F);
                const bool actual = channel(x, y, 1U) > 80U;
                expectedCovered += expected ? 1U : 0U;
                actualCovered += actual ? 1U : 0U;
                mismatches += expected != actual ? 1U : 0U;
            }
        }
        require(expectedCovered > 1000U && actualCovered > 1000U,
                "Vulkan/CPU triangle coverage is unexpectedly empty");
        require(mismatches < 200U, "Vulkan triangle differs excessively from CPU coverage oracle");

        if (const char* output = std::getenv("DVE_VULKAN_TEST_OUTPUT")) {
            std::ofstream ppm(output, std::ios::binary | std::ios::trunc);
            require(ppm.good(), "could not open Vulkan reference image output");
            ppm << "P6\n" << width << ' ' << height << "\n255\n";
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::array<char, 3> rgb{
                        static_cast<char>(channel(x, y, 0U)),
                        static_cast<char>(channel(x, y, 1U)),
                        static_cast<char>(channel(x, y, 2U))};
                    ppm.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
                }
            require(ppm.good(), "could not write Vulkan reference image output");
        }

        require(device.destroy_buffer(vertices, &error), error.c_str());
        require(device.destroy_buffer(indices, &error), error.c_str());
        require(device.destroy_graphics_pipeline(pipeline, &error), error.c_str());
        require(device.destroy_bind_group(sampledGroup, &error), error.c_str());
        require(device.destroy_bind_group_layout(sampledLayout, &error), error.c_str());
        require(device.destroy_sampler(sampler, &error), error.c_str());
        require(device.destroy_texture_view(sampledView, &error), error.c_str());
        require(device.destroy_texture(sampledTexture, &error), error.c_str());
        require(device.destroy_texture_view(cubeView, &error), error.c_str());
        require(device.destroy_texture(cubeTexture, &error), error.c_str());
        require(device.destroy_texture(color, &error), error.c_str());
        require(device.destroy_texture(depth, &error), error.c_str());
        require(device.destroy_texture(uploadTexture, &error), error.c_str());
        const DeviceStatistics stats = device.statistics();
        require(stats.renderPassesExecuted == 1U && stats.indexedDrawsExecuted == 1U,
                "Vulkan graphics statistics are incorrect");
        require(stats.texturesCreated == stats.texturesDestroyed,
                "Vulkan texture lifetime is unbalanced");
        std::cout << "Vulkan offscreen indexed graphics tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Vulkan offscreen indexed graphics tests failed: " << exception.what() << '\n';
        return 1;
    }
}
