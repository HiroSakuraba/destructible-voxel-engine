#include "dve/rhi/null_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve::rhi;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

TextureHandle target(NullDevice& device, std::uint32_t width, std::uint32_t height,
                     std::string name) {
    std::string error;
    TextureDesc desc;
    desc.format = TextureFormat::RGBA8Unorm;
    desc.width = width;
    desc.height = height;
    desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    desc.initialState = ResourceState::RenderTarget;
    desc.debugName = std::move(name);
    auto handle = device.create_texture(desc, &error);
    require(static_cast<bool>(handle), error.c_str());
    return handle;
}

GraphicsPipelineHandle pipeline(NullDevice& device, std::size_t targetCount) {
    std::string error;
    GraphicsPipelineDesc desc;
    desc.vertexBytecode = {std::byte{1}};
    desc.fragmentBytecode = {std::byte{2}};
    desc.colorFormat.reset();
    desc.colorFormats.assign(targetCount, TextureFormat::RGBA8Unorm);
    desc.depthFormat.reset();
    desc.cullMode = CullMode::Disabled;
    auto handle = device.create_graphics_pipeline(desc, &error);
    require(static_cast<bool>(handle), error.c_str());
    return handle;
}

void test_four_target_pass() {
    NullDevice device;
    std::string error;
    std::array<TextureHandle, 4> colors{
        target(device, 16, 16, "mrt0"), target(device, 16, 16, "mrt1"),
        target(device, 16, 16, "mrt2"), target(device, 16, 16, "mrt3")};
    const auto mrt = pipeline(device, colors.size());

    BufferDesc vb; vb.bytes = 16U; vb.usage = BufferUsage::Vertex;
    vb.memory = MemoryDomain::Upload; vb.initialState = ResourceState::ShaderRead;
    BufferDesc ib; ib.bytes = 3U * sizeof(std::uint32_t); ib.usage = BufferUsage::Index;
    ib.memory = MemoryDomain::Upload; ib.initialState = ResourceState::ShaderRead;
    const auto vertex = device.create_buffer(vb, &error);
    const auto index = device.create_buffer(ib, &error);
    require(vertex && index, error.c_str());
    const std::array<std::uint32_t, 3> indices{0U, 1U, 2U};
    require(device.write_buffer(index, 0U, std::as_bytes(std::span(indices)), &error),
            error.c_str());

    const auto commands = device.begin_commands(QueueKind::Graphics, "four target pass", &error);
    require(static_cast<bool>(commands), error.c_str());
    RenderPassDesc pass;
    pass.debugName = "MRT contract";
    for (std::size_t i = 0; i < colors.size(); ++i)
        pass.colors.push_back({colors[i], true, static_cast<float>(i) * 0.1F, 0, 0, 1});
    require(device.begin_render_pass(commands, pass, &error), error.c_str());
    require(device.bind_graphics_pipeline(commands, mrt, &error), error.c_str());
    require(device.set_viewport(commands, {0,0,16,16,0,1}, &error), error.c_str());
    require(device.set_scissor(commands, {0,0,16,16}, &error), error.c_str());
    require(device.bind_vertex_buffer(commands, 0U, vertex, 0U, 4U, &error), error.c_str());
    require(device.bind_index_buffer(commands, index, 0U, IndexFormat::Uint32, &error),
            error.c_str());
    require(device.draw_indexed(commands, 3U, 1U, 0U, 0, 0U, &error), error.c_str());
    require(device.end_render_pass(commands, &error), error.c_str());
    require(static_cast<bool>(device.submit(commands, &error)), error.c_str());
    require(device.statistics().renderPassesExecuted == 1U, "MRT pass was not submitted");
    require(device.statistics().indexedDrawsExecuted == 1U, "MRT draw was not submitted");
}

void test_pipeline_compatibility_and_limits() {
    NullDevice device;
    std::string error;
    const auto a = target(device, 8, 8, "a");
    const auto b = target(device, 8, 8, "b");
    const auto one = pipeline(device, 1U);
    const auto two = pipeline(device, 2U);

    auto commands = device.begin_commands(QueueKind::Graphics, "compatibility", &error);
    RenderPassDesc pass; pass.colors = {{a,true,0,0,0,1},{b,true,0,0,0,1}};
    require(device.begin_render_pass(commands, pass, &error), error.c_str());
    require(!device.bind_graphics_pipeline(commands, one, &error),
            "single-target pipeline bound to two-target pass");
    error.clear();
    require(device.bind_graphics_pipeline(commands, two, &error), error.c_str());
    require(device.end_render_pass(commands, &error), error.c_str());

    const auto wrongSize = target(device, 4, 8, "wrong-size");
    commands = device.begin_commands(QueueKind::Graphics, "dimension mismatch", &error);
    RenderPassDesc mismatch; mismatch.colors = {{a,true,0,0,0,1},{wrongSize,true,0,0,0,1}};
    require(!device.begin_render_pass(commands, mismatch, &error),
            "MRT pass accepted mismatched dimensions");

    commands = device.begin_commands(QueueKind::Graphics, "too many", &error);
    RenderPassDesc tooMany;
    tooMany.colors = {{a,true,0,0,0,1},{b,true,0,0,0,1},{a,true,0,0,0,1},
                      {b,true,0,0,0,1},{a,true,0,0,0,1}};
    require(!device.begin_render_pass(commands, tooMany, &error),
            "MRT pass accepted more than four color attachments");

    GraphicsPipelineDesc invalid;
    invalid.vertexBytecode = {std::byte{1}};
    invalid.fragmentBytecode = {std::byte{2}};
    invalid.colorFormat.reset();
    invalid.colorFormats.assign(5U, TextureFormat::RGBA8Unorm);
    invalid.depthFormat.reset();
    require(!device.create_graphics_pipeline(invalid, &error),
            "graphics pipeline accepted more than four color formats");
}

void test_separate_image_and_sampler_bindings() {
    NullDevice device;
    std::string error;
    TextureDesc textureDesc;
    textureDesc.width = 4U;
    textureDesc.height = 4U;
    textureDesc.usage = TextureUsage::Sampled;
    const auto texture = device.create_texture(textureDesc, &error);
    TextureViewDesc viewDesc;
    viewDesc.texture = texture;
    const auto view = device.create_texture_view(viewDesc, &error);
    const auto sampler = device.create_sampler(SamplerDesc{}, &error);
    require(texture && view && sampler, error.c_str());

    BindGroupLayoutDesc layoutDesc;
    layoutDesc.bindings = {{0U, BindingType::SampledImage, ShaderStage::Fragment},
                           {1U, BindingType::Sampler, ShaderStage::Fragment}};
    const auto layout = device.create_bind_group_layout(layoutDesc, &error);
    require(static_cast<bool>(layout), error.c_str());
    BindGroupDesc groupDesc;
    groupDesc.layout = layout;
    groupDesc.entries = {{0U, {}, view, 0U, 0U, {}},
                         {1U, {}, {}, 0U, 0U, sampler}};
    const auto group = device.create_bind_group(groupDesc, &error);
    require(static_cast<bool>(group), error.c_str());
    require(!device.destroy_texture_view(view, &error), "separate image was not retained");
    require(!device.destroy_sampler(sampler, &error), "separate sampler was not retained");

    require(device.destroy_bind_group(group, &error), error.c_str());
    groupDesc.entries[0].sampler = sampler;
    require(!device.create_bind_group(groupDesc, &error),
            "sampled-image binding accepted an embedded sampler");
    groupDesc.entries[0].sampler = {};
    groupDesc.entries[1].textureView = view;
    require(!device.create_bind_group(groupDesc, &error),
            "sampler binding accepted a texture view");
}

} // namespace

int main() {
    try {
        test_four_target_pass();
        test_pipeline_compatibility_and_limits();
        test_separate_image_and_sampler_bindings();
        std::cout << "dve_rhi_mrt_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_rhi_mrt_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
