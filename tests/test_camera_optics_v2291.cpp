#include "dve/camera_runtime.hpp"
#include "dve/camera_system.hpp"
#include "dve/render/camera_frame_graph.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace dve {
class GameWorld;
} // namespace dve

using namespace dve;
using namespace dve::camera;
using namespace dve::render;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    } else {
        std::printf("  pass  %s\n", what.c_str());
    }
}

CameraFloatImage make_image(std::uint32_t width, std::uint32_t height, float depth, float value) {
    CameraFloatImage image;
    image.width = width;
    image.height = height;
    image.rgba.assign(static_cast<std::size_t>(width) * height * 4U, value);
    image.linearDepthMeters.assign(static_cast<std::size_t>(width) * height, depth);
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 1.0F;
    return image;
}

CameraPose physical_pose(float focalMm, float fStop) {
    CameraPose pose;
    pose.position = {0.0F, 0.0F, 0.0F};
    pose.target = {0.0F, 0.0F, -1.0F};
    pose.lens.physical.enabled = true;
    pose.lens.physical.focalLengthMillimeters = focalMm;
    pose.lens.physical.apertureFStop = fStop;
    pose.lens.aspectRatio = 16.0F / 9.0F;
    return pose;
}

// ---------------------------------------------------------------------------------------------
// A1 / A4: anamorphic bokeh orientation and cat's eye clipping, observed through the rendered blur.
void test_bokeh_shape() {
    std::printf("bokeh shape\n");
    // A single bright pixel on a defocused plane; the blur it leaves shows the aperture shape.
    const auto render = [](float anamorphicRatio) {
        CameraFloatImage image = make_image(201U, 201U, 12.0F, 0.0F);
        const std::size_t centre = (100U * 201U + 100U) * 4U;
        image.rgba[centre] = image.rgba[centre + 1U] = image.rgba[centre + 2U] = 100.0F;
        CameraPostProcessProfile post;
        post.depthOfFieldWeight = 1.0F;
        post.focusDistanceMeters = 2.0F;
        post.apertureFStop = 1.4F;
        post.cinematic.bokeh.bladeCount = 0U;
        post.cinematic.bokeh.anamorphicRatio = anamorphicRatio;
        CameraDofSettings settings;
        settings.quality = CameraDofQuality::High;
        settings.maximumBlurRadiusFraction = 20.0F / 81.0F;
        (void)apply_camera_depth_of_field(image, physical_pose(85.0F, 1.4F), post, settings);
        float width = 0.0F, height = 0.0F;
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * image.width + x) * 4U;
                if (image.rgba[p] > 1.0e-4F) {
                    width = std::max(width, std::abs(static_cast<float>(x) - 100.0F));
                    height = std::max(height, std::abs(static_cast<float>(y) - 100.0F));
                }
            }
        return std::make_pair(width, height);
    };
    const auto round = render(1.0F);
    const auto anamorphic = render(2.0F);
    check(std::abs(round.first - round.second) <= 1.0F,
          "ratio 1.0 leaves a round highlight");
    // sqrt split: semi-axes go as sqrt(ratio) and 1/sqrt(ratio), so 2.0 predicts height/width
    // near 2.0. Assert comfortably inside that rather than an absolute pixel margin, which the
    // previous 81 px image could not resolve.
    check(anamorphic.second > anamorphic.first * 1.4F,
          "ratio 2.0 leaves a vertically elongated highlight (was horizontal)");
    const auto squeezed = render(0.5F);
    check(squeezed.first > squeezed.second * 1.4F,
          "ratio 0.5 elongates horizontally, so the control is not one-sided");
}

// ---------------------------------------------------------------------------------------------
// A3: the split diopter seam must never be sharper than either half.
void test_split_diopter_seam() {
    std::printf("split diopter\n");
    CameraPostProcessProfile post;
    post.depthOfFieldWeight = 1.0F;
    post.apertureFStop = 2.0F;
    post.focusDistanceMeters = 6.0F;
    post.cinematic.splitDiopter.enabled = true;
    post.cinematic.splitDiopter.nearFocusDistanceMeters = 1.2F;
    post.cinematic.splitDiopter.farFocusDistanceMeters = 12.0F;
    post.cinematic.splitDiopter.featherFraction = 0.25F;
    const CameraPose pose = physical_pose(50.0F, 2.0F);
    CameraDofSettings settings;
    settings.quality = CameraDofQuality::High;

    // A subject sitting between the two focus planes is the case that used to snap into focus
    // exactly on the seam.
    const float intermediateDepth = 4.0F;
    float nearHalf = 0.0F, seam = 0.0F, farHalf = 0.0F;
    for (int column = 0; column < 3; ++column) {
        CameraFloatImage image = make_image(101U, 21U, intermediateDepth, 0.0F);
        const std::uint32_t x = column == 0 ? 5U : (column == 1 ? 50U : 95U);
        const std::size_t p = (10U * 101U + x) * 4U;
        image.rgba[p] = image.rgba[p + 1U] = image.rgba[p + 2U] = 50.0F;
        const auto telemetry = apply_camera_depth_of_field(image, pose, post, settings);
        const float retained = image.rgba[p];
        if (column == 0) nearHalf = retained;
        else if (column == 1) seam = retained;
        else farHalf = retained;
        (void)telemetry;
    }
    // Lower retained energy at the source pixel means more of it was spread out, i.e. blurrier.
    check(seam <= nearHalf + 1.0e-4F && seam <= farHalf + 1.0e-4F,
          "seam is at least as blurred as both halves (was the sharpest point in frame)");
}

// ---------------------------------------------------------------------------------------------
// B6: out of focus foreground has to spread over a sharp background.
void test_foreground_spread() {
    std::printf("foreground spread\n");
    CameraFloatImage image = make_image(81U, 41U, 20.0F, 0.05F);
    // Left half is a near, heavily defocused occluder; right half is the sharp background.
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < 40U; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * image.width + x;
            image.linearDepthMeters[p] = 0.6F;
            image.rgba[p * 4U] = image.rgba[p * 4U + 1U] = image.rgba[p * 4U + 2U] = 4.0F;
        }
    CameraPostProcessProfile post;
    post.depthOfFieldWeight = 1.0F;
    post.focusDistanceMeters = 20.0F;
    post.apertureFStop = 1.4F;
    CameraDofSettings settings;
    settings.quality = CameraDofQuality::High;
    settings.maximumBlurRadiusFraction = 16.0F / 41.0F;
    const std::size_t justInside = (20U * 81U + 43U) * 4U; // background side of the edge
    const float before = image.rgba[justInside];
    (void)apply_camera_depth_of_field(image, physical_pose(85.0F, 1.4F), post, settings);
    check(image.rgba[justInside] > before + 0.05F,
          "blurred foreground bleeds onto the sharp background across the edge");
}

// ---------------------------------------------------------------------------------------------
// B7: gate fit has to change the circle of confusion, because it changes the field of view.
void test_gate_fit() {
    std::printf("gate fit\n");
    const auto blurred = [](CameraGateFit fit) {
        CameraFloatImage image = make_image(64U, 36U, 3.0F, 0.0F);
        const std::size_t p = (18U * 64U + 32U) * 4U;
        image.rgba[p] = image.rgba[p + 1U] = image.rgba[p + 2U] = 40.0F;
        CameraPose pose = physical_pose(50.0F, 1.4F);
        pose.lens.physical.sensorWidthMillimeters = 36.0F;
        pose.lens.physical.sensorHeightMillimeters = 24.0F;
        pose.lens.physical.gateFit = fit;
        CameraPostProcessProfile post;
        post.depthOfFieldWeight = 1.0F;
        post.focusDistanceMeters = 20.0F;
        post.apertureFStop = 1.4F;
        CameraDofSettings settings;
        settings.quality = CameraDofQuality::High;
        return apply_camera_depth_of_field(image, pose, post, settings)
            .maximumCircleOfConfusionPixels;
    };
    const float vertical = blurred(CameraGateFit::Vertical);
    const float horizontal = blurred(CameraGateFit::Horizontal);
    check(vertical > 0.0F && horizontal > 0.0F, "both gate fits produce blur");
    check(std::abs(vertical - horizontal) > 1.0e-3F,
          "gate fit changes the circle of confusion (was ignored entirely)");
}

// ---------------------------------------------------------------------------------------------
// Blending: focus in diopters, aperture in stops, temperature in mireds, blade count stays valid.
void test_blending() {
    std::printf("blending\n");
    CameraPostProcessProfile from, to;
    from.focusDistanceMeters = 1.0F;   to.focusDistanceMeters = 100.0F;
    from.apertureFStop = 2.0F;         to.apertureFStop = 32.0F;
    from.cinematic.colorGrade.temperatureKelvin = 3200.0F;
    to.cinematic.colorGrade.temperatureKelvin = 6500.0F;
    from.cinematic.bokeh.bladeCount = 5U;
    to.cinematic.bokeh.bladeCount = 12U;
    const auto middle = blend_camera_post_process(from, to, 0.5F);
    check(std::abs(middle.focusDistanceMeters - 1.9802F) < 0.01F,
          "focus midpoint is diopter linear (1.98 m, was 50.5 m)");
    check(std::abs(middle.apertureFStop - 8.0F) < 0.01F,
          "aperture midpoint is one stop scale (f/8, was f/17)");
    const float mired = 1.0e6F / middle.cinematic.colorGrade.temperatureKelvin;
    check(std::abs(mired - 0.5F * (1.0e6F / 3200.0F + 1.0e6F / 6500.0F)) < 1.0F,
          "colour temperature midpoint is mired linear");

    std::string error;
    for (int step = 0; step <= 20; ++step) {
        const auto blended = blend_camera_post_process(from, to,
            static_cast<float>(step) / 20.0F);
        if (!blended.validate(&error)) {
            check(false, "blended profile stays valid at weight " +
                         std::to_string(static_cast<float>(step) / 20.0F) + ": " + error);
            return;
        }
    }
    check(true, "blended profile validates across the whole blend (blade count never hits 1 or 2)");
}

// ---------------------------------------------------------------------------------------------
// A2: the anamorphic squeeze widens the horizontal frustum instead of magnifying in post.
void test_anamorphic_projection() {
    std::printf("anamorphic squeeze (2026-07-25 correction)\n");
    // The squeeze must not reach the frustum. Multiplying the raster aspect by it renders every
    // circle with a horizontal-to-vertical semi-axis ratio of 1/squeeze, which is a squeezed
    // negative delivered to a display with nothing to undo it.
    CameraLens lens;
    lens.aspectRatio = 2.360F;
    lens.verticalFieldOfViewRadians = 26.18F * kDegreesToRadians;
    const auto projection = camera_projection_matrix(lens);
    // Square pixels: a world circle at any depth must render circular. Semi-axis ratio is
    // (W/2)*m00 / ((H/2)*m11) with W/H = aspectRatio.
    const float axisRatio = lens.aspectRatio * projection.values[0] / projection.values[5];
    check(std::abs(axisRatio - 1.0F) < 1.0e-4F, "rendered circles stay circular (square pixels)");

    // The anamorphic field is carried by the delivery aspect instead.
    CameraPhysicalLens physical = camera_physical_lens_preset(CameraFilmbackPreset::Anamorphic35, 40.0F);
    const float aspect = camera_anamorphic_filmback_aspect_ratio(physical, 2.0F);
    check(std::abs(aspect - 2.360F) < 2.0e-3F, "anamorphic 35 at 2x delivers the 2.39 scope aspect");

    // and that aspect reproduces the real lens field: vertical from the sensor height, horizontal
    // from the raster aspect, both matching the taking lens to float precision.
    physical.gateFit = CameraGateFit::Vertical;
    const float vertical = physical.vertical_field_of_view_radians(aspect);
    const float horizontal = 2.0F * std::atan(aspect * std::tan(vertical * 0.5F));
    const float expectedHorizontal =
        2.0F * std::atan(2.0F * physical.sensorWidthMillimeters /
                         (2.0F * physical.focalLengthMillimeters));
    check(std::abs(vertical - 26.18F * kDegreesToRadians) < 1.0e-3F,
          "vertical field comes from sensor height and focal length");
    check(std::abs(horizontal - expectedHorizontal) < 1.0e-3F,
          "horizontal field matches the 2x anamorphic taking lens");
}

void test_field_of_view_blend() {
    std::printf("field of view blending (2026-07-25)\n");
    // blend_lens log-blends focalLengthMillimeters, so the free field of view has to use the same
    // curve or the same authored move behaves differently with the physical lens switched off.
    const float wide = 60.0F * kDegreesToRadians;
    const float tele = 10.0F * kDegreesToRadians;
    const float midpoint = blend_camera_vertical_fov(wide, tele, 0.5F);
    const float linear = wide + (tele - wide) * 0.5F;
    check(midpoint < linear - 5.0F * kDegreesToRadians,
          "midpoint is well below the linear-in-radians midpoint");
    // Equivalence with the focal length curve: fov and focal length are related by 1/tan(fov/2),
    // so a blend of one must agree with a blend of the other on the same filmback.
    const float sensorHeight = 24.0F;
    const auto focalFor = [&](float fov) { return sensorHeight / (2.0F * std::tan(fov * 0.5F)); };
    for (float t = 0.0F; t <= 1.0F; t += 0.25F) {
        const float viaFov = focalFor(blend_camera_vertical_fov(wide, tele, t));
        const float viaFocal = blend_camera_focal_length(focalFor(wide), focalFor(tele), t);
        check(std::abs(viaFov - viaFocal) < 1.0e-3F * std::max(1.0F, viaFocal),
              "field of view and focal length blends agree");
    }
    check(std::abs(blend_camera_vertical_fov(wide, tele, 0.0F) - wide) < 1.0e-5F, "blend endpoint at t=0");
    check(std::abs(blend_camera_vertical_fov(wide, tele, 1.0F) - tele) < 1.0e-5F, "blend endpoint at t=1");
}

void test_circle_of_confusion_default() {
    std::printf("format-relative circle of confusion (2026-07-25)\n");
    // The acceptable circle of confusion scales with the format. A fixed 0.03 mm is the full
    // frame 35 value and made the hyperfocal readout wrong for every other filmback.
    struct Case { CameraFilmbackPreset preset; float expectedCoc; };
    const Case cases[] = {
        {CameraFilmbackPreset::Super16, 0.0097F},
        {CameraFilmbackPreset::Super35, 0.0207F},
        {CameraFilmbackPreset::FullFrame35, 0.0288F},
        {CameraFilmbackPreset::Imax15Perf, 0.0586F},
    };
    for (const auto& item : cases) {
        auto lens = camera_physical_lens_preset(item.preset, 50.0F);
        lens.apertureFStop = 2.8F;
        lens.focusDistanceMeters = 5.0F;
        const auto derived = camera_depth_of_field_range(lens);
        const auto explicitCoc = camera_depth_of_field_range(lens, item.expectedCoc);
        check(std::abs(derived.hyperfocalDistanceMeters - explicitCoc.hyperfocalDistanceMeters) <
                  0.02F * explicitCoc.hyperfocalDistanceMeters,
              "default circle of confusion follows the filmback diagonal");
        check(derived.nearFocusMeters > 0.0F &&
                  derived.farFocusMeters > derived.nearFocusMeters,
              "depth of field range stays ordered");
    }
    // Super 16 is the sharpest constraint of the set, so it must give the longest hyperfocal.
    auto super16 = camera_physical_lens_preset(CameraFilmbackPreset::Super16, 50.0F);
    auto imax = camera_physical_lens_preset(CameraFilmbackPreset::Imax15Perf, 50.0F);
    super16.apertureFStop = imax.apertureFStop = 2.8F;
    check(camera_depth_of_field_range(super16).hyperfocalDistanceMeters >
              camera_depth_of_field_range(imax).hyperfocalDistanceMeters,
          "a smaller format demands a longer hyperfocal at the same focal length");
}

void test_validation() {
    std::printf("validation\n");
    CameraSplitDiopterSettings split;
    split.enabled = true;
    split.nearFocusDistanceMeters = 20.0F;
    split.farFocusDistanceMeters = 1.0F;
    std::string error;
    check(!split.validate(&error), "inverted split diopter near/far is rejected");
    split.nearFocusDistanceMeters = 1.0F;
    split.farFocusDistanceMeters = 20.0F;
    check(split.validate(&error), "ordered split diopter near/far is accepted");
}

// ---------------------------------------------------------------------------------------------
// Grading order: the LUT now runs after the tone curve, so it can no longer be fed clamped
// linear values in which every highlight collapses onto the white corner.
void test_highlight_separation() {
    std::printf("LUT applied after the tone curve\n");
    const std::uint32_t edge = 8U;
    CameraColorLut3D lut;
    lut.edgeSize = edge;
    lut.values.resize(static_cast<std::size_t>(edge) * edge * edge);
    for (std::uint32_t b = 0; b < edge; ++b)
        for (std::uint32_t g = 0; g < edge; ++g)
            for (std::uint32_t r = 0; r < edge; ++r)
                lut.values[r + edge * (g + edge * b)] = Float3{
                    static_cast<float>(r) / static_cast<float>(edge - 1U),
                    static_cast<float>(g) / static_cast<float>(edge - 1U),
                    static_cast<float>(b) / static_cast<float>(edge - 1U)};

    const float inputs[4] = {1.2F, 3.0F, 8.0F, 30.0F};
    const auto run = [&](float lutBlend, const CameraColorLut3D* table) {
        CameraFloatImage image = make_image(4U, 2U, 10.0F, 0.0F);
        for (std::uint32_t x = 0; x < 4U; ++x) {
            const std::size_t p = x * 4U;
            image.rgba[p] = image.rgba[p + 1U] = image.rgba[p + 2U] = inputs[x];
        }
        CameraPostProcessProfile post;
        // Reinhard rather than ACES: ACES legitimately saturates 8.0 and 30.0 to the same display
        // white, so it cannot show whether the LUT clamped them beforehand. Reinhard keeps all
        // four separable, which is what makes the clamping observable.
        post.cinematic.colorGrade.toneMap = CameraToneMapCurve::Reinhard;
        post.cinematic.colorGrade.lutBlend = lutBlend;
        CameraCinematicPipelineSettings settings;
        settings.applyDepthOfField = false;
        CameraPose pose;
        (void)apply_camera_cinematic_pipeline(image, pose, post, settings, table, nullptr);
        return image;
    };

    const auto graded = run(1.0F, &lut);
    bool separated = true;
    for (std::uint32_t x = 1; x < 4U; ++x)
        if (graded.rgba[x * 4U] <= graded.rgba[(x - 1U) * 4U] + 1.0e-5F) separated = false;
    check(separated,
          "distinct HDR highlights stay distinct (previously all clamped to LUT white)");

    // An identity LUT at full blend must be a no-op, which only holds if the lookup happens in the
    // display referred domain the table is authored against.
    const auto ungraded = run(0.0F, nullptr);
    float worst = 0.0F;
    for (std::uint32_t x = 0; x < 4U; ++x)
        worst = std::max(worst, std::abs(graded.rgba[x * 4U] - ungraded.rgba[x * 4U]));
    check(worst < 2.0e-2F, "identity LUT at full blend is a no-op after the tone curve");

    // and everything stays inside the display range.
    bool bounded = true;
    for (std::uint32_t x = 0; x < 4U; ++x)
        if (graded.rgba[x * 4U] < 0.0F || graded.rgba[x * 4U] > 1.0F + 1.0e-4F) bounded = false;
    check(bounded, "graded output stays within the display range");
}

// Focused verification of the v2.29 camera optics fixes. Links only the camera translation units
// plus stubs for the engine symbols camera_runtime.cpp refers to, so it can run without the full
// engine build.

} // namespace

// Engine symbols referenced by camera_runtime.cpp but not needed by these checks.
namespace dve {
struct Float3Stub {};
} // namespace dve

int main() {
    test_bokeh_shape();
    test_split_diopter_seam();
    test_foreground_spread();
    test_gate_fit();
    test_blending();
    test_anamorphic_projection();
    test_field_of_view_blend();
    test_circle_of_confusion_default();
    test_validation();
    test_highlight_separation();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
