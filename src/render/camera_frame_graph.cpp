#include "dve/render/camera_frame_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace dve::render {
namespace {
constexpr float kEpsilon = 1.0e-6F;

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    alignment = std::max<std::size_t>(1U, alignment);
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + (alignment - remainder);
}

Float3 normalized(Float3 value, Float3 fallback) noexcept {
    const float squared = length_squared(value);
    if (!(squared > 1.0e-12F) || !std::isfinite(squared)) return fallback;
    return multiply(value, 1.0F / std::sqrt(squared));
}

float pose_angle(const camera::CameraPose& a, const camera::CameraPose& b) noexcept {
    const Float3 af = normalized(subtract(a.target, a.position), {0.0F, 0.0F, -1.0F});
    const Float3 bf = normalized(subtract(b.target, b.position), {0.0F, 0.0F, -1.0F});
    return std::acos(std::clamp(dot(af, bf), -1.0F, 1.0F));
}

bool different_origin(Float3 a, Float3 b) noexcept {
    return length_squared(subtract(a, b)) > 1.0e-10F;
}

float srgb_encode(float linear) noexcept {
    linear = std::clamp(linear, 0.0F, 1.0F);
    return linear <= 0.0031308F ? linear * 12.92F
                               : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

// The sensor height that actually maps onto the rendered image. This has to agree with
// CameraPhysicalLens::vertical_field_of_view_radians, otherwise the blur does not match the
// rendered field of view for any gate fit other than Vertical.
float effective_sensor_height_millimeters(const camera::CameraPhysicalLens& physical,
                                          float viewportAspect) noexcept {
    if (!physical.enabled) return 24.0F;
    const float sensorHeight = std::max(0.001F, physical.sensorHeightMillimeters);
    const float fitted = std::max(0.001F, physical.sensorWidthMillimeters) /
                         std::max(0.01F, viewportAspect);
    switch (physical.gateFit) {
        case camera::CameraGateFit::Horizontal: return fitted;
        case camera::CameraGateFit::Fill: return std::min(sensorHeight, fitted);
        case camera::CameraGateFit::Overscan: return std::max(sensorHeight, fitted);
        case camera::CameraGateFit::Vertical:
        case camera::CameraGateFit::Stretch: break;
    }
    return sensorHeight;
}

float circle_of_confusion_pixels(float depthMeters,
                                 const camera::CameraPose& pose,
                                 const camera::CameraPostProcessProfile& post,
                                 std::uint32_t imageWidth,
                                 std::uint32_t imageHeight,
                                 float focusDistanceMeters) noexcept {
    if (!(depthMeters > 0.0F) || !std::isfinite(depthMeters) || imageHeight == 0U ||
        imageWidth == 0U) return 0.0F;
    const auto& physical = pose.lens.physical;
    const float focalMeters = std::max(0.001F,
        (physical.enabled ? physical.focalLengthMillimeters : 35.0F) * 0.001F);
    const float aperture = std::max(0.5F, post.apertureFStop);
    const float focus = std::max(focalMeters + 0.001F, focusDistanceMeters);
    const float apertureDiameter = focalMeters / aperture;
    const float denominator = std::max(kEpsilon, focus - focalMeters);
    const float blurDiameterMeters = std::abs(apertureDiameter * focalMeters *
        (depthMeters - focus) / (depthMeters * denominator));
    const float viewportAspect = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
    const float sensorHeightMeters =
        std::max(0.001F, effective_sensor_height_millimeters(physical, viewportAspect) * 0.001F);
    return 0.5F * blurDiameterMeters / sensorHeightMeters * static_cast<float>(imageHeight) *
           std::clamp(post.depthOfFieldWeight, 0.0F, 1.0F);
}

std::vector<std::pair<float, float>> sample_pattern(CameraDofQuality quality) {
    switch (quality) {
        case CameraDofQuality::Off: return {};
        case CameraDofQuality::Low:
            return {{-0.707F,-0.707F},{0.707F,-0.707F},{-0.707F,0.707F},{0.707F,0.707F}};
        case CameraDofQuality::Medium:
            return {{1,0},{-1,0},{0,1},{0,-1},{0.707F,0.707F},{-0.707F,0.707F},
                    {0.707F,-0.707F},{-0.707F,-0.707F}};
        case CameraDofQuality::High:
            return {{1,0},{-1,0},{0,1},{0,-1},{0.707F,0.707F},{-0.707F,0.707F},
                    {0.707F,-0.707F},{-0.707F,-0.707F},{0.9239F,0.3827F},{0.3827F,0.9239F},
                    {-0.3827F,0.9239F},{-0.9239F,0.3827F},{0.9239F,-0.3827F},
                    {0.3827F,-0.9239F},{-0.3827F,-0.9239F},{-0.9239F,-0.3827F}};
    }
    return {};
}

float smoothstep(float edge0,float edge1,float value) noexcept {
    if(!(edge1>edge0))return value>=edge1?1.0F:0.0F;
    const float t=std::clamp((value-edge0)/(edge1-edge0),0.0F,1.0F);
    return t*t*(3.0F-2.0F*t);
}

// A split diopter is a half lens cemented over one side of the front element. It produces two
// focus planes and a seam where both optical paths overlap and neither is in focus, so the seam is
// the blurriest region in frame. Interpolating the focus *distance* across the seam did the
// opposite: it swept focus through every intermediate distance and brought mid-distance subjects
// into focus exactly on the line. The circle of confusion is therefore blended instead, taking the
// larger of the two paths through the transition band.
float split_diopter_coc_pixels(float depthMeters,
                               const camera::CameraPose& pose,
                               const camera::CameraPostProcessProfile& post,
                               std::uint32_t x,std::uint32_t y,
                               std::uint32_t width,std::uint32_t height,
                               bool enabled) noexcept {
    const auto& split=post.cinematic.splitDiopter;
    const auto coc=[&](float focus){
        return circle_of_confusion_pixels(depthMeters,pose,post,width,height,
                                          std::max(0.001F,focus));
    };
    if(!enabled||!split.enabled||width==0U||height==0U)return coc(post.focusDistanceMeters);
    const float u=(static_cast<float>(x)+0.5F)/static_cast<float>(width)*2.0F-1.0F;
    const float v=(static_cast<float>(y)+0.5F)/static_cast<float>(height)*2.0F-1.0F;
    const float signedDistance=(u-split.centerX)*std::cos(split.angleRadians)+
                               (v-split.centerY)*std::sin(split.angleRadians);
    const float feather=std::max(1.0e-4F,split.featherFraction);
    const float blend=smoothstep(-feather,feather,signedDistance);
    const float nearCoc=coc(split.nearFocusDistanceMeters);
    const float farCoc=coc(split.farFocusDistanceMeters);
    const float seam=1.0F-std::abs(2.0F*blend-1.0F);
    const float interpolated=nearCoc+(farCoc-nearCoc)*blend;
    const float overlapped=std::max(nearCoc,farCoc);
    return interpolated+(overlapped-interpolated)*seam;
}

// Reported focus distance for the focus plane overlay only. The rendered blur no longer routes
// through a single distance, so this exists purely so the viewport guide can draw something.
float split_diopter_preview_focus(const camera::CameraPostProcessProfile& post,
                                  std::uint32_t x,std::uint32_t y,
                                  std::uint32_t width,std::uint32_t height,
                                  bool enabled) noexcept {
    const auto& split=post.cinematic.splitDiopter;
    if(!enabled||!split.enabled||width==0U||height==0U)return post.focusDistanceMeters;
    const float u=(static_cast<float>(x)+0.5F)/static_cast<float>(width)*2.0F-1.0F;
    const float v=(static_cast<float>(y)+0.5F)/static_cast<float>(height)*2.0F-1.0F;
    const float signedDistance=(u-split.centerX)*std::cos(split.angleRadians)+
                               (v-split.centerY)*std::sin(split.angleRadians);
    const float feather=std::max(1.0e-4F,split.featherFraction);
    return signedDistance<0.0F?split.nearFocusDistanceMeters:
           (smoothstep(-feather,feather,signedDistance)>=0.5F?split.farFocusDistanceMeters
                                                             :split.nearFocusDistanceMeters);
}

// Returns the aperture sample, or nullopt when the tap is clipped away by optical vignetting.
std::optional<std::pair<float,float>> shape_bokeh_sample(
    std::pair<float,float> sample,
    const camera::CameraBokehSettings& bokeh,
    float screenU,float screenV) noexcept {
    float x=sample.first;
    float y=sample.second;
    // The aperture polygon rotates with bladeRotation, so its boundary is evaluated in the
    // aperture's own frame. Sign matches shaders/cinematic_dof.hlsl.
    const float theta=std::atan2(y,x)-bokeh.bladeRotationRadians;
    if(bokeh.bladeCount>=3U){
        const float sides=static_cast<float>(bokeh.bladeCount);
        const float sector=2.0F*3.14159265358979323846F/sides;
        const float local=theta-std::floor(theta/sector+0.5F)*sector;
        const float polygonRadius=std::cos(3.14159265358979323846F/sides)/
            std::max(0.15F,std::cos(local));
        const float boundary=polygonRadius+(1.0F-polygonRadius)*
            std::clamp(bokeh.roundness,0.0F,1.0F);
        x*=boundary;y*=boundary;
    }
    // Anamorphic highlights are elongated vertically in the projected image, so the squeeze goes
    // on x and the stretch on y. The sqrt split keeps sampled disk area constant, so the ratio
    // changes bokeh shape without changing total blur energy.
    const float ratio=std::sqrt(std::max(0.25F,bokeh.anamorphicRatio));
    x/=ratio;y*=ratio;
    // Optical vignetting truncates the aperture toward frame centre into a lens shape and darkens
    // the corner. It does not shrink the blur circle isotropically, which is what the previous
    // scale factor did: it made corners sharper rather than lens shaped.
    const float catEye=std::clamp(bokeh.catEye,0.0F,1.0F);
    if(catEye>0.0F){
        const float screenLength=std::sqrt(screenU*screenU+screenV*screenV);
        const float edge=std::clamp(screenLength,0.0F,1.41421356F)/1.41421356F;
        if(screenLength>1.0e-5F){
            const float centreX=-screenU/screenLength*catEye*edge;
            const float centreY=-screenV/screenLength*catEye*edge;
            const float dx=x-centreX,dy=y-centreY;
            if(std::sqrt(dx*dx+dy*dy)>1.0F)return std::nullopt;
        }
    }
    return std::make_pair(x,y);
}

float deterministic_noise(std::uint32_t x,std::uint32_t y,std::uint32_t frame,std::uint32_t seed) noexcept {
    std::uint32_t value=x*0x1f123bb5U+y*0x5f356495U+frame*0x9e3779b9U+seed*0x85ebca6bU;
    value^=value>>16U;value*=0x7feb352dU;value^=value>>15U;value*=0x846ca68bU;value^=value>>16U;
    return static_cast<float>(value&0x00ffffffU)/16777215.0F;
}

float luminance(float r,float g,float b) noexcept{return r*0.2126F+g*0.7152F+b*0.0722F;}

float aces_filmic(float value) noexcept {
    const float a=2.51F,b=0.03F,c=2.43F,d=0.59F,e=0.14F;
    return std::clamp((value*(a*value+b))/(value*(c*value+d)+e),0.0F,1.0F);
}

float tone_map(float value,camera::CameraToneMapCurve curve) noexcept {
    value=std::max(0.0F,value);
    switch(curve){
        case camera::CameraToneMapCurve::Linear:return std::clamp(value,0.0F,1.0F);
        case camera::CameraToneMapCurve::Reinhard:return value/(1.0F+value);
        case camera::CameraToneMapCurve::AcesFilmic:return aces_filmic(value);
    }
    return std::clamp(value,0.0F,1.0F);
}

std::array<float,4> sample_rgba_bilinear(const std::vector<float>& rgba,std::uint32_t width,
                                         std::uint32_t height,float x,float y) noexcept {
    if(width==0U||height==0U||x<-1.0F||y<-1.0F||x>static_cast<float>(width)||
       y>static_cast<float>(height))return {0.0F,0.0F,0.0F,0.0F};
    x=std::clamp(x,0.0F,static_cast<float>(width-1U));
    y=std::clamp(y,0.0F,static_cast<float>(height-1U));
    const auto x0=static_cast<std::uint32_t>(std::floor(x));
    const auto y0=static_cast<std::uint32_t>(std::floor(y));
    const auto x1=std::min(width-1U,x0+1U),y1=std::min(height-1U,y0+1U);
    const float tx=x-static_cast<float>(x0),ty=y-static_cast<float>(y0);
    std::array<float,4> out{};
    for(std::size_t channel=0;channel<4U;++channel){
        const auto at=[&](std::uint32_t sx,std::uint32_t sy){return rgba[(static_cast<std::size_t>(sy)*width+sx)*4U+channel];};
        const float top=at(x0,y0)+(at(x1,y0)-at(x0,y0))*tx;
        const float bottom=at(x0,y1)+(at(x1,y1)-at(x0,y1))*tx;
        out[channel]=top+(bottom-top)*ty;
    }
    return out;
}

float sample_depth_nearest(const std::vector<float>& depth,std::uint32_t width,std::uint32_t height,
                           float x,float y) noexcept {
    if(width==0U||height==0U||x<0.0F||y<0.0F||x>static_cast<float>(width-1U)||
       y>static_cast<float>(height-1U))return std::numeric_limits<float>::infinity();
    const auto sx=static_cast<std::uint32_t>(std::lround(x));
    const auto sy=static_cast<std::uint32_t>(std::lround(y));
    return depth[static_cast<std::size_t>(sy)*width+sx];
}

Float3 sample_lut(const CameraColorLut3D& lut,Float3 color) noexcept {
    const float edge=static_cast<float>(lut.edgeSize-1U);
    const float rx=std::clamp(color.x,0.0F,1.0F)*edge;
    const float gy=std::clamp(color.y,0.0F,1.0F)*edge;
    const float bz=std::clamp(color.z,0.0F,1.0F)*edge;
    const auto r0=static_cast<std::uint32_t>(std::floor(rx));const auto r1=std::min(lut.edgeSize-1U,r0+1U);
    const auto g0=static_cast<std::uint32_t>(std::floor(gy));const auto g1=std::min(lut.edgeSize-1U,g0+1U);
    const auto b0=static_cast<std::uint32_t>(std::floor(bz));const auto b1=std::min(lut.edgeSize-1U,b0+1U);
    const float tr=rx-static_cast<float>(r0),tg=gy-static_cast<float>(g0),tb=bz-static_cast<float>(b0);
    const auto at=[&](std::uint32_t r,std::uint32_t g,std::uint32_t b){return lut.values[r+lut.edgeSize*(g+lut.edgeSize*b)];};
    const auto mix3=[](Float3 a,Float3 b,float t){return Float3{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};};
    const Float3 c00=mix3(at(r0,g0,b0),at(r1,g0,b0),tr);
    const Float3 c10=mix3(at(r0,g1,b0),at(r1,g1,b0),tr);
    const Float3 c01=mix3(at(r0,g0,b1),at(r1,g0,b1),tr);
    const Float3 c11=mix3(at(r0,g1,b1),at(r1,g1,b1),tr);
    return mix3(mix3(c00,c10,tg),mix3(c01,c11,tg),tb);
}

} // namespace

CameraFrameGraph::CameraFrameGraph(CameraFrameGraphSettings settings) : settings_(settings) {
    settings_.packetAlignmentBytes = std::max<std::size_t>(16U, settings_.packetAlignmentBytes);
}

void CameraFrameGraph::begin_frame(std::uint32_t mainWidth, std::uint32_t mainHeight,
                                   Float3 worldOrigin) noexcept {
    mainWidth_ = std::max(1U, mainWidth);
    mainHeight_ = std::max(1U, mainHeight);
    worldOrigin_ = worldOrigin;
    pending_.clear();
}

bool CameraFrameGraph::submit(const camera::CameraViewportFrame& frame,
                              std::uint32_t targetWidth,
                              std::uint32_t targetHeight,
                              std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!frame.viewport.validate(error)) return false;
    if (!frame.viewport.enabled) return true;
    if (!frame.postProcess.validate(error) || !frame.pose.lens.validate(error)) return false;
    targetWidth = targetWidth == 0U ? mainWidth_ : targetWidth;
    targetHeight = targetHeight == 0U ? mainHeight_ : targetHeight;
    if (targetWidth == 0U || targetHeight == 0U) return fail("camera render target extent is zero");
    if (std::any_of(pending_.begin(), pending_.end(), [&](const auto& plan) {
            return plan.frame.viewport.id == frame.viewport.id;
        })) return fail("camera viewport submitted more than once in one frame");

    CameraViewportRenderPlan plan;
    plan.frame = frame;
    plan.pixelX = std::min(targetWidth - 1U, static_cast<std::uint32_t>(
        std::floor(frame.viewport.normalizedX * static_cast<float>(targetWidth))));
    plan.pixelY = std::min(targetHeight - 1U, static_cast<std::uint32_t>(
        std::floor(frame.viewport.normalizedY * static_cast<float>(targetHeight))));
    plan.pixelWidth = std::max(1U, static_cast<std::uint32_t>(
        std::lround(frame.viewport.normalizedWidth * static_cast<float>(targetWidth))));
    plan.pixelHeight = std::max(1U, static_cast<std::uint32_t>(
        std::lround(frame.viewport.normalizedHeight * static_cast<float>(targetHeight))));
    plan.pixelWidth = std::min(plan.pixelWidth, targetWidth - plan.pixelX);
    plan.pixelHeight = std::min(plan.pixelHeight, targetHeight - plan.pixelY);

    CameraTemporalResetReason reasons = CameraTemporalResetReason::NoReset;
    const auto previousIt = previous_.find(frame.viewport.id);
    if (frame.gpu.resetTemporalHistory != 0U ||
        (previousIt != previous_.end() && previousIt->second.valid &&
         previousIt->second.cutGeneration != frame.gpu.cameraCutGeneration)) {
        reasons = reasons | CameraTemporalResetReason::Cut;
    }
    if (manualResets_[frame.viewport.id]) reasons = reasons | CameraTemporalResetReason::Manual;
    if (previousIt != previous_.end() && previousIt->second.valid) {
        const auto& previous = previousIt->second;
        if (length(subtract(frame.pose.position, previous.pose.position)) >
                settings_.teleportDistanceMeters ||
            pose_angle(frame.pose, previous.pose) > settings_.teleportAngleRadians) {
            reasons = reasons | CameraTemporalResetReason::Teleport;
        }
        if (different_origin(worldOrigin_, previous.worldOrigin))
            reasons = reasons | CameraTemporalResetReason::OriginShift;
        if (previous.width != targetWidth || previous.height != targetHeight)
            reasons = reasons | CameraTemporalResetReason::ViewportResize;
        if (previous.target != frame.viewport.renderTarget)
            reasons = reasons | CameraTemporalResetReason::RenderTargetChange;
    } else {
        reasons = reasons | CameraTemporalResetReason::Manual;
    }

    plan.history.reasons = reasons;
    plan.history.resetTaa = any(reasons);
    plan.history.resetMotionVectors = any(reasons);
    plan.history.resetExposure =
        any(reasons & (CameraTemporalResetReason::Teleport |
                       CameraTemporalResetReason::OriginShift |
                       CameraTemporalResetReason::ViewportResize |
                       CameraTemporalResetReason::RenderTargetChange |
                       CameraTemporalResetReason::Manual)) ||
        (settings_.resetExposureOnCut &&
         any(reasons & CameraTemporalResetReason::Cut));
    plan.history.resetOcclusion =
        any(reasons & (CameraTemporalResetReason::Teleport |
                       CameraTemporalResetReason::OriginShift |
                       CameraTemporalResetReason::ViewportResize |
                       CameraTemporalResetReason::RenderTargetChange |
                       CameraTemporalResetReason::Manual)) ||
        (settings_.resetOcclusionOnCut &&
         any(reasons & CameraTemporalResetReason::Cut));

    previous_[frame.viewport.id] = {
        frame.pose, frame.gpu.cameraCutGeneration, targetWidth, targetHeight,
        frame.viewport.renderTarget, worldOrigin_, true};
    manualResets_[frame.viewport.id] = false;
    pending_.push_back(std::move(plan));
    return true;
}

void CameraFrameGraph::request_manual_reset(camera::CameraViewportId id) noexcept {
    manualResets_[id] = true;
}

CameraFramePlan CameraFrameGraph::finalize() {
    CameraFramePlan result;
    result.viewports = std::move(pending_);
    pending_.clear();
    std::stable_sort(result.viewports.begin(), result.viewports.end(), [](const auto& a, const auto& b) {
        if (a.frame.viewport.renderTarget != b.frame.viewport.renderTarget)
            return a.frame.viewport.renderTarget < b.frame.viewport.renderTarget;
        return a.frame.viewport.id < b.frame.viewport.id;
    });
    result.packetStride = align_up(sizeof(camera::CameraGpuPacket), settings_.packetAlignmentBytes);
    result.packetUpload.assign(result.packetStride * result.viewports.size(), std::byte{});

    std::map<std::string, CameraRenderTargetPlan, std::less<>> targets;
    for (std::size_t index = 0; index < result.viewports.size(); ++index) {
        auto& viewport = result.viewports[index];
        viewport.gpuPacketOffset = index * result.packetStride;
        std::memcpy(result.packetUpload.data() + viewport.gpuPacketOffset,
                    &viewport.frame.gpu, sizeof(camera::CameraGpuPacket));
        auto& target = targets[viewport.frame.viewport.renderTarget];
        target.name = viewport.frame.viewport.renderTarget;
        target.width = std::max(target.width, viewport.pixelX + viewport.pixelWidth);
        target.height = std::max(target.height, viewport.pixelY + viewport.pixelHeight);
        target.viewports.push_back(viewport.frame.viewport.id);
    }
    for (auto& [name, target] : targets) {
        (void)name;
        result.targets.push_back(std::move(target));
    }
    return result;
}

bool CameraFrameGraph::upload_packets(rhi::IDevice& device, rhi::BufferHandle buffer,
                                      const CameraFramePlan& plan,
                                      std::string* error) const {
    if (!buffer) {
        if (error) *error = "camera packet upload buffer is invalid";
        return false;
    }
    if (plan.packetUpload.empty()) return true;
    return device.write_buffer(buffer, 0U, plan.packetUpload, error);
}

bool CameraFloatImage::validate(std::string* error) const {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (width == 0U || height == 0U) return fail("camera image extent is zero");
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (rgba.size() != pixels * 4U || linearDepthMeters.size() != pixels)
        return fail("camera image storage does not match its extent");
    for (float value : rgba) if (!std::isfinite(value)) return fail("camera image contains non-finite color");
    for (float value : linearDepthMeters)
        if (!(std::isfinite(value) || std::isinf(value)) || value < 0.0F)
            return fail("camera image contains invalid linear depth");
    return true;
}

CameraDofTelemetry apply_camera_depth_of_field(CameraFloatImage& image,
                                                const camera::CameraPose& pose,
                                                const camera::CameraPostProcessProfile& post,
                                                const CameraDofSettings& settings) {
    CameraDofTelemetry telemetry;
    std::string ignored;
    if (!image.validate(&ignored) || settings.quality == CameraDofQuality::Off ||
        post.depthOfFieldWeight <= 0.0F) return telemetry;
    const std::vector<float> source = image.rgba;
    const auto pattern = sample_pattern(settings.quality);
    // Expressed as a fraction of image height so the depth of field look does not change with
    // output resolution. A fixed pixel cap clamped 1080p and 2160p renders of the same shot by
    // very different amounts.
    const float maxRadius = std::max(0.0F, settings.maximumBlurRadiusFraction) *
                            static_cast<float>(image.height);
    const auto cocAt=[&](std::uint32_t px,std::uint32_t py,float depth){
        return std::min(maxRadius,split_diopter_coc_pixels(depth,pose,post,px,py,
            image.width,image.height,settings.useSplitDiopter));
    };
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
            const float depth = image.linearDepthMeters[pixel];
            if (settings.focusPlanePreview) {
                const float focusDistance=std::max(0.001F,split_diopter_preview_focus(
                    post,x,y,image.width,image.height,settings.useSplitDiopter));
                const float delta = std::isfinite(depth) ? std::abs(depth - focusDistance)
                                                         : std::numeric_limits<float>::infinity();
                float* out = image.rgba.data() + pixel * 4U;
                if (delta <= std::max(0.001F, settings.focusPlaneBandMeters)) {
                    out[0] = out[0] * 0.4F;
                    out[1] = out[1] * 0.4F + 0.6F;
                    out[2] = out[2] * 0.4F;
                    ++telemetry.focusedPixels;
                } else {
                    out[0] = out[0] * 0.6F + 0.25F;
                    out[1] *= 0.6F;
                    out[2] *= 0.6F;
                }
                continue;
            }

            const float radius = cocAt(x,y,depth);
            telemetry.maximumCircleOfConfusionPixels =
                std::max(telemetry.maximumCircleOfConfusionPixels, radius);

            // A sharp pixel can still be covered by a nearer, blurrier neighbour, so the gather
            // radius cannot be the centre pixel's own circle of confusion. Without this, out of
            // focus foreground never spreads over a sharp background, which is the most
            // conspicuous depth of field error. The coarse probe ring stands in for the tile-max
            // pass a production implementation would use.
            float gatherRadius = radius;
            if (maxRadius > radius) {
                for (std::uint32_t probe = 0U; probe < 8U; ++probe) {
                    const float probeAngle = static_cast<float>(probe) * 0.78539816F;
                    const int px = std::clamp(static_cast<int>(std::lround(
                        static_cast<float>(x) + std::cos(probeAngle) * maxRadius)),
                        0, static_cast<int>(image.width) - 1);
                    const int py = std::clamp(static_cast<int>(std::lround(
                        static_cast<float>(y) + std::sin(probeAngle) * maxRadius)),
                        0, static_cast<int>(image.height) - 1);
                    const std::size_t pp = static_cast<std::size_t>(py) * image.width +
                                           static_cast<std::size_t>(px);
                    const float probeDepth = image.linearDepthMeters[pp];
                    if (std::isfinite(probeDepth) && (!std::isfinite(depth) || probeDepth < depth))
                        gatherRadius = std::max(gatherRadius,
                            cocAt(static_cast<std::uint32_t>(px),
                                  static_cast<std::uint32_t>(py), probeDepth));
                }
            }

            if (!(gatherRadius >= 0.5F) || pattern.empty()) {
                ++telemetry.focusedPixels;
                continue;
            }

            const float screenU=(static_cast<float>(x)+0.5F)/static_cast<float>(image.width)*2.0F-1.0F;
            const float screenV=(static_cast<float>(y)+0.5F)/static_cast<float>(image.height)*2.0F-1.0F;
            float accum[4]{source[pixel * 4U + 0U], source[pixel * 4U + 1U],
                           source[pixel * 4U + 2U], source[pixel * 4U + 3U]};
            float weight = 1.0F;
            for (std::size_t sample = 0; sample < pattern.size(); ++sample) {
                const float ring = sample < pattern.size() / 2U ? 0.55F : 1.0F;
                std::pair<float,float> apertureSample = pattern[sample];
                if (settings.useCinematicBokeh) {
                    const auto shaped = shape_bokeh_sample(pattern[sample], post.cinematic.bokeh,
                                                           screenU, screenV);
                    if (!shaped) continue; // clipped by optical vignetting
                    apertureSample = *shaped;
                }
                const float offsetX = apertureSample.first * gatherRadius * ring;
                const float offsetY = apertureSample.second * gatherRadius * ring;
                // 2026-07-25: the reach test below has to measure distance in the aperture's own
                // metric, not in pixels. The circle of confusion is the radius of a *circular*
                // aperture image; an anamorphic aperture is an ellipse of the same area with
                // semi-axes coc/ratio and coc*ratio, so a tap on the vertical extreme sits
                // ratio^2 times further away in pixels than the horizontal one while being
                // equally inside the aperture. Comparing raw pixel distance against the circular
                // coc therefore rejected exactly the taps that carry the anamorphic shape, and
                // clipped the bokeh back to a circle: at ratio 2.0 the measured highlight came
                // out 7x6 px instead of the predicted 2:1. Undoing the shaping before measuring
                // restores it. Source ellipse covers this pixel iff
                // (dx*ratio)^2 + (dy/ratio)^2 <= coc^2.
                const float shapeRatio = std::sqrt(std::clamp(
                    post.cinematic.bokeh.anamorphicRatio, 0.25F, 4.0F));
                const float shapedX = offsetX * shapeRatio;
                const float shapedY = offsetY / shapeRatio;
                const float offsetLength = std::sqrt(shapedX * shapedX + shapedY * shapedY);
                const int sx = std::clamp(static_cast<int>(std::lround(
                    static_cast<float>(x) + offsetX)), 0, static_cast<int>(image.width) - 1);
                const int sy = std::clamp(static_cast<int>(std::lround(
                    static_cast<float>(y) + offsetY)), 0, static_cast<int>(image.height) - 1);
                const std::size_t sp = static_cast<std::size_t>(sy) * image.width +
                                       static_cast<std::size_t>(sx);
                const float sampleDepth = image.linearDepthMeters[sp];
                const float sampleCoc = cocAt(static_cast<std::uint32_t>(sx),
                                              static_cast<std::uint32_t>(sy), sampleDepth);

                // Scatter as gather: a tap contributes only where its own blur circle actually
                // reaches this pixel. Background taps additionally contribute out to this pixel's
                // own circle of confusion, which is the ordinary gather assumption for defocused
                // background.
                float reach = sampleCoc;
                const bool behind = !std::isfinite(sampleDepth) || !std::isfinite(depth) ||
                                    sampleDepth >= depth;
                if (behind) reach = std::max(reach, radius);
                float sampleWeight = std::clamp(reach - offsetLength + 1.0F, 0.0F, 1.0F);
                if (settings.preserveForegroundEdges && std::isfinite(depth) &&
                    std::isfinite(sampleDepth) && sampleDepth + 0.05F < depth) {
                    // Nearer geometry may only bleed in as far as it is genuinely blurred.
                    sampleWeight = std::min(sampleWeight,
                        std::clamp(sampleCoc - offsetLength + 1.0F, 0.0F, 1.0F));
                }
                if (!(sampleWeight > 0.0F)) continue;
                for (std::size_t channel = 0; channel < 4U; ++channel)
                    accum[channel] += source[sp * 4U + channel] * sampleWeight;
                weight += sampleWeight;
            }
            float* out = image.rgba.data() + pixel * 4U;
            for (std::size_t channel = 0; channel < 4U; ++channel)
                out[channel] = accum[channel] / std::max(weight, 1.0e-5F);
            ++telemetry.blurredPixels;
        }
    }
    return telemetry;
}

bool CameraColorLut3D::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(edgeSize<2U||edgeSize>65U)return fail("camera LUT edge size must be 2 to 65");
    const std::size_t edge=static_cast<std::size_t>(edgeSize);
    if(values.size()!=edge*edge*edge)return fail("camera LUT value count does not match edge size");
    for(const Float3 value:values){
        if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z))
            return fail("camera LUT contains a non-finite value");
    }
    return true;
}

bool CameraMotionField::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(width==0U||height==0U)return fail("camera motion field extent is zero");
    const std::size_t pixels=static_cast<std::size_t>(width)*height;
    if(xyPixels.size()!=pixels*2U)return fail("camera motion field storage does not match its extent");
    for(float value:xyPixels)if(!std::isfinite(value))return fail("camera motion field contains non-finite motion");
    return true;
}

std::optional<CameraColorLut3D> parse_camera_cube_lut(std::string_view text,std::string* error) {
    auto fail=[&](std::string message)->std::optional<CameraColorLut3D>{
        if(error) *error=std::move(message);
        return std::nullopt;
    };
    CameraColorLut3D result;
    std::istringstream input{std::string(text)};
    std::string line;
    while(std::getline(input,line)){
        const auto comment=line.find('#');if(comment!=std::string::npos)line.erase(comment);
        const auto first=line.find_first_not_of(" \t\r");if(first==std::string::npos)continue;
        const auto last=line.find_last_not_of(" \t\r");line=line.substr(first,last-first+1U);
        std::istringstream row(line);
        std::string token;row>>token;
        if(token=="TITLE")continue;
        if(token=="DOMAIN_MIN"||token=="DOMAIN_MAX"){
            float a{},b{},c{};if(!(row>>a>>b>>c))return fail("malformed camera LUT domain");
            continue;
        }
        if(token=="LUT_3D_SIZE"){
            std::uint32_t edge{};if(!(row>>edge)||edge<2U||edge>65U)return fail("invalid camera LUT edge size");
            if(result.edgeSize!=0U)return fail("duplicate camera LUT edge size");
            result.edgeSize=edge;continue;
        }
        if(result.edgeSize==0U)return fail("camera LUT data appears before LUT_3D_SIZE");
        row.clear();row.str(line);
        Float3 value{};if(!(row>>value.x>>value.y>>value.z))return fail("malformed camera LUT sample");
        std::string trailing;if(row>>trailing)return fail("trailing camera LUT sample data");
        result.values.push_back(value);
    }
    if(!result.validate(error))return std::nullopt;
    return result;
}

bool apply_camera_motion_blur(CameraFloatImage& image,const CameraMotionField& motionField,
                              float weight,float shutterAngleDegrees,std::uint32_t maximumSamples,
                              std::uint64_t* blurredPixels,std::string* error) {
    if(!image.validate(error)||!motionField.validate(error))return false;
    if(image.width!=motionField.width||image.height!=motionField.height){
        if(error) *error="camera motion field extent does not match image";
        return false;
    }
    if(!std::isfinite(weight)||!std::isfinite(shutterAngleDegrees)||weight<0.0F||weight>1.0F||
       shutterAngleDegrees<0.0F||shutterAngleDegrees>360.0F||maximumSamples<2U||maximumSamples>64U){
        if(error) *error="camera motion-blur settings are invalid";
        return false;
    }
    const float exposureScale=weight*(shutterAngleDegrees/360.0F);
    if(!(exposureScale>0.0F)){if(blurredPixels)*blurredPixels=0U;return true;}
    const std::vector<float> source=image.rgba;
    std::uint64_t count{};
    for(std::uint32_t y=0;y<image.height;++y){
        for(std::uint32_t x=0;x<image.width;++x){
            const std::size_t pixel=static_cast<std::size_t>(y)*image.width+x;
            const float mx=motionField.xyPixels[pixel*2U]*exposureScale;
            const float my=motionField.xyPixels[pixel*2U+1U]*exposureScale;
            const float magnitude=std::sqrt(mx*mx+my*my);
            if(!(magnitude>=0.5F))continue;
            const std::uint32_t samples=std::clamp(static_cast<std::uint32_t>(std::ceil(magnitude))+1U,
                                                   2U,maximumSamples);
            std::array<float,4> accum{};
            for(std::uint32_t sample=0U;sample<samples;++sample){
                const float t=samples>1U?static_cast<float>(sample)/static_cast<float>(samples-1U)-0.5F:0.0F;
                const auto color=sample_rgba_bilinear(source,image.width,image.height,
                    static_cast<float>(x)+mx*t,static_cast<float>(y)+my*t);
                for(std::size_t channel=0;channel<4U;++channel)accum[channel]+=color[channel];
            }
            float* out=image.rgba.data()+pixel*4U;
            for(std::size_t channel=0;channel<4U;++channel)out[channel]=accum[channel]/static_cast<float>(samples);
            ++count;
        }
    }
    if(blurredPixels)*blurredPixels=count;
    return true;
}

CameraCinematicTelemetry apply_camera_cinematic_pipeline(
    CameraFloatImage& image,const camera::CameraPose& pose,
    const camera::CameraPostProcessProfile& post,
    const CameraCinematicPipelineSettings& settings,
    const CameraColorLut3D* colorLut,const CameraMotionField* motionField) {
    CameraCinematicTelemetry telemetry;
    std::string ignored;
    if(!image.validate(&ignored)||!post.validate(&ignored)||!pose.lens.validate(&ignored))return telemetry;
    const auto& cinematic=post.cinematic;

    if(settings.applyLensDistortion){
        const auto& lens=cinematic.lens;
        const bool active=lens.distortionModel!=camera::CameraLensDistortionModel::Disabled||
            lens.chromaticAberrationPixels>0.0F||
            lens.lensBreathing>0.0F||lens.gateWeavePixels>0.0F;
        if(active){
            const std::vector<float> source=image.rgba;
            const std::vector<float> sourceDepth=image.linearDepthMeters;
            const float aspect=static_cast<float>(image.width)/static_cast<float>(image.height);
            const float weaveX=(deterministic_noise(0U,0U,settings.frameIndex,17U)-0.5F)*
                               lens.gateWeavePixels*2.0F;
            const float weaveY=(deterministic_noise(1U,0U,settings.frameIndex,31U)-0.5F)*
                               lens.gateWeavePixels*2.0F;
            // Focus breathing is a magnification change, m = f / (D - f), referenced to infinity
            // focus. The previous clamp(1/D) pinned the effect at its maximum for every focus
            // distance below one metre, which is exactly the range where breathing is visible.
            const float breathingFocalMeters=std::max(0.001F,
                (pose.lens.physical.enabled?pose.lens.physical.focalLengthMillimeters:35.0F)*0.001F);
            const float breathingFocusMeters=std::max(breathingFocalMeters+0.001F,
                                                      post.focusDistanceMeters);
            const float magnification=breathingFocalMeters/(breathingFocusMeters-breathingFocalMeters);
            const float breathingScale=1.0F+lens.lensBreathing*magnification;
            for(std::uint32_t y=0U;y<image.height;++y){
                for(std::uint32_t x=0U;x<image.width;++x){
                    float nx=((static_cast<float>(x)+0.5F)/static_cast<float>(image.width)*2.0F-1.0F)*aspect;
                    float ny=(static_cast<float>(y)+0.5F)/static_cast<float>(image.height)*2.0F-1.0F;
                    // No horizontal remap for anamorphicSqueeze: it widens the frustum at capture
                    // in camera_projection_matrix, so this image is already desqueezed.
                    nx*=breathingScale;ny*=breathingScale;
                    const float radius=std::sqrt(nx*nx+ny*ny);
                    if(lens.distortionModel==camera::CameraLensDistortionModel::BrownConrady){
                        const float r2=radius*radius;
                        const float scale=1.0F+lens.radialK1*r2+lens.radialK2*r2*r2+lens.radialK3*r2*r2*r2;
                        nx*=scale;ny*=scale;
                    }else if(lens.distortionModel==camera::CameraLensDistortionModel::FisheyeEquidistant&&
                             lens.fisheyeStrength>0.0F&&radius>1.0e-6F){
                        const float normalizedRadius=std::min(1.0F,radius/1.41421356F);
                        const float maximumAngle=0.45F+lens.fisheyeStrength*0.85F;
                        const float mapped=std::tan(normalizedRadius*maximumAngle)/std::tan(maximumAngle)*1.41421356F;
                        const float scale=mapped/radius;nx*=scale;ny*=scale;
                    }
                    const float sx=(nx/aspect+1.0F)*0.5F*static_cast<float>(image.width)-0.5F+weaveX;
                    const float sy=(ny+1.0F)*0.5F*static_cast<float>(image.height)-0.5F+weaveY;
                    const float dx=sx-static_cast<float>(x),dy=sy-static_cast<float>(y);
                    const float displacement=std::sqrt(dx*dx+dy*dy);
                    telemetry.maximumDistortionPixels=std::max(telemetry.maximumDistortionPixels,displacement);
                    if(displacement>0.01F)++telemetry.distortedPixels;
                    const auto base=sample_rgba_bilinear(source,image.width,image.height,sx,sy);
                    // Lateral chromatic aberration grows with field height and is zero on the
                    // optical axis. The previous constant offset displaced the channels even at
                    // the exact centre of frame, and worked in aspect corrected NDC while the
                    // shader worked in pixels, so the two could not agree.
                    const float centreX=static_cast<float>(image.width)*0.5F;
                    const float centreY=static_cast<float>(image.height)*0.5F;
                    const float deltaX=sx-centreX,deltaY=sy-centreY;
                    const float pixelRadius=std::sqrt(deltaX*deltaX+deltaY*deltaY);
                    const float cornerRadius=std::sqrt(centreX*centreX+centreY*centreY);
                    const float fieldHeight=cornerRadius>1.0e-4F?
                        std::clamp(pixelRadius/cornerRadius,0.0F,1.0F):0.0F;
                    const float aberration=lens.chromaticAberrationPixels*fieldHeight;
                    const float ax=pixelRadius>1.0e-4F?deltaX/pixelRadius*aberration:0.0F;
                    const float ay=pixelRadius>1.0e-4F?deltaY/pixelRadius*aberration:0.0F;
                    const auto red=sample_rgba_bilinear(source,image.width,image.height,sx+ax,sy+ay);
                    const auto blue=sample_rgba_bilinear(source,image.width,image.height,sx-ax,sy-ay);
                    const std::size_t pixel=static_cast<std::size_t>(y)*image.width+x;
                    image.rgba[pixel*4U]=red[0];image.rgba[pixel*4U+1U]=base[1];
                    image.rgba[pixel*4U+2U]=blue[2];image.rgba[pixel*4U+3U]=base[3];
                    image.linearDepthMeters[pixel]=sample_depth_nearest(sourceDepth,image.width,image.height,sx,sy);
                }
            }
        }
    }

    if(settings.applyDepthOfField)
        telemetry.depthOfField=apply_camera_depth_of_field(image,pose,post,settings.depthOfField);

    if(motionField&&cinematic.film.motionBlurWeight>0.0F){
        (void)apply_camera_motion_blur(image,*motionField,cinematic.film.motionBlurWeight,
            cinematic.film.shutterAngleDegrees,16U,&telemetry.motionBlurredPixels,&ignored);
    }

    if(settings.applyColorGrade||settings.applyFilmEffects){
        const std::vector<float> source=image.rgba;
        const auto& grade=cinematic.colorGrade;
        const auto& film=cinematic.film;
        // Exposure is applied ahead of every threshold based effect. Thresholding unexposed
        // radiance meant that which highlights halated or flared did not change when the camera
        // was stopped down.
        const float exposure=settings.applyColorGrade?post.exposure:1.0F;
        // Colour temperature is perceptually uniform in mireds, not in kelvin. The sign convention
        // is unchanged: lowering the value warms the image, which is what the shipped presets
        // expect. Blending 3200 K to 6500 K in kelvin moved the colour fast then slow.
        const float referenceMired=1.0e6F/6500.0F;
        const float mired=1.0e6F/std::clamp(grade.temperatureKelvin,1000.0F,20000.0F);
        const float warmth=std::clamp((mired-referenceMired)/158.7F,-1.0F,1.0F);
        const float tint=grade.tint;
        const bool validLut=colorLut&&colorLut->validate(nullptr)&&grade.lutBlend>0.0F;
        const auto exposedLuminanceAt=[&](int sx,int sy){
            sx=std::clamp(sx,0,static_cast<int>(image.width)-1);
            sy=std::clamp(sy,0,static_cast<int>(image.height)-1);
            const std::size_t sp=static_cast<std::size_t>(sy)*image.width+static_cast<std::size_t>(sx);
            return luminance(source[sp*4U],source[sp*4U+1U],source[sp*4U+2U])*exposure;
        };
        // Halation and flare are wide optical effects. Their radii are fractions of the frame so
        // they read the same at every delivery resolution; the previous 3x3 halation gather and
        // 25 pixel flare streak were invisible at any production size.
        const float halationRadius=0.02F*static_cast<float>(image.height);
        const float flareSpan=0.12F*static_cast<float>(image.width);
        for(std::uint32_t y=0U;y<image.height;++y){
            for(std::uint32_t x=0U;x<image.width;++x){
                const std::size_t pixel=static_cast<std::size_t>(y)*image.width+x;
                Float3 color{source[pixel*4U],source[pixel*4U+1U],source[pixel*4U+2U]};
                color=multiply(color,exposure);
                if(settings.applyFilmEffects&&film.sharpenIntensity>0.0F){
                    Float3 average{};std::uint32_t count{};
                    const int offsets[4][2]={{-1,0},{1,0},{0,-1},{0,1}};
                    for(const auto& offset:offsets){
                        const int sx=std::clamp(static_cast<int>(x)+offset[0],0,static_cast<int>(image.width)-1);
                        const int sy=std::clamp(static_cast<int>(y)+offset[1],0,static_cast<int>(image.height)-1);
                        const std::size_t sp=static_cast<std::size_t>(sy)*image.width+static_cast<std::size_t>(sx);
                        average.x+=source[sp*4U];average.y+=source[sp*4U+1U];average.z+=source[sp*4U+2U];++count;
                    }
                    average=multiply(average,exposure/static_cast<float>(count));
                    color=add(color,multiply(subtract(color,average),film.sharpenIntensity));
                }
                if(settings.applyFilmEffects&&film.halationIntensity>0.0F){
                    // Light through the emulsion, scattered in the base, reflected off the
                    // backing: a wide red-orange glow around clipped highlights.
                    float halo{},haloWeight{};
                    for(std::uint32_t ring=1U;ring<=2U;++ring){
                        const float ringRadius=halationRadius*static_cast<float>(ring)*0.5F;
                        const float ringWeight=1.0F/static_cast<float>(ring);
                        for(std::uint32_t step=0U;step<8U;++step){
                            const float stepAngle=(static_cast<float>(step)+
                                0.5F*static_cast<float>(ring))*0.78539816F;
                            const int sx=static_cast<int>(std::lround(static_cast<float>(x)+
                                std::cos(stepAngle)*ringRadius));
                            const int sy=static_cast<int>(std::lround(static_cast<float>(y)+
                                std::sin(stepAngle)*ringRadius));
                            halo+=std::max(0.0F,exposedLuminanceAt(sx,sy)-1.0F)*ringWeight;
                            haloWeight+=ringWeight;
                        }
                    }
                    halo=halo/std::max(haloWeight,1.0e-5F)*film.halationIntensity;
                    color.x+=halo*0.75F;color.y+=halo*0.20F;color.z+=halo*0.05F;
                }
                if(settings.applyFilmEffects&&cinematic.lens.anamorphicFlareIntensity>0.0F){
                    float flare{},flareWeight{};
                    for(std::uint32_t step=1U;step<=12U;++step){
                        const float distance=flareSpan*static_cast<float>(step)/12.0F;
                        const float stepWeight=1.0F-static_cast<float>(step-1U)/12.0F;
                        const int offset=static_cast<int>(std::lround(distance));
                        const float left=exposedLuminanceAt(static_cast<int>(x)-offset,static_cast<int>(y));
                        const float right=exposedLuminanceAt(static_cast<int>(x)+offset,static_cast<int>(y));
                        flare+=(std::max(0.0F,left-cinematic.lens.anamorphicFlareThreshold)+
                                std::max(0.0F,right-cinematic.lens.anamorphicFlareThreshold))*stepWeight;
                        flareWeight+=2.0F*stepWeight;
                    }
                    flare=flare/std::max(flareWeight,1.0e-5F)*cinematic.lens.anamorphicFlareIntensity;
                    color.x+=flare*0.35F;color.y+=flare*0.60F;color.z+=flare;
                }
                const float u=(static_cast<float>(x)+0.5F)/static_cast<float>(image.width)*2.0F-1.0F;
                const float v=(static_cast<float>(y)+0.5F)/static_cast<float>(image.height)*2.0F-1.0F;
                if(settings.applyColorGrade){
                    color.x*=1.0F+0.12F*warmth+0.05F*tint;
                    color.y*=1.0F-0.08F*tint;
                    color.z*=1.0F-0.12F*warmth+0.05F*tint;
                    color=add(color,grade.lift);
                    color.x=std::pow(std::max(0.0F,color.x),1.0F/grade.gamma.x)*grade.gain.x;
                    color.y=std::pow(std::max(0.0F,color.y),1.0F/grade.gamma.y)*grade.gain.y;
                    color.z=std::pow(std::max(0.0F,color.z),1.0F/grade.gamma.z)*grade.gain.z;
                    color.x=(color.x-0.18F)*post.contrast+0.18F;
                    color.y=(color.y-0.18F)*post.contrast+0.18F;
                    color.z=(color.z-0.18F)*post.contrast+0.18F;
                    const float luma=luminance(color.x,color.y,color.z);
                    color.x=luma+(color.x-luma)*post.saturation;
                    color.y=luma+(color.y-luma)*post.saturation;
                    color.z=luma+(color.z-luma)*post.saturation;
                    color.x*=post.colorTint.x;color.y*=post.colorTint.y;color.z*=post.colorTint.z;
                }
                // Vignetting is an irradiance falloff at the aperture, so it belongs in linear
                // scene referred light ahead of the tone curve. Applied afterwards it darkened
                // display values instead of removing light, which changes the highlight rolloff
                // rather than dimming the corners.
                if(settings.applyFilmEffects&&post.vignette>0.0F){
                    const float radius=std::clamp((u*u+v*v)*0.5F,0.0F,1.0F);
                    const float factor=std::max(0.0F,1.0F-post.vignette*smoothstep(0.25F,1.0F,radius));
                    color=multiply(color,factor);
                }
                if(settings.applyColorGrade){
                    color.x=tone_map(color.x,grade.toneMap);color.y=tone_map(color.y,grade.toneMap);
                    color.z=tone_map(color.z,grade.toneMap);
                    // Grading LUTs are authored against display referred input, so the lookup
                    // happens after the tone curve. Sampling clamped linear values beforehand
                    // collapsed everything above 1.0 onto the LUT white corner and destroyed
                    // highlight separation.
                    if(validLut){
                        const Float3 mapped=sample_lut(*colorLut,color);
                        color=add(multiply(color,1.0F-grade.lutBlend),multiply(mapped,grade.lutBlend));
                    }
                }
                if(settings.applyFilmEffects&&film.grainIntensity>0.0F){
                    // Film grain density peaks in the midtones and vanishes in clipped white and
                    // clean black. Uniform additive noise lifted the blacks, because the negative
                    // half of the signal was clipped away by the final max().
                    const float grainLuma=std::clamp(luminance(color.x,color.y,color.z),0.0F,1.0F);
                    const float density=4.0F*grainLuma*(1.0F-grainLuma);
                    const float noise=(deterministic_noise(x,y,settings.frameIndex,film.grainSeed)-0.5F)*
                                      film.grainIntensity*0.12F*density;
                    color.x+=noise;color.y+=noise;color.z+=noise;
                }
                image.rgba[pixel*4U]=std::max(0.0F,color.x);
                image.rgba[pixel*4U+1U]=std::max(0.0F,color.y);
                image.rgba[pixel*4U+2U]=std::max(0.0F,color.z);
                ++telemetry.gradedPixels;
            }
        }
    }

    if(settings.applyFraming&&cinematic.film.framing!=camera::CameraFramingPreset::Native){
        const float nativeAspect=static_cast<float>(image.width)/static_cast<float>(image.height);
        const float targetAspect=camera::camera_framing_aspect_ratio(cinematic.film,nativeAspect);
        float minimumX=0.0F,maximumX=1.0F,minimumY=0.0F,maximumY=1.0F;
        if(nativeAspect>targetAspect){
            const float widthFraction=targetAspect/nativeAspect;
            minimumX=(1.0F-widthFraction)*0.5F;maximumX=1.0F-minimumX;
        }else if(nativeAspect<targetAspect){
            const float heightFraction=nativeAspect/targetAspect;
            minimumY=(1.0F-heightFraction)*0.5F;maximumY=1.0F-minimumY;
        }
        for(std::uint32_t y=0U;y<image.height;++y){
            for(std::uint32_t x=0U;x<image.width;++x){
                const float u=(static_cast<float>(x)+0.5F)/static_cast<float>(image.width);
                const float v=(static_cast<float>(y)+0.5F)/static_cast<float>(image.height);
                if(u>=minimumX&&u<=maximumX&&v>=minimumY&&v<=maximumY)continue;
                const std::size_t pixel=static_cast<std::size_t>(y)*image.width+x;
                float* out=image.rgba.data()+pixel*4U;
                const float opacity=cinematic.film.matteOpacity;
                out[0]+= (cinematic.film.matteColor.x-out[0])*opacity;
                out[1]+= (cinematic.film.matteColor.y-out[1])*opacity;
                out[2]+= (cinematic.film.matteColor.z-out[2])*opacity;
                if(opacity>=0.999F)image.linearDepthMeters[pixel]=std::numeric_limits<float>::infinity();
                ++telemetry.mattePixels;
            }
        }
    }
    return telemetry;
}

bool composite_camera_viewport(CameraFloatImage& destination,
                               const CameraFloatImage& source,
                               const camera::CameraViewportDesc& viewport,
                               std::string* error) {
    if (!destination.validate(error) || !source.validate(error) || !viewport.validate(error)) return false;
    const std::uint32_t x0 = std::min(destination.width - 1U, static_cast<std::uint32_t>(
        std::floor(viewport.normalizedX * static_cast<float>(destination.width))));
    const std::uint32_t y0 = std::min(destination.height - 1U, static_cast<std::uint32_t>(
        std::floor(viewport.normalizedY * static_cast<float>(destination.height))));
    const std::uint32_t outWidth = std::max(1U, std::min(destination.width - x0,
        static_cast<std::uint32_t>(std::lround(viewport.normalizedWidth * static_cast<float>(destination.width)))));
    const std::uint32_t outHeight = std::max(1U, std::min(destination.height - y0,
        static_cast<std::uint32_t>(std::lround(viewport.normalizedHeight * static_cast<float>(destination.height)))));
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const std::uint32_t sy = std::min(source.height - 1U,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) / outHeight));
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const std::uint32_t sx = std::min(source.width - 1U,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) / outWidth));
            const std::size_t src = static_cast<std::size_t>(sy) * source.width + sx;
            const std::size_t dst = static_cast<std::size_t>(y0 + y) * destination.width + (x0 + x);
            std::copy_n(source.rgba.data() + src * 4U, 4U, destination.rgba.data() + dst * 4U);
            destination.linearDepthMeters[dst] = source.linearDepthMeters[src];
        }
    }
    return true;
}

std::vector<std::uint8_t> camera_image_to_srgb8(const CameraFloatImage& image) {
    std::vector<std::uint8_t> output;
    std::string ignored;
    if (!image.validate(&ignored)) return output;
    output.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(image.width) * image.height; ++pixel) {
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            output[pixel * 4U + channel] = static_cast<std::uint8_t>(std::lround(
                std::clamp(srgb_encode(image.rgba[pixel * 4U + channel]), 0.0F, 1.0F) * 255.0F));
        }
        output[pixel * 4U + 3U] = static_cast<std::uint8_t>(std::lround(
            std::clamp(image.rgba[pixel * 4U + 3U], 0.0F, 1.0F) * 255.0F));
    }
    return output;
}

} // namespace dve::render
