#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "dve/render/camera_frame_graph.hpp"
#include "dve/rhi/null_device.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::render;
using namespace dve::rhi;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

CameraViewportFrame make_frame(CameraViewportId id, float x, float width,
                               std::string target, Float3 position,
                               std::uint32_t cutGeneration = 0U) {
    CameraViewportFrame frame;
    frame.viewport = {id, "Viewport", 1U, x, 0.0F, width, 1.0F, std::move(target), true};
    frame.pose.position = position;
    frame.pose.target = {0.0F, 0.0F, 0.0F};
    frame.pose.lens.aspectRatio = width * 16.0F / 9.0F;
    frame.postProcess.depthOfFieldWeight = 1.0F;
    frame.postProcess.focusDistanceMeters = 5.0F;
    frame.gpu = pack_camera_gpu_packet(frame.viewport, frame.pose, frame.postProcess,
                                       cutGeneration, cutGeneration != 0U);
    return frame;
}

void test_frame_plan_upload_and_resets() {
    CameraFrameGraph graph;
    graph.begin_frame(1280U, 720U);
    std::string error;
    require(graph.submit(make_frame(1U, 0.0F, 0.5F, "main", {0, 1, 5}), 0U, 0U, &error),
            error.c_str());
    require(graph.submit(make_frame(2U, 0.5F, 0.5F, "main", {3, 1, 5}), 0U, 0U, &error),
            error.c_str());
    auto first = graph.finalize();
    require(first.viewports.size() == 2U && first.targets.size() == 1U,
            "split-screen frame plan is incomplete");
    require(first.viewports[0].pixelWidth == 640U && first.viewports[1].pixelX == 640U,
            "split-screen pixel rectangles are wrong");
    require(first.packetStride % 256U == 0U && first.packetUpload.size() == first.packetStride * 2U,
            "camera packet upload alignment is wrong");
    require(first.viewports[0].history.resetTaa && first.viewports[0].history.resetExposure,
            "first camera frame did not reset temporal history");

    NullDevice device;
    const auto buffer = device.create_buffer({first.packetUpload.size(),
        BufferUsage::Constant | BufferUsage::CopyDestination,
        MemoryDomain::Upload, "camera packets", ResourceState::ShaderRead}, &error);
    require(static_cast<bool>(buffer), error.c_str());
    require(graph.upload_packets(device, buffer, first, &error), error.c_str());
    std::vector<std::byte> readback(first.packetUpload.size());
    require(device.read_buffer(buffer, 0U, readback, &error), error.c_str());
    require(readback == first.packetUpload, "camera GPU packet upload changed bytes");

    graph.begin_frame(1280U, 720U);
    require(graph.submit(make_frame(1U, 0.0F, 0.5F, "main", {0.1F, 1, 5}), 0U, 0U, &error),
            error.c_str());
    auto stable = graph.finalize();
    require(!stable.viewports[0].history.resetTaa,
            "small camera motion incorrectly reset temporal history");

    graph.begin_frame(1280U, 720U);
    require(graph.submit(make_frame(1U, 0.0F, 0.5F, "main", {30, 1, 5}), 0U, 0U, &error),
            error.c_str());
    auto teleported = graph.finalize();
    require(any(teleported.viewports[0].history.reasons & CameraTemporalResetReason::Teleport),
            "camera teleport was not detected");
    require(teleported.viewports[0].history.resetMotionVectors &&
            teleported.viewports[0].history.resetOcclusion,
            "teleport did not reset dependent histories");
}

void test_depth_of_field_and_composition() {
    CameraFloatImage image;
    image.width = 64U;
    image.height = 64U;
    image.rgba.assign(64U * 64U * 4U, 0.0F);
    image.linearDepthMeters.assign(64U * 64U, 20.0F);
    for (std::size_t pixel = 0; pixel < 64U * 64U; ++pixel) image.rgba[pixel * 4U + 3U] = 1.0F;
    const std::size_t center = 32U * 64U + 32U;
    image.rgba[center * 4U + 0U] = 1.0F;
    image.linearDepthMeters[center] = 5.0F;

    CameraPose pose;
    pose.lens.physical.enabled = true;
    pose.lens.physical.focalLengthMillimeters = 85.0F;
    pose.lens.physical.sensorHeightMillimeters = 24.0F;
    pose.lens.physical.focusDistanceMeters = 5.0F;
    pose.lens.physical.apertureFStop = 1.4F;
    CameraPostProcessProfile post;
    post.depthOfFieldWeight = 1.0F;
    post.focusDistanceMeters = 5.0F;
    post.apertureFStop = 1.4F;
    const auto telemetry = apply_camera_depth_of_field(image, pose, post,
        {CameraDofQuality::High, 8.0F, 0.1F, false, true});
    require(telemetry.blurredPixels > 100U && telemetry.focusedPixels >= 1U,
            "depth-of-field classifier did not separate focused and blurred pixels");
    require(telemetry.maximumCircleOfConfusionPixels >= 1.0F,
            "depth-of-field blur radius was unexpectedly zero");
    require(image.rgba[center * 4U] > 0.5F,
            "focused foreground point was blurred away");

    CameraFloatImage destination;
    destination.width = 128U;
    destination.height = 64U;
    destination.rgba.assign(128U * 64U * 4U, 0.0F);
    destination.linearDepthMeters.assign(128U * 64U, INFINITY);
    CameraViewportDesc right{3U, "Right", 1U, 0.5F, 0.0F, 0.5F, 1.0F, "main", true};
    std::string error;
    require(composite_camera_viewport(destination, image, right, &error), error.c_str());
    const std::size_t copiedCenter = 32U * 128U + 96U;
    require(destination.rgba[copiedCenter * 4U] > 0.4F,
            "camera viewport compositor did not place the source in the right half");
    require(destination.rgba[(32U * 128U + 32U) * 4U] == 0.0F,
            "camera viewport compositor overwrote the left half");
    require(camera_image_to_srgb8(destination).size() == 128U * 64U * 4U,
            "camera image conversion returned the wrong size");
}

} // namespace

int main() {
    try {
        test_frame_plan_upload_and_resets();
        test_depth_of_field_and_composition();
        std::cout << "dve_camera_frame_graph_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_camera_frame_graph_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
