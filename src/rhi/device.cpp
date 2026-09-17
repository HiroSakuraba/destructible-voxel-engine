#include "dve/rhi/device.hpp"

#include <algorithm>

namespace dve::rhi {

std::uint32_t vertex_format_bytes(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::Float2: return 8U;
        case VertexFormat::Float3: return 12U;
        case VertexFormat::Float4: return 16U;
    }
    return 0U;
}

bool validate_vertex_input_layout(const GraphicsPipelineDesc& desc, std::string* error) {
    const auto fail = [&](std::string_view message) {
        if (error) error->assign(message);
        return false;
    };
    if (!desc.vertexBuffer) {
        if (!desc.vertexAttributes.empty())
            return fail("vertex attributes require a vertex-buffer layout");
        return true;
    }
    if (desc.vertexBuffer->stride == 0U || desc.vertexBuffer->stride > 4096U ||
        desc.vertexAttributes.empty() || desc.vertexAttributes.size() > 16U)
        return fail("vertex-buffer stride or attribute count is invalid");
    std::vector<std::uint32_t> locations;
    locations.reserve(desc.vertexAttributes.size());
    for (std::size_t index = 0U; index < desc.vertexAttributes.size(); ++index) {
        const VertexAttributeDesc& attribute = desc.vertexAttributes[index];
        const std::uint32_t bytes = vertex_format_bytes(attribute.format);
        if (bytes == 0U || attribute.location >= 16U ||
            std::find(locations.begin(), locations.end(), attribute.location) != locations.end() ||
            attribute.offset % 4U != 0U || attribute.offset > desc.vertexBuffer->stride ||
            bytes > desc.vertexBuffer->stride - attribute.offset)
            return fail("vertex attribute location, format, or range is invalid");
        for (std::size_t priorIndex = 0U; priorIndex < index; ++priorIndex) {
            const VertexAttributeDesc& prior = desc.vertexAttributes[priorIndex];
            const std::uint32_t priorEnd = prior.offset + vertex_format_bytes(prior.format);
            const std::uint32_t end = attribute.offset + bytes;
            if (attribute.offset < priorEnd && prior.offset < end)
                return fail("vertex attribute byte ranges overlap");
        }
        locations.push_back(attribute.location);
    }
    return true;
}

namespace {
void unsupported(std::string* error, std::string_view operation) {
    if (error != nullptr) *error = std::string(operation) + " is not supported by this RHI backend";
}
}

TextureFormatCapabilities IDevice::texture_format_capabilities(TextureFormat) const noexcept {
    return {};
}

bool IDevice::write_texture(TextureHandle, std::uint32_t, std::uint32_t,
                            std::span<const std::byte>, std::size_t, std::string* error) {
    unsupported(error, "texture upload");
    return false;
}
bool IDevice::read_texture(TextureHandle, std::uint32_t, std::uint32_t,
                           std::span<std::byte>, std::size_t, std::string* error) {
    unsupported(error, "texture readback");
    return false;
}
GraphicsPipelineHandle IDevice::create_graphics_pipeline(const GraphicsPipelineDesc&,
                                                         std::string* error) {
    unsupported(error, "graphics pipeline creation");
    return {};
}
bool IDevice::destroy_graphics_pipeline(GraphicsPipelineHandle, std::string* error) {
    unsupported(error, "graphics pipeline destruction");
    return false;
}
bool IDevice::begin_render_pass(CommandListHandle, const RenderPassDesc&, std::string* error) {
    unsupported(error, "render pass recording");
    return false;
}
bool IDevice::end_render_pass(CommandListHandle, std::string* error) {
    unsupported(error, "render pass recording");
    return false;
}
bool IDevice::bind_graphics_pipeline(CommandListHandle, GraphicsPipelineHandle,
                                     std::string* error) {
    unsupported(error, "graphics pipeline binding");
    return false;
}
bool IDevice::set_viewport(CommandListHandle, const Viewport&, std::string* error) {
    unsupported(error, "viewport recording");
    return false;
}
bool IDevice::set_scissor(CommandListHandle, const ScissorRect&, std::string* error) {
    unsupported(error, "scissor recording");
    return false;
}
bool IDevice::clear_depth_region(CommandListHandle, float, const ScissorRect&,
                                 std::string* error) {
    unsupported(error, "depth-region clearing");
    return false;
}
bool IDevice::bind_vertex_buffer(CommandListHandle, std::uint32_t, BufferHandle,
                                 std::size_t, std::uint32_t, std::string* error) {
    unsupported(error, "vertex-buffer binding");
    return false;
}
bool IDevice::bind_index_buffer(CommandListHandle, BufferHandle, std::size_t,
                                IndexFormat, std::string* error) {
    unsupported(error, "index-buffer binding");
    return false;
}
bool IDevice::bind_compute_bind_group(CommandListHandle, std::uint32_t, BindGroupHandle,
                                        std::string* error) {
    if (error) *error = "compute bind groups are not supported by this backend";
    return false;
}

bool IDevice::bind_graphics_bind_group(CommandListHandle, std::uint32_t, BindGroupHandle,
                                       std::string* error) {
    unsupported(error, "graphics bind-group binding");
    return false;
}
bool IDevice::draw_indexed(CommandListHandle, std::uint32_t, std::uint32_t,
                           std::uint32_t, std::int32_t, std::uint32_t,
                           std::string* error) {
    unsupported(error, "indexed drawing");
    return false;
}
std::string_view backend_name(Backend backend) noexcept {
    switch (backend) {
        case Backend::Null: return "Null";
        case Backend::Direct3D12: return "Direct3D 12";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Metal: return "Metal";
    }
    return "Unknown";
}
std::string_view resource_state_name(ResourceState state) noexcept {
    switch (state) {
        case ResourceState::Undefined: return "Undefined";
        case ResourceState::CopySource: return "CopySource";
        case ResourceState::CopyDestination: return "CopyDestination";
        case ResourceState::ShaderRead: return "ShaderRead";
        case ResourceState::ShaderWrite: return "ShaderWrite";
        case ResourceState::RenderTarget: return "RenderTarget";
        case ResourceState::DepthWrite: return "DepthWrite";
        case ResourceState::Present: return "Present";
    }
    return "Unknown";
}
} // namespace dve::rhi
