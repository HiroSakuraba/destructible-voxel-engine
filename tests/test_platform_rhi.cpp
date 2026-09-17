#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/platform/headless_application_host.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/render/brickmap_rhi_mirror.hpp"
#include "dve/voxel_object.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_headless_host_contract() {
    using namespace dve::platform;
    HeadlessApplicationHost host;
    std::string error;
    require(!host.create_window({"bad", 0, 800}, &error), "invalid window size was accepted");
    require(!error.empty(), "invalid window size did not report an error");

    require(host.create_window({"DVE host test", 1280, 800}, &error), error.c_str());
    require(host.has_window(), "headless host did not publish window state");
    require(host.window_metrics().logicalWidth == 1280, "initial width mismatch");
    require(host.native_window_handle().backend == HostBackend::Headless, "wrong native handle backend");

    host.set_clipboard_text("voxel clipboard");
    require(host.clipboard_text() == "voxel clipboard", "clipboard round trip failed");
    host.set_window_title("Dirty Scene *");
    require(host.window_title() == "Dirty Scene *", "window title update failed");

    PlatformEvent resize;
    resize.type = EventType::WindowResized;
    resize.width = 1920;
    resize.height = 1080;
    host.push_event(resize);
    PlatformEvent key;
    key.type = EventType::KeyDown;
    key.key = "S";
    key.modifiers = Modifier::Control | Modifier::Shift;
    host.push_event(key);

    PlatformEvent received;
    require(host.poll_event(received) && received.type == EventType::WindowResized,
            "resize event was not delivered");
    require(host.window_metrics().logicalWidth == 1920, "resize did not update metrics");
    require(host.poll_event(received) && received.key == "S", "key event was not delivered");
    require(has_modifier(received.modifiers, Modifier::Control), "control modifier was lost");
    require(!host.poll_event(received), "event queue did not drain");

    FileDialogRequest request;
    request.kind = FileDialogKind::OpenFile;
    request.filters.push_back({"DVE scenes", {"dvescene"}});
    const FileDialogToken token = host.request_file_dialog(request);
    require(!host.take_file_dialog_result(token).has_value(), "pending dialog completed early");
    require(host.complete_file_dialog(token, {"scene.dvescene"}), "dialog completion failed");
    const auto result = host.take_file_dialog_result(token);
    require(result && result->accepted && result->paths.size() == 1U,
            "dialog result was not published");

    host.destroy_window();
    require(!host.has_window(), "destroy_window did not clear state");
}

void test_null_rhi_lifetime_and_copy() {
    using namespace dve::rhi;
    NullDevice device;
    require(device.capabilities().backend == Backend::Null, "wrong RHI backend");

    const BufferDesc uploadDesc{
        64U,
        BufferUsage::CopySource | BufferUsage::Storage,
        MemoryDomain::Upload,
        "upload"};
    const BufferDesc readbackDesc{
        64U,
        BufferUsage::CopyDestination | BufferUsage::Storage,
        MemoryDomain::Readback,
        "readback"};

    std::string error;
    const BufferHandle upload = device.create_buffer(uploadDesc, &error);
    const BufferHandle readback = device.create_buffer(readbackDesc, &error);
    require(upload && readback, error.c_str());

    std::array<std::byte, 16> source{};
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<std::byte>(index * 7U + 3U);
    require(device.write_buffer(upload, 8U, source, &error), error.c_str());

    const CommandListHandle copy = device.begin_commands(QueueKind::Copy, "copy test", &error);
    require(static_cast<bool>(copy), error.c_str());
    require(device.copy_buffer(copy, upload, 8U, readback, 24U, source.size(), &error), error.c_str());
    const FenceHandle copyFence = device.submit(copy, &error);
    require(copyFence && device.fence_complete(copyFence), error.c_str());

    std::array<std::byte, 16> destination{};
    require(device.read_buffer(readback, 24U, destination, &error), error.c_str());
    require(destination == source, "null RHI copy differed from source bytes");

    ComputePipelineDesc pipelineDesc;
    pipelineDesc.debugName = "primary brickmap trace contract";
    pipelineDesc.threadsX = 8U;
    pipelineDesc.threadsY = 8U;
    pipelineDesc.threadsZ = 1U;
    const ComputePipelineHandle pipeline = device.create_compute_pipeline(pipelineDesc, &error);
    require(static_cast<bool>(pipeline), error.c_str());
    const CommandListHandle compute = device.begin_commands(QueueKind::Compute, "dispatch test", &error);
    require(device.dispatch(compute, pipeline, 4U, 2U, 1U, &error), error.c_str());
    require(static_cast<bool>(device.submit(compute, &error)), error.c_str());

    require(device.destroy_buffer(upload, &error), error.c_str());
    require(!device.write_buffer(upload, 0U, source, &error), "stale buffer handle was accepted");
    require(error.find("stale") != std::string::npos, "stale handle failure was not diagnostic");
    require(device.destroy_buffer(readback, &error), error.c_str());
    require(device.destroy_compute_pipeline(pipeline, &error), error.c_str());

    const DeviceStatistics stats = device.statistics();
    require(stats.buffersCreated == 2U && stats.buffersDestroyed == 2U,
            "RHI lifetime counters are wrong");
    require(stats.copiesExecuted == 1U && stats.dispatchesExecuted == 1U,
            "RHI command counters are wrong");
}


void test_null_rhi_bind_group_contract() {
    using namespace dve::rhi;
    NullDevice device;
    std::string error;

    const BufferHandle constants = device.create_buffer({
        256U, BufferUsage::Constant | BufferUsage::CopyDestination,
        MemoryDomain::Upload, "frame constants"}, &error);
    const BufferHandle storage = device.create_buffer({
        1024U, BufferUsage::Storage | BufferUsage::CopyDestination,
        MemoryDomain::DeviceLocal, "brick records"}, &error);
    require(static_cast<bool>(constants) && static_cast<bool>(storage), error.c_str());

    TextureDesc textureDesc;
    textureDesc.width = 64U;
    textureDesc.height = 64U;
    textureDesc.usage = TextureUsage::Sampled | TextureUsage::Storage;
    textureDesc.debugName = "material atlas";
    const TextureHandle texture = device.create_texture(textureDesc, &error);
    require(static_cast<bool>(texture), error.c_str());
    TextureViewDesc viewDesc;
    viewDesc.texture = texture;
    viewDesc.debugName = "material atlas view";
    const TextureViewHandle view = device.create_texture_view(viewDesc, &error);
    require(static_cast<bool>(view), error.c_str());
    SamplerDesc samplerDesc;
    samplerDesc.addressU = AddressMode::ClampToEdge;
    samplerDesc.addressV = AddressMode::ClampToEdge;
    samplerDesc.debugName = "material atlas sampler";
    const SamplerHandle sampler = device.create_sampler(samplerDesc, &error);
    require(static_cast<bool>(sampler), error.c_str());

    BindGroupLayoutDesc layoutDesc;
    layoutDesc.debugName = "primary tracer resources";
    layoutDesc.bindings = {
        {0U, BindingType::UniformBuffer, ShaderStage::Compute},
        {1U, BindingType::StorageBufferReadOnly, ShaderStage::Compute},
        {2U, BindingType::SampledTexture, ShaderStage::Compute}};
    const BindGroupLayoutHandle layout = device.create_bind_group_layout(layoutDesc, &error);
    require(static_cast<bool>(layout), error.c_str());

    BindGroupDesc groupDesc;
    groupDesc.layout = layout;
    groupDesc.debugName = "primary tracer frame resources";
    groupDesc.entries = {
        {0U, constants, {}, 0U, 256U},
        {1U, storage, {}, 128U, 512U},
        {2U, {}, view, 0U, 0U, sampler}};
    const BindGroupHandle group = device.create_bind_group(groupDesc, &error);
    require(static_cast<bool>(group), error.c_str());

    require(!device.destroy_buffer(storage, &error), "bind group did not retain its storage buffer");
    require(error.find("live bind group") != std::string::npos,
            "retained-buffer failure was not diagnostic");
    require(!device.destroy_texture_view(view, &error), "bind group did not retain its texture view");
    require(!device.destroy_sampler(sampler, &error), "bind group did not retain its sampler");
    require(!device.destroy_bind_group_layout(layout, &error),
            "bind group did not retain its layout");

    require(device.destroy_bind_group(group, &error), error.c_str());
    require(device.destroy_bind_group_layout(layout, &error), error.c_str());
    require(device.destroy_sampler(sampler, &error), error.c_str());
    require(device.destroy_texture_view(view, &error), error.c_str());
    require(device.destroy_texture(texture, &error), error.c_str());
    require(device.destroy_buffer(constants, &error), error.c_str());
    require(device.destroy_buffer(storage, &error), error.c_str());

    const DeviceStatistics stats = device.statistics();
    require(stats.bindGroupLayoutsCreated == 1U && stats.bindGroupLayoutsDestroyed == 1U,
            "bind-group-layout counters are wrong");
    require(stats.bindGroupsCreated == 1U && stats.bindGroupsDestroyed == 1U,
            "bind-group counters are wrong");
    require(stats.samplersCreated == 1U && stats.samplersDestroyed == 1U,
            "sampler counters are wrong");
}

void test_null_rhi_presentation_and_device_loss() {
    using namespace dve::platform;
    using namespace dve::rhi;

    NullDevice device;
    std::string error;

    TextureDesc textureDesc;
    textureDesc.format = TextureFormat::RGBA8Unorm;
    textureDesc.width = 64U;
    textureDesc.height = 32U;
    textureDesc.usage = TextureUsage::Sampled | TextureUsage::Storage;
    textureDesc.initialState = ResourceState::ShaderRead;
    textureDesc.debugName = "view lifetime texture";
    const TextureHandle texture = device.create_texture(textureDesc, &error);
    require(static_cast<bool>(texture), error.c_str());

    TextureViewDesc viewDesc;
    viewDesc.texture = texture;
    viewDesc.debugName = "full texture view";
    const TextureViewHandle view = device.create_texture_view(viewDesc, &error);
    require(static_cast<bool>(view), error.c_str());
    require(!device.destroy_texture(texture, &error), "texture with a live view was destroyed");
    require(error.find("live view") != std::string::npos,
            "texture/view lifetime failure was not diagnostic");
    require(device.destroy_texture_view(view, &error), error.c_str());
    require(device.destroy_texture(texture, &error), error.c_str());

    SwapchainDesc swapchainDesc;
    swapchainDesc.window = {HostBackend::Headless, nullptr, nullptr};
    swapchainDesc.width = 1280U;
    swapchainDesc.height = 720U;
    swapchainDesc.imageCount = 3U;
    swapchainDesc.presentMode = PresentMode::Fifo;
    swapchainDesc.debugName = "null presentation contract";
    const SwapchainHandle swapchain = device.create_swapchain(swapchainDesc, &error);
    require(static_cast<bool>(swapchain), error.c_str());

    const AcquiredSwapchainImage acquired = device.acquire_next_image(swapchain, &error);
    require(static_cast<bool>(acquired.texture), error.c_str());
    require(acquired.imageIndex < swapchainDesc.imageCount, "invalid acquired swapchain image index");

    const TimestampQueryPoolHandle timestamps =
        device.create_timestamp_query_pool(2U, "frame timestamps", &error);
    require(static_cast<bool>(timestamps), error.c_str());
    const CommandListHandle frame = device.begin_commands(QueueKind::Graphics, "frame", &error);
    require(static_cast<bool>(frame), error.c_str());
    require(device.begin_debug_label(frame, "DVE frame", &error), error.c_str());
    require(device.write_timestamp(frame, timestamps, 0U, &error), error.c_str());
    require(device.transition_texture(frame, acquired.texture, ResourceState::Present,
                                      ResourceState::RenderTarget, &error), error.c_str());
    require(device.transition_texture(frame, acquired.texture, ResourceState::RenderTarget,
                                      ResourceState::Present, &error), error.c_str());
    require(device.write_timestamp(frame, timestamps, 1U, &error), error.c_str());
    require(device.end_debug_label(frame, &error), error.c_str());
    const FenceHandle frameFence = device.submit(frame, &error);
    require(frameFence && device.fence_complete(frameFence), error.c_str());

    std::array<std::uint64_t, 2> resolved{};
    require(device.resolve_timestamps(timestamps, 0U, resolved, &error), error.c_str());
    require(resolved[1] > resolved[0], "timestamp sequence was not monotonic");
    require(device.present(swapchain, frameFence, &error) == PresentResult::Presented,
            error.c_str());
    require(device.destroy_timestamp_query_pool(timestamps, &error), error.c_str());
    require(device.destroy_swapchain(swapchain, &error), error.c_str());

    const DeviceStatistics beforeLoss = device.statistics();
    require(beforeLoss.texturesCreated == 4U && beforeLoss.texturesDestroyed == 4U,
            "texture lifetime counters do not include swapchain images");
    require(beforeLoss.textureViewsCreated == 1U && beforeLoss.textureViewsDestroyed == 1U,
            "texture-view counters are wrong");
    require(beforeLoss.swapchainsCreated == 1U && beforeLoss.swapchainsDestroyed == 1U &&
            beforeLoss.presents == 1U, "swapchain counters are wrong");
    require(beforeLoss.barriersExecuted == 2U, "texture barriers were not executed");

    device.simulate_device_loss("test adapter removal");
    require(device.status() == DeviceStatus::Lost, "device-loss status was not published");
    require(device.device_loss_reason() == "test adapter removal", "device-loss reason was lost");
    error.clear();
    require(!device.create_buffer({16U, BufferUsage::Storage, MemoryDomain::DeviceLocal,
                                   "after loss"}, &error),
            "resource creation succeeded after device loss");
    require(error.find("adapter removal") != std::string::npos,
            "device-loss failure did not include the backend reason");
}

void test_brickmap_rhi_publication() {
    using namespace dve;
    using namespace dve::rhi;
    using namespace dve::render;

    VoxelObject object(77U);
    object.fill_brick({0, 0, 0}, 3U);
    object.set_voxel({8, 0, 0}, 5U);
    object.set_voxel({9, 0, 0}, 6U);

    PackedBrickmapScene scene;
    scene.rebuild(object);
    require(scene.validate_against(object), "packed brickmap source scene is invalid");

    NullDevice device;
    PackedBrickmapRhiMirror mirror(device);
    std::string error;
    require(mirror.upload(scene, &error), error.c_str());
    require(mirror.readback_matches(scene, &error), error.c_str());
    require(mirror.stats().publications == 1U, "brickmap publication counter is wrong");
    require(mirror.stats().reallocations == 3U, "brickmap did not allocate three logical buffers");

    object.set_voxel({10, 0, 0}, 7U);
    scene.rebuild(object);
    require(mirror.upload(scene, &error), error.c_str());
    require(mirror.readback_matches(scene, &error), error.c_str());
    require(mirror.stats().publications == 2U, "second brickmap publication was not recorded");
}

} // namespace

int main() {
    try {
        test_headless_host_contract();
        test_null_rhi_lifetime_and_copy();
        test_null_rhi_bind_group_contract();
        test_null_rhi_presentation_and_device_loss();
        test_brickmap_rhi_publication();
        std::cout << "platform/RHI contract tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "platform/RHI contract tests failed: " << exception.what() << '\n';
        return 1;
    }
}
