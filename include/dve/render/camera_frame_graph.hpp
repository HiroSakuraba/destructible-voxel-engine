#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/camera_runtime.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

enum class CameraTemporalResetReason : std::uint32_t {
    NoReset = 0U,
    Cut = 1U << 0U,
    Teleport = 1U << 1U,
    OriginShift = 1U << 2U,
    ViewportResize = 1U << 3U,
    RenderTargetChange = 1U << 4U,
    Manual = 1U << 5U,
};
[[nodiscard]] constexpr CameraTemporalResetReason operator|(CameraTemporalResetReason a,
                                                            CameraTemporalResetReason b) noexcept {
    return static_cast<CameraTemporalResetReason>(static_cast<std::uint32_t>(a) |
                                                   static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr CameraTemporalResetReason operator&(CameraTemporalResetReason a,
                                                            CameraTemporalResetReason b) noexcept {
    return static_cast<CameraTemporalResetReason>(static_cast<std::uint32_t>(a) &
                                                   static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool any(CameraTemporalResetReason value) noexcept {
    return value != CameraTemporalResetReason::NoReset;
}

struct CameraTemporalHistoryFlags {
    bool resetTaa{};
    bool resetMotionVectors{};
    bool resetExposure{};
    bool resetOcclusion{};
    CameraTemporalResetReason reasons{CameraTemporalResetReason::NoReset};
};

struct CameraFrameGraphSettings {
    float teleportDistanceMeters{8.0F};
    float teleportAngleRadians{1.0471975512F};
    bool resetExposureOnCut{true};
    bool resetOcclusionOnCut{true};
    std::size_t packetAlignmentBytes{256U};
};

struct CameraRenderTargetPlan {
    std::string name;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<camera::CameraViewportId> viewports;
};

struct CameraViewportRenderPlan {
    camera::CameraViewportFrame frame{};
    std::uint32_t pixelX{};
    std::uint32_t pixelY{};
    std::uint32_t pixelWidth{};
    std::uint32_t pixelHeight{};
    std::size_t gpuPacketOffset{};
    CameraTemporalHistoryFlags history{};
};

struct CameraFramePlan {
    std::vector<CameraViewportRenderPlan> viewports;
    std::vector<CameraRenderTargetPlan> targets;
    std::vector<std::byte> packetUpload;
    std::size_t packetStride{};
};

class CameraFrameGraph {
public:
    explicit CameraFrameGraph(CameraFrameGraphSettings settings = {});

    void begin_frame(std::uint32_t mainWidth, std::uint32_t mainHeight,
                     Float3 worldOrigin = {}) noexcept;
    [[nodiscard]] bool submit(const camera::CameraViewportFrame& frame,
                              std::uint32_t targetWidth = 0U,
                              std::uint32_t targetHeight = 0U,
                              std::string* error = nullptr);
    void request_manual_reset(camera::CameraViewportId id) noexcept;
    [[nodiscard]] CameraFramePlan finalize();

    [[nodiscard]] bool upload_packets(rhi::IDevice& device, rhi::BufferHandle buffer,
                                      const CameraFramePlan& plan,
                                      std::string* error = nullptr) const;

private:
    struct PreviousViewport {
        camera::CameraPose pose{};
        std::uint32_t cutGeneration{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::string target;
        Float3 worldOrigin{};
        bool valid{};
    };

    CameraFrameGraphSettings settings_{};
    std::uint32_t mainWidth_{1U};
    std::uint32_t mainHeight_{1U};
    Float3 worldOrigin_{};
    std::vector<CameraViewportRenderPlan> pending_;
    std::map<camera::CameraViewportId, PreviousViewport> previous_;
    std::map<camera::CameraViewportId, bool> manualResets_;
};

enum class CameraDofQuality : std::uint8_t { Off, Low, Medium, High };

struct CameraDofSettings {
    CameraDofQuality quality{CameraDofQuality::Medium};
    // Fraction of image height, not an absolute pixel count. A fixed pixel cap made the depth of
    // field look resolution dependent: 85 mm at f/1.4 focused at 2 m with a subject at 10 m wants
    // a 48 px blur radius at 1080p and 97 px at 2160p, and both were clamped to the same 12 px.
    // The default reproduces the previous 12 px at 1080p.
    float maximumBlurRadiusFraction{12.0F / 1080.0F};
    float focusPlaneBandMeters{0.15F};
    bool focusPlanePreview{};
    bool preserveForegroundEdges{true};
    bool useCinematicBokeh{true};
    bool useSplitDiopter{true};
};

struct CameraFloatImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> rgba;              // linear-light RGBA, four floats per pixel
    std::vector<float> linearDepthMeters; // one float per pixel; +inf means background

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraDofTelemetry {
    std::uint64_t focusedPixels{};
    std::uint64_t blurredPixels{};
    float maximumCircleOfConfusionPixels{};
};

struct CameraColorLut3D {
    std::uint32_t edgeSize{};
    std::vector<Float3> values;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraMotionField {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> xyPixels;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct CameraCinematicPipelineSettings {
    CameraDofSettings depthOfField{};
    bool applyDepthOfField{true};
    bool applyLensDistortion{true};
    bool applyColorGrade{true};
    bool applyFilmEffects{true};
    bool applyFraming{true};
    std::uint32_t frameIndex{};
};

struct CameraCinematicTelemetry {
    CameraDofTelemetry depthOfField{};
    std::uint64_t distortedPixels{};
    std::uint64_t gradedPixels{};
    std::uint64_t mattePixels{};
    std::uint64_t motionBlurredPixels{};
    float maximumDistortionPixels{};
};

[[nodiscard]] CameraDofTelemetry apply_camera_depth_of_field(
    CameraFloatImage& image,
    const camera::CameraPose& pose,
    const camera::CameraPostProcessProfile& postProcess,
    const CameraDofSettings& settings = {});

[[nodiscard]] CameraCinematicTelemetry apply_camera_cinematic_pipeline(
    CameraFloatImage& image,
    const camera::CameraPose& pose,
    const camera::CameraPostProcessProfile& postProcess,
    const CameraCinematicPipelineSettings& settings = {},
    const CameraColorLut3D* colorLut = nullptr,
    const CameraMotionField* motionField = nullptr);

[[nodiscard]] std::optional<CameraColorLut3D> parse_camera_cube_lut(
    std::string_view text,
    std::string* error = nullptr);

[[nodiscard]] bool apply_camera_motion_blur(
    CameraFloatImage& image,
    const CameraMotionField& motionField,
    float weight,
    float shutterAngleDegrees,
    std::uint32_t maximumSamples = 16U,
    std::uint64_t* blurredPixels = nullptr,
    std::string* error = nullptr);

[[nodiscard]] bool composite_camera_viewport(
    CameraFloatImage& destination,
    const CameraFloatImage& source,
    const camera::CameraViewportDesc& viewport,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> camera_image_to_srgb8(const CameraFloatImage& image);

} // namespace dve::render
