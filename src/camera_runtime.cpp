#include "dve/camera_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iomanip>
#include <sstream>

#include "dve/game_world.hpp"
#include "dve/camera_sequence.hpp"

namespace dve::camera {
namespace {
constexpr float kEpsilon = 1.0e-6F;
Float3 cross3(Float3 a, Float3 b) noexcept { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Float3 safe_normalize(Float3 v, Float3 fallback) noexcept {
    const float l2=length_squared(v); return l2>1.0e-12F&&std::isfinite(l2)?multiply(v,1.0F/std::sqrt(l2)):fallback;
}

}

CameraMatrix4 CameraMatrix4::identity() noexcept { CameraMatrix4 r{}; r.values={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; return r; }

bool CameraColorGradeSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{temperatureKelvin,tint,lift.x,lift.y,lift.z,gamma.x,gamma.y,gamma.z,
                         gain.x,gain.y,gain.z,lutBlend};
    for(float value:values)if(!std::isfinite(value))return fail("color-grade value is not finite");
    if(static_cast<std::uint32_t>(toneMap)>static_cast<std::uint32_t>(CameraToneMapCurve::AcesFilmic))
        return fail("tone-map curve is invalid");
    if(temperatureKelvin<1000.0F||temperatureKelvin>40000.0F||tint<-1.0F||tint>1.0F)
        return fail("white-balance value is out of range");
    if(lutBlend<0.0F||lutBlend>1.0F) return fail("LUT blend is out of range");
    const Float3 vectors[]{lift,gamma,gain};
    for(const Float3 value:vectors){
        if(value.x<-2.0F||value.y<-2.0F||value.z<-2.0F||value.x>16.0F||value.y>16.0F||value.z>16.0F)
            return fail("color-grade vector is out of range");
    }
    if(gamma.x<0.05F||gamma.y<0.05F||gamma.z<0.05F||gain.x<0.0F||gain.y<0.0F||gain.z<0.0F)
        return fail("gamma or gain is invalid");
    return true;
}

bool CameraLensEffectSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{radialK1,radialK2,radialK3,fisheyeStrength,chromaticAberrationPixels,
                         anamorphicSqueeze,lensBreathing,anamorphicFlareIntensity,
                         anamorphicFlareThreshold,gateWeavePixels};
    for(float value:values)if(!std::isfinite(value))return fail("lens-effect value is not finite");
    if(static_cast<std::uint32_t>(distortionModel)>
       static_cast<std::uint32_t>(CameraLensDistortionModel::FisheyeEquidistant))
        return fail("lens-distortion model is invalid");
    if(radialK1<-2.0F||radialK1>2.0F||radialK2<-2.0F||radialK2>2.0F||radialK3<-2.0F||radialK3>2.0F)
        return fail("radial distortion is out of range");
    if(fisheyeStrength<0.0F||fisheyeStrength>1.0F||chromaticAberrationPixels<0.0F||
       chromaticAberrationPixels>32.0F||anamorphicSqueeze<0.25F||anamorphicSqueeze>4.0F||
       lensBreathing<0.0F||lensBreathing>1.0F||anamorphicFlareIntensity<0.0F||
       anamorphicFlareIntensity>8.0F||anamorphicFlareThreshold<0.0F||
       anamorphicFlareThreshold>64.0F||gateWeavePixels<0.0F||gateWeavePixels>32.0F)
        return fail("lens-effect control is out of range");
    return true;
}

bool CameraBokehSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(!std::isfinite(bladeRotationRadians)||!std::isfinite(roundness)||
       !std::isfinite(anamorphicRatio)||!std::isfinite(catEye))
        return fail("bokeh value is not finite");
    if(bladeCount!=0U&&(bladeCount<3U||bladeCount>16U))return fail("bokeh blade count is invalid");
    if(roundness<0.0F||roundness>1.0F||anamorphicRatio<0.25F||anamorphicRatio>4.0F||
       catEye<0.0F||catEye>1.0F)return fail("bokeh control is out of range");
    return true;
}

bool CameraSplitDiopterSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{nearFocusDistanceMeters,farFocusDistanceMeters,centerX,centerY,angleRadians,
                         featherFraction};
    for(float value:values)if(!std::isfinite(value))return fail("split-diopter value is not finite");
    if(nearFocusDistanceMeters>farFocusDistanceMeters)
        return fail("split-diopter near focus must not exceed far focus");
    if(nearFocusDistanceMeters<=0.0F||farFocusDistanceMeters<=0.0F||centerX<-2.0F||centerX>2.0F||
       centerY<-2.0F||centerY>2.0F||angleRadians<-6.2831855F||angleRadians>6.2831855F||
       featherFraction<0.0001F||featherFraction>2.0F)
        return fail("split-diopter control is out of range");
    return true;
}

bool CameraFilmSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{grainIntensity,halationIntensity,sharpenIntensity,motionBlurWeight,
                         shutterAngleDegrees,customAspectRatio,matteOpacity,matteColor.x,
                         matteColor.y,matteColor.z};
    for(float value:values)if(!std::isfinite(value))return fail("film value is not finite");
    if(static_cast<std::uint32_t>(framing)>static_cast<std::uint32_t>(CameraFramingPreset::Custom))
        return fail("framing preset is invalid");
    if(grainIntensity<0.0F||grainIntensity>1.0F||halationIntensity<0.0F||halationIntensity>2.0F||
       sharpenIntensity<0.0F||sharpenIntensity>2.0F||motionBlurWeight<0.0F||motionBlurWeight>1.0F||
       shutterAngleDegrees<0.0F||shutterAngleDegrees>360.0F||customAspectRatio<0.1F||
       customAspectRatio>10.0F||matteOpacity<0.0F||matteOpacity>1.0F||matteColor.x<0.0F||
       matteColor.y<0.0F||matteColor.z<0.0F||matteColor.x>1.0F||matteColor.y>1.0F||matteColor.z>1.0F)
        return fail("film control is out of range");
    return true;
}

bool CameraCinematicSettings::validate(std::string* error) const {
    return colorGrade.validate(error)&&lens.validate(error)&&bokeh.validate(error)&&
           splitDiopter.validate(error)&&film.validate(error);
}

bool CameraPostProcessProfile::validate(std::string* error) const {
    auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};
    const float values[]{exposure,bloomIntensity,saturation,contrast,vignette,depthOfFieldWeight,focusDistanceMeters,apertureFStop,colorTint.x,colorTint.y,colorTint.z};
    for(float v:values) if(!std::isfinite(v)) return fail("post-process value is not finite");
    if(exposure<0.0F||exposure>64.0F) return fail("exposure is out of range");
    if(bloomIntensity<0.0F||bloomIntensity>64.0F) return fail("bloom intensity is out of range");
    if(saturation<0.0F||saturation>8.0F||contrast<0.0F||contrast>8.0F) return fail("color controls are out of range");
    if(vignette<0.0F||vignette>1.0F||depthOfFieldWeight<0.0F||depthOfFieldWeight>1.0F) return fail("post-process weight is out of range");
    if(focusDistanceMeters<=0.0F||apertureFStop<0.5F||apertureFStop>64.0F) return fail("focus or aperture is invalid");
    if(colorTint.x<0.0F||colorTint.y<0.0F||colorTint.z<0.0F) return fail("color tint cannot be negative");
    return cinematic.validate(error);
}

namespace {
CameraCinematicSettings blend_cinematic(const CameraCinematicSettings& from,
                                         const CameraCinematicSettings& to,
                                         float weight) noexcept {
    const auto mix=[weight](float a,float b){return a+(b-a)*weight;};
    const auto mix3=[&](Float3 a,Float3 b){return Float3{mix(a.x,b.x),mix(a.y,b.y),mix(a.z,b.z)};};
    CameraCinematicSettings result{};
    result.colorGrade.toneMap=weight<0.5F?from.colorGrade.toneMap:to.colorGrade.toneMap;
    result.colorGrade.temperatureKelvin=blend_camera_temperature_kelvin(from.colorGrade.temperatureKelvin,to.colorGrade.temperatureKelvin,weight);
    result.colorGrade.tint=mix(from.colorGrade.tint,to.colorGrade.tint);
    result.colorGrade.lift=mix3(from.colorGrade.lift,to.colorGrade.lift);
    result.colorGrade.gamma=mix3(from.colorGrade.gamma,to.colorGrade.gamma);
    result.colorGrade.gain=mix3(from.colorGrade.gain,to.colorGrade.gain);
    result.colorGrade.lutBlend=mix(from.colorGrade.lutBlend,to.colorGrade.lutBlend);
    result.lens.distortionModel=weight<0.5F?from.lens.distortionModel:to.lens.distortionModel;
    result.lens.radialK1=mix(from.lens.radialK1,to.lens.radialK1);
    result.lens.radialK2=mix(from.lens.radialK2,to.lens.radialK2);
    result.lens.radialK3=mix(from.lens.radialK3,to.lens.radialK3);
    result.lens.fisheyeStrength=mix(from.lens.fisheyeStrength,to.lens.fisheyeStrength);
    result.lens.chromaticAberrationPixels=mix(from.lens.chromaticAberrationPixels,to.lens.chromaticAberrationPixels);
    result.lens.anamorphicSqueeze=mix(from.lens.anamorphicSqueeze,to.lens.anamorphicSqueeze);
    result.lens.lensBreathing=mix(from.lens.lensBreathing,to.lens.lensBreathing);
    result.lens.anamorphicFlareIntensity=mix(from.lens.anamorphicFlareIntensity,to.lens.anamorphicFlareIntensity);
    result.lens.anamorphicFlareThreshold=mix(from.lens.anamorphicFlareThreshold,to.lens.anamorphicFlareThreshold);
    result.lens.gateWeavePixels=mix(from.lens.gateWeavePixels,to.lens.gateWeavePixels);
    result.bokeh.bladeCount=blend_camera_blade_count(from.bokeh.bladeCount,to.bokeh.bladeCount,weight);
    result.bokeh.bladeRotationRadians=mix(from.bokeh.bladeRotationRadians,to.bokeh.bladeRotationRadians);
    result.bokeh.roundness=mix(from.bokeh.roundness,to.bokeh.roundness);
    result.bokeh.anamorphicRatio=mix(from.bokeh.anamorphicRatio,to.bokeh.anamorphicRatio);
    result.bokeh.catEye=mix(from.bokeh.catEye,to.bokeh.catEye);
    result.splitDiopter.enabled=weight<0.5F?from.splitDiopter.enabled:to.splitDiopter.enabled;
    result.splitDiopter.nearFocusDistanceMeters=blend_camera_focus_distance(from.splitDiopter.nearFocusDistanceMeters,to.splitDiopter.nearFocusDistanceMeters,weight);
    result.splitDiopter.farFocusDistanceMeters=blend_camera_focus_distance(from.splitDiopter.farFocusDistanceMeters,to.splitDiopter.farFocusDistanceMeters,weight);
    result.splitDiopter.centerX=mix(from.splitDiopter.centerX,to.splitDiopter.centerX);
    result.splitDiopter.centerY=mix(from.splitDiopter.centerY,to.splitDiopter.centerY);
    result.splitDiopter.angleRadians=mix(from.splitDiopter.angleRadians,to.splitDiopter.angleRadians);
    result.splitDiopter.featherFraction=mix(from.splitDiopter.featherFraction,to.splitDiopter.featherFraction);
    result.film.grainIntensity=mix(from.film.grainIntensity,to.film.grainIntensity);
    result.film.halationIntensity=mix(from.film.halationIntensity,to.film.halationIntensity);
    result.film.sharpenIntensity=mix(from.film.sharpenIntensity,to.film.sharpenIntensity);
    result.film.motionBlurWeight=mix(from.film.motionBlurWeight,to.film.motionBlurWeight);
    result.film.shutterAngleDegrees=mix(from.film.shutterAngleDegrees,to.film.shutterAngleDegrees);
    result.film.grainSeed=weight<0.5F?from.film.grainSeed:to.film.grainSeed;
    result.film.framing=weight<0.5F?from.film.framing:to.film.framing;
    result.film.customAspectRatio=mix(from.film.customAspectRatio,to.film.customAspectRatio);
    result.film.matteOpacity=mix(from.film.matteOpacity,to.film.matteOpacity);
    result.film.matteColor=mix3(from.film.matteColor,to.film.matteColor);
    return result;
}
}

CameraPostProcessProfile blend_camera_post_process(const CameraPostProcessProfile& from,
                                                   const CameraPostProcessProfile& to,
                                                   float weight) noexcept {
    weight = std::clamp(weight, 0.0F, 1.0F);
    const auto mix = [weight](float a, float b) { return a + (b - a) * weight; };
    CameraPostProcessProfile result;
    result.exposure = mix(from.exposure, to.exposure);
    result.bloomIntensity = mix(from.bloomIntensity, to.bloomIntensity);
    result.saturation = mix(from.saturation, to.saturation);
    result.contrast = mix(from.contrast, to.contrast);
    result.vignette = mix(from.vignette, to.vignette);
    result.depthOfFieldWeight = mix(from.depthOfFieldWeight, to.depthOfFieldWeight);
    result.focusDistanceMeters = blend_camera_focus_distance(from.focusDistanceMeters, to.focusDistanceMeters, weight);
    result.apertureFStop = blend_camera_f_stop(from.apertureFStop, to.apertureFStop, weight);
    result.colorTint = {
        mix(from.colorTint.x, to.colorTint.x),
        mix(from.colorTint.y, to.colorTint.y),
        mix(from.colorTint.z, to.colorTint.z)};
    result.cinematic=blend_cinematic(from.cinematic,to.cinematic,weight);
    return result;
}

float camera_framing_aspect_ratio(const CameraFilmSettings& film,float nativeAspectRatio) noexcept {
    switch(film.framing){
        case CameraFramingPreset::Native:return std::max(0.1F,nativeAspectRatio);
        case CameraFramingPreset::Academy137:return 1.375F;
        case CameraFramingPreset::Imax143:return 1.43F;
        case CameraFramingPreset::Imax190:return 1.90F;
        case CameraFramingPreset::Widescreen185:return 1.85F;
        case CameraFramingPreset::Scope239:return 2.39F;
        case CameraFramingPreset::Custom:return std::max(0.1F,film.customAspectRatio);
    }
    return std::max(0.1F,nativeAspectRatio);
}

CameraPostProcessProfile camera_cinematic_preset(CameraCinematicPreset preset) noexcept {
    CameraPostProcessProfile result{};
    switch(preset){
        case CameraCinematicPreset::Neutral:break;
        case CameraCinematicPreset::AcademyClassic:
            result.cinematic.film.framing=CameraFramingPreset::Academy137;
            result.cinematic.film.grainIntensity=0.10F;result.cinematic.film.halationIntensity=0.08F;
            result.saturation=0.92F;result.contrast=1.08F;break;
        case CameraCinematicPreset::Imax143:
            result.cinematic.film.framing=CameraFramingPreset::Imax143;result.contrast=1.02F;break;
        case CameraCinematicPreset::Imax190:
            result.cinematic.film.framing=CameraFramingPreset::Imax190;result.contrast=1.02F;break;
        case CameraCinematicPreset::Scope239:
            result.cinematic.film.framing=CameraFramingPreset::Scope239;break;
        case CameraCinematicPreset::VintageAnamorphic:
            result.cinematic.film.framing=CameraFramingPreset::Scope239;
            result.cinematic.lens.anamorphicSqueeze=2.0F;result.cinematic.lens.radialK1=-0.08F;
            result.cinematic.lens.distortionModel=CameraLensDistortionModel::BrownConrady;
            result.cinematic.lens.chromaticAberrationPixels=1.25F;
            result.cinematic.lens.anamorphicFlareIntensity=0.65F;
            result.cinematic.bokeh.anamorphicRatio=2.0F;result.cinematic.bokeh.catEye=0.35F;
            result.cinematic.film.grainIntensity=0.08F;result.cinematic.film.halationIntensity=0.12F;break;
        case CameraCinematicPreset::FisheyeAction:
            result.cinematic.lens.distortionModel=CameraLensDistortionModel::FisheyeEquidistant;
            result.cinematic.lens.fisheyeStrength=0.85F;result.cinematic.lens.chromaticAberrationPixels=0.75F;break;
        case CameraCinematicPreset::SplitDiopter:
            result.depthOfFieldWeight=1.0F;result.apertureFStop=2.0F;
            result.cinematic.splitDiopter.enabled=true;result.cinematic.splitDiopter.nearFocusDistanceMeters=1.2F;
            result.cinematic.splitDiopter.farFocusDistanceMeters=12.0F;break;
        case CameraCinematicPreset::BleachBypass:
            result.saturation=0.35F;result.contrast=1.35F;result.cinematic.colorGrade.gain={1.08F,1.08F,1.08F};
            result.cinematic.film.grainIntensity=0.12F;result.cinematic.film.sharpenIntensity=0.15F;break;
        case CameraCinematicPreset::SeventiesFilm:
            result.saturation=0.82F;result.contrast=0.96F;result.colorTint={1.06F,0.98F,0.86F};
            result.cinematic.colorGrade.temperatureKelvin=5200.0F;result.cinematic.colorGrade.lift={0.015F,0.008F,0.0F};
            result.cinematic.film.grainIntensity=0.18F;result.cinematic.film.halationIntensity=0.20F;
            result.cinematic.lens.gateWeavePixels=0.35F;break;
    }
    return result;
}

bool CameraPostProcessLayer::validate(std::string* error) const {
    if (name.empty()) { if (error) *error = "post-process layer name is empty"; return false; }
    if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
        if (error) *error = "post-process layer weight is out of range";
        return false;
    }
    if ((overrideMask & ~CameraPostAll) != 0U || overrideMask == 0U) {
        if (error) *error = "post-process layer override mask is invalid";
        return false;
    }
    return profile.validate(error);
}

bool CameraPostProcessStack::validate(std::string* error) const {
    if (layers.size() > 64U) { if (error) *error = "too many post-process layers"; return false; }
    for (const auto& layer : layers) if (!layer.validate(error)) return false;
    return true;
}

CameraPostProcessProfile CameraPostProcessStack::evaluate() const noexcept {
    CameraPostProcessProfile result{};
    const auto mix=[](float a,float b,float w){return a+(b-a)*w;};
    for (const auto& layer : layers) {
        if (!layer.enabled || !(layer.weight > 0.0F)) continue;
        const float w=std::clamp(layer.weight,0.0F,1.0F);
        const auto mask=layer.overrideMask;
        if(mask&CameraPostExposure)result.exposure=mix(result.exposure,layer.profile.exposure,w);
        if(mask&CameraPostBloom)result.bloomIntensity=mix(result.bloomIntensity,layer.profile.bloomIntensity,w);
        if(mask&CameraPostSaturation)result.saturation=mix(result.saturation,layer.profile.saturation,w);
        if(mask&CameraPostContrast)result.contrast=mix(result.contrast,layer.profile.contrast,w);
        if(mask&CameraPostVignette)result.vignette=mix(result.vignette,layer.profile.vignette,w);
        if(mask&CameraPostDepthOfField)result.depthOfFieldWeight=mix(result.depthOfFieldWeight,layer.profile.depthOfFieldWeight,w);
        if(mask&CameraPostFocus)result.focusDistanceMeters=mix(result.focusDistanceMeters,layer.profile.focusDistanceMeters,w);
        if(mask&CameraPostAperture)result.apertureFStop=mix(result.apertureFStop,layer.profile.apertureFStop,w);
        if(mask&CameraPostColorTint){result.colorTint.x=mix(result.colorTint.x,layer.profile.colorTint.x,w);result.colorTint.y=mix(result.colorTint.y,layer.profile.colorTint.y,w);result.colorTint.z=mix(result.colorTint.z,layer.profile.colorTint.z,w);}
        const CameraCinematicSettings blended=blend_cinematic(result.cinematic,layer.profile.cinematic,w);
        if(mask&CameraPostColorGrade)result.cinematic.colorGrade=blended.colorGrade;
        if(mask&CameraPostLensEffects)result.cinematic.lens=blended.lens;
        if(mask&CameraPostBokeh)result.cinematic.bokeh=blended.bokeh;
        if(mask&CameraPostSplitDiopter)result.cinematic.splitDiopter=blended.splitDiopter;
        if(mask&CameraPostFilm)result.cinematic.film=blended.film;
    }
    return result;
}

bool CameraAccessibilitySettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{motionScale,shakeScale,bloomScale,maximumDepthOfFieldWeight};
    for(float value:values)if(!std::isfinite(value))return fail("camera accessibility value is not finite");
    if(motionScale<0.0F||motionScale>2.0F||shakeScale<0.0F||shakeScale>2.0F||
       bloomScale<0.0F||bloomScale>2.0F||maximumDepthOfFieldWeight<0.0F||maximumDepthOfFieldWeight>1.0F)
        return fail("camera accessibility value is outside supported range");
    return true;
}

bool CameraViewportDesc::validate(std::string* error) const {
    auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};
    if(id==0U||name.empty()||renderTarget.empty()) return fail("viewport requires id, name, and render target");
    const float values[]{normalizedX,normalizedY,normalizedWidth,normalizedHeight};
    for(float v:values) if(!std::isfinite(v)) return fail("viewport rectangle is not finite");
    if(normalizedX<0.0F||normalizedY<0.0F||normalizedWidth<=0.0F||normalizedHeight<=0.0F||normalizedX+normalizedWidth>1.0001F||normalizedY+normalizedHeight>1.0001F)
        return fail("viewport rectangle is outside normalized output bounds");
    if(outputChannelMask==0U) return fail("viewport output channel cannot be zero");
    return true;
}

CameraMatrix4 multiply(CameraMatrix4 a, CameraMatrix4 b) noexcept {
    CameraMatrix4 r{};
    for(int row=0;row<4;++row) for(int col=0;col<4;++col) {
        float sum=0.0F; for(int k=0;k<4;++k) sum+=a.values[static_cast<std::size_t>(row*4+k)]*b.values[static_cast<std::size_t>(k*4+col)];
        r.values[static_cast<std::size_t>(row*4+col)]=sum;
    }
    return r;
}

CameraMatrix4 camera_view_matrix(const CameraPose& pose) noexcept {
    const Float3 forward=safe_normalize(subtract(pose.target,pose.position),{0,0,-1});
    const Float3 right=safe_normalize(cross3(forward,pose.worldUp),{1,0,0});
    const Float3 up=safe_normalize(cross3(right,forward),{0,1,0});
    CameraMatrix4 r=CameraMatrix4::identity();
    r.values={right.x,right.y,right.z,-dot(right,pose.position),
              up.x,up.y,up.z,-dot(up,pose.position),
              -forward.x,-forward.y,-forward.z,dot(forward,pose.position),
              0,0,0,1};
    return r;
}

// 2026-07-25: the anamorphic squeeze must not appear here at all.
//
// The projection aspect is the *raster* aspect. Multiplying it by the squeeze maps a squeeze
// times wider horizontal field into an unchanged raster width, so pixels stop being square and
// every rendered circle comes out with a horizontal-to-vertical semi-axis ratio of exactly
// 1/squeeze. At 2x that is characters rendered half as wide as they should be. That is the
// squeezed negative, and in a real workflow the projector optics undo it; there is no such step
// here, so it reaches the display uncorrected.
//
// The anamorphic horizontal field is already expressed exactly by the delivery aspect, with no
// projection change needed: vertical field comes from the sensor height and focal length, and
// horizontal field comes from the raster aspect. Setting the viewport to the anamorphic filmback
// aspect (sensor width times squeeze, over sensor height; see
// camera_anamorphic_filmback_aspect_ratio) reproduces the scene field of the real lens to within
// float precision, with square pixels. Anamorphic 35 at 40 mm and 2x: vertical 26.18 degrees,
// horizontal 57.51 degrees, tan-space aspect 2.360, which is the 2.39 scope frame.
//
// So the squeeze is a filmback and post-process property (oval bokeh, horizontal flare, barrel
// distortion), not a frustum property.
CameraMatrix4 camera_projection_matrix(const CameraLens& lens) noexcept {
    CameraMatrix4 r{};
    const float nearPlane=std::max(0.0001F,lens.nearPlaneMeters), farPlane=std::max(nearPlane+0.0001F,lens.farPlaneMeters);
    const float aspect=std::max(0.001F,lens.aspectRatio);
    if(lens.projection==CameraProjection::Orthographic) {
        const float h=std::max(0.001F,lens.orthographicHeightMeters), w=h*aspect;
        r.values={2.0F/w,0,0,0, 0,2.0F/h,0,0, 0,0,1.0F/(nearPlane-farPlane),nearPlane/(nearPlane-farPlane), 0,0,0,1};
    } else {
        const float f=1.0F/std::tan(std::clamp(lens.effective_vertical_field_of_view_radians(),0.01F,3.13F)*0.5F);
        r.values={f/aspect,0,0,0, 0,f,0,0, 0,0,farPlane/(nearPlane-farPlane),(farPlane*nearPlane)/(nearPlane-farPlane), 0,0,-1,0};
    }
    return r;
}

// 2026-07-25: the anamorphic field is a filmback property. A 2x squeeze on an Anamorphic 35
// negative covers the horizontal scene field of a 43.9 mm wide spherical negative, so the
// delivery aspect is sensor width times squeeze over sensor height. Drive the viewport aspect
// from this and the projection needs no anamorphic special case.
float camera_anamorphic_filmback_aspect_ratio(const CameraPhysicalLens& lens,
                                              float anamorphicSqueeze) noexcept {
    const float squeeze=std::isfinite(anamorphicSqueeze)?std::clamp(anamorphicSqueeze,0.25F,4.0F):1.0F;
    const float width=std::max(0.001F,lens.sensorWidthMillimeters)*squeeze;
    const float height=std::max(0.001F,lens.sensorHeightMillimeters);
    return std::clamp(width/height,0.001F,100.0F);
}

CameraDepthOfFieldRange camera_depth_of_field_range(const CameraPhysicalLens& lens,float cocMillimeters) noexcept {
    CameraDepthOfFieldRange r{};
    const float f=std::max(0.001F,lens.focalLengthMillimeters)*0.001F;
    const float n=std::max(0.5F,lens.apertureFStop);
    // 2026-07-25: the acceptable circle of confusion is a property of the format, conventionally
    // the frame diagonal over 1500. The previous fixed 0.03 mm default is the full frame 35 value
    // and was applied to every filmback, so the hyperfocal and near/far readouts were wrong for
    // all of the others: 3.1x too permissive on Super 16 (0.0097 mm) and 2x too strict on IMAX
    // 15-perf (0.0586 mm). A non-positive argument now means "derive from the filmback".
    const float diagonal=std::sqrt(lens.sensorWidthMillimeters*lens.sensorWidthMillimeters+
                                   lens.sensorHeightMillimeters*lens.sensorHeightMillimeters);
    const float resolved=cocMillimeters>0.0F?cocMillimeters:std::max(0.001F,diagonal/1500.0F);
    const float coc=std::max(0.001F,resolved)*0.001F;
    const float focus=std::max(f+0.0001F,lens.focusDistanceMeters);
    r.hyperfocalDistanceMeters=(f*f)/(n*coc)+f;
    r.nearFocusMeters=(r.hyperfocalDistanceMeters*focus)/(r.hyperfocalDistanceMeters+(focus-f));
    const float denominator=r.hyperfocalDistanceMeters-(focus-f);
    r.farFocusMeters=denominator>1.0e-6F?(r.hyperfocalDistanceMeters*focus)/denominator:std::numeric_limits<float>::infinity();
    return r;
}
CameraFrameGuides camera_frame_guides(float viewportAspect,float targetAspect) noexcept {
    CameraFrameGuides r{};viewportAspect=std::max(0.001F,viewportAspect);targetAspect=std::max(0.001F,targetAspect);
    if(viewportAspect>targetAspect) r.insetX=(1.0F-targetAspect/viewportAspect)*0.5F;
    else r.insetY=(1.0F-viewportAspect/targetAspect)*0.5F;
    return r;
}
bool camera_sphere_visible(const CameraPose& pose,Float3 center,float radius) noexcept {
    radius=std::max(0.0F,radius);const Float3 forward=safe_normalize(subtract(pose.target,pose.position),{0,0,-1});const Float3 right=safe_normalize(cross3(forward,pose.worldUp),{1,0,0});const Float3 up=safe_normalize(cross3(right,forward),{0,1,0});const Float3 delta=subtract(center,pose.position);const float z=dot(delta,forward),x=dot(delta,right),y=dot(delta,up);
    if(z+radius<pose.lens.nearPlaneMeters||z-radius>pose.lens.farPlaneMeters)return false;
    if(pose.lens.projection==CameraProjection::Orthographic){const float halfH=pose.lens.orthographicHeightMeters*0.5F,halfW=halfH*pose.lens.aspectRatio;return std::abs(x)<=halfW+radius&&std::abs(y)<=halfH+radius;}
    if(z<=0.0F) return radius>length(delta);
    const float halfH=z*std::tan(pose.lens.effective_vertical_field_of_view_radians()*0.5F);
    const float halfW=halfH*pose.lens.aspectRatio;
    return std::abs(x)<=halfW+radius&&std::abs(y)<=halfH+radius;
}
float camera_projected_sphere_radius_pixels(const CameraPose& pose,Float3 center,float radius,float viewportHeight) noexcept {
    if(!(radius>0.0F)||!(viewportHeight>0.0F)) return 0.0F;
    if(pose.lens.projection==CameraProjection::Orthographic)
        return radius/std::max(0.001F,pose.lens.orthographicHeightMeters)*viewportHeight;
    const Float3 forward=safe_normalize(subtract(pose.target,pose.position),{0,0,-1});const float depth=dot(subtract(center,pose.position),forward);if(depth<=0.0F)return 0.0F;const float focalPixels=viewportHeight/(2.0F*std::tan(pose.lens.effective_vertical_field_of_view_radians()*0.5F));return radius/depth*focalPixels;
}

CameraGpuPacket pack_camera_gpu_packet(const CameraViewportDesc& viewport,const CameraPose& pose,const CameraPostProcessProfile& post,std::uint32_t cutGeneration,bool resetTemporalHistory) noexcept {
    CameraGpuPacket p{}; const CameraMatrix4 view=camera_view_matrix(pose), projection=camera_projection_matrix(pose.lens), vp=multiply(projection,view);
    p.view=view.values;p.projection=projection.values;p.viewProjection=vp.values;
    p.cameraPosition={pose.position.x,pose.position.y,pose.position.z,1.0F};
    p.viewportRect={viewport.normalizedX,viewport.normalizedY,viewport.normalizedWidth,viewport.normalizedHeight};
    p.lens={pose.lens.effective_vertical_field_of_view_radians(),pose.lens.nearPlaneMeters,pose.lens.farPlaneMeters,pose.lens.physical.enabled?1.0F:0.0F};
    p.physicalLens0={pose.lens.physical.focalLengthMillimeters,pose.lens.physical.sensorWidthMillimeters,
                     pose.lens.physical.sensorHeightMillimeters,pose.lens.physical.lensShiftX};
    p.physicalLens1={pose.lens.physical.lensShiftY,static_cast<float>(pose.lens.physical.gateFit),0.0F,0.0F};
    p.postProcess0={post.exposure,post.bloomIntensity,post.saturation,post.contrast};
    p.postProcess1={post.vignette,post.depthOfFieldWeight,post.focusDistanceMeters,post.apertureFStop};
    p.colorTint={post.colorTint.x,post.colorTint.y,post.colorTint.z,pose.postProcessWeight};
    const auto& cinematic=post.cinematic;
    p.colorGrade0={cinematic.colorGrade.temperatureKelvin,cinematic.colorGrade.tint,
                   cinematic.colorGrade.lutBlend,static_cast<float>(cinematic.colorGrade.toneMap)};
    p.colorGradeLift={cinematic.colorGrade.lift.x,cinematic.colorGrade.lift.y,cinematic.colorGrade.lift.z,0.0F};
    p.colorGradeGamma={cinematic.colorGrade.gamma.x,cinematic.colorGrade.gamma.y,cinematic.colorGrade.gamma.z,0.0F};
    p.colorGradeGain={cinematic.colorGrade.gain.x,cinematic.colorGrade.gain.y,cinematic.colorGrade.gain.z,0.0F};
    p.lensEffects0={static_cast<float>(cinematic.lens.distortionModel),cinematic.lens.radialK1,
                    cinematic.lens.radialK2,cinematic.lens.radialK3};
    p.lensEffects1={cinematic.lens.fisheyeStrength,cinematic.lens.chromaticAberrationPixels,
                    cinematic.lens.anamorphicSqueeze,cinematic.lens.lensBreathing};
    p.lensEffects2={cinematic.lens.anamorphicFlareIntensity,cinematic.lens.anamorphicFlareThreshold,
                    cinematic.lens.gateWeavePixels,0.0F};
    p.bokeh={static_cast<float>(cinematic.bokeh.bladeCount),cinematic.bokeh.bladeRotationRadians,
             cinematic.bokeh.roundness,cinematic.bokeh.anamorphicRatio};
    p.splitDiopter0={cinematic.splitDiopter.enabled?1.0F:0.0F,
                     cinematic.splitDiopter.nearFocusDistanceMeters,
                     cinematic.splitDiopter.farFocusDistanceMeters,
                     cinematic.splitDiopter.featherFraction};
    p.splitDiopter1={cinematic.splitDiopter.centerX,cinematic.splitDiopter.centerY,
                     cinematic.splitDiopter.angleRadians,cinematic.bokeh.catEye};
    p.film0={cinematic.film.grainIntensity,cinematic.film.halationIntensity,
             cinematic.film.sharpenIntensity,cinematic.film.motionBlurWeight};
    p.film1={cinematic.film.shutterAngleDegrees,static_cast<float>(cinematic.film.grainSeed),
             static_cast<float>(cinematic.film.framing),cinematic.film.customAspectRatio};
    p.matteColor={cinematic.film.matteColor.x,cinematic.film.matteColor.y,
                  cinematic.film.matteColor.z,cinematic.film.matteOpacity};
    p.outputChannelMask=viewport.outputChannelMask;p.cameraCutGeneration=cutGeneration;p.resetTemporalHistory=resetTemporalHistory?1U:0U;
    return p;
}

bool GameWorldCameraCollisionWorld::sweep_sphere(Float3 start,Float3 end,float radiusMeters,CameraCollisionHit& out) const {
    if(!world_||!std::isfinite(radiusMeters)||radiusMeters<0.0F) return false;
    const Float3 delta=subtract(end,start); const float distance=length(delta); if(!(distance>kEpsilon)) return false;
    const Float3 direction=multiply(delta,1.0F/distance);
    const Float3 axis=std::abs(direction.y)<0.9F?Float3{0,1,0}:Float3{1,0,0};
    const Float3 right=safe_normalize(cross3(direction,axis),{1,0,0}); const Float3 up=safe_normalize(cross3(right,direction),{0,1,0});
    const Float3 offsets[]{ {0,0,0},multiply(right,radiusMeters),multiply(right,-radiusMeters),multiply(up,radiusMeters),multiply(up,-radiusMeters)};
    float best=std::numeric_limits<float>::infinity(); std::optional<GameRaycastHit> bestHit;
    for(Float3 offset:offsets) {
        const auto hit=world_->raycast(add(start,offset),direction,distance); if(hit&&hit->distance<best){best=hit->distance;bestHit=hit;}
    }
    if(!bestHit) return false;
    out.position=add(start,multiply(direction,best));
    out.normal=bestHit->worldNormal;
    out.fraction=std::clamp(best/distance,0.0F,1.0F);
    out.objectId=bestHit->objectId;
    out.materialId=static_cast<std::uint32_t>(bestHit->material);
    return true;
}

GameCameraRuntime::GameCameraRuntime() = default;
GameCameraRuntime::~GameCameraRuntime() = default;

bool GameCameraRuntime::add_viewport(CameraViewportDesc desc,std::string* error) {
    if(!desc.validate(error)) return false;
    if(viewports_.contains(desc.id)){if(error)*error="duplicate camera viewport id";return false;}
    ViewportState s{};
    s.desc=std::move(desc);
    s.director.set_channel_mask(s.desc.outputChannelMask);
    s.director.set_reduced_motion(
        reducedMotion_ || accessibility_.preset != CameraAccessibilityPreset::Standard);
    float shake=accessibility_.shakeScale;
    if(accessibility_.preset==CameraAccessibilityPreset::ReducedMotion)shake*=0.2F;
    if(accessibility_.preset==CameraAccessibilityPreset::Photosensitive)shake=0.0F;
    s.director.set_shake_scale(shake);
    s.frame.viewport=s.desc;
    viewports_.emplace(s.desc.id,std::move(s));
    return true;
}
bool GameCameraRuntime::remove_viewport(CameraViewportId id) noexcept{return viewports_.erase(id)!=0U;}
bool GameCameraRuntime::update_viewport(CameraViewportDesc desc,std::string* error){
    if(!desc.validate(error))return false;
    auto it=viewports_.find(desc.id);
    if(it==viewports_.end()){if(error)*error="unknown camera viewport id";return false;}
    it->second.desc=std::move(desc);
    it->second.director.set_channel_mask(it->second.desc.outputChannelMask);
    it->second.frame.viewport=it->second.desc;
    return true;
}
CameraDirector* GameCameraRuntime::director(CameraViewportId id) noexcept {auto it=viewports_.find(id);return it==viewports_.end()?nullptr:&it->second.director;}
const CameraDirector* GameCameraRuntime::director(CameraViewportId id) const noexcept {auto it=viewports_.find(id);return it==viewports_.end()?nullptr:&it->second.director;}
const CameraViewportFrame* GameCameraRuntime::frame(CameraViewportId id) const noexcept {auto it=viewports_.find(id);return it==viewports_.end()?nullptr:&it->second.frame;}
std::vector<CameraViewportId> GameCameraRuntime::viewport_ids() const {std::vector<CameraViewportId> r;for(auto&[id,_]:viewports_)r.push_back(id);return r;}
bool GameCameraRuntime::bind_target(CameraViewportId v,CameraTargetId t,GameObjectId o) noexcept {auto it=viewports_.find(v);if(it==viewports_.end()||t==0U||o==0U)return false;it->second.targetBindings[t]=o;return true;}
bool GameCameraRuntime::unbind_target(CameraViewportId v,CameraTargetId t) noexcept {auto it=viewports_.find(v);return it!=viewports_.end()&&it->second.targetBindings.erase(t)!=0U;}
bool GameCameraRuntime::set_post_process(CameraViewportId v,CameraRigId r,CameraPostProcessProfile p,std::string* error){
    CameraPostProcessStack stack; stack.layers.push_back({"Base", p, 1.0F, true});
    return set_post_process_stack(v, r, std::move(stack), error);
}
bool GameCameraRuntime::set_post_process_stack(CameraViewportId v, CameraRigId r,
                                               CameraPostProcessStack stack,
                                               std::string* error) {
    if (!stack.validate(error)) return false;
    auto it = viewports_.find(v);
    if (it == viewports_.end() || r == 0U) {
        if (error) *error = "unknown viewport or rig";
        return false;
    }
    it->second.postProcessStacks[r] = std::move(stack);
    return true;
}
bool GameCameraRuntime::set_sequence(CameraViewportId id,const CameraSequence& sequence,std::string* error){if(!sequence.validate(error))return false;auto it=viewports_.find(id);if(it==viewports_.end()){if(error)*error="unknown camera viewport";return false;}it->second.sequence=std::make_shared<CameraSequence>(sequence);it->second.sequencePlayer=std::make_unique<CameraSequencePlayer>();it->second.sequencePlayer->set_sequence(it->second.sequence.get());it->second.previousShotId.reset();return true;}
bool GameCameraRuntime::play_sequence(CameraViewportId id,bool loop) noexcept{auto it=viewports_.find(id);if(it==viewports_.end()||!it->second.sequencePlayer)return false;it->second.sequencePlayer->play(loop);return true;}
bool GameCameraRuntime::pause_sequence(CameraViewportId id) noexcept{auto it=viewports_.find(id);if(it==viewports_.end()||!it->second.sequencePlayer)return false;it->second.sequencePlayer->pause();return true;}
bool GameCameraRuntime::seek_sequence(CameraViewportId id,float t) noexcept{auto it=viewports_.find(id);if(it==viewports_.end()||!it->second.sequencePlayer)return false;it->second.sequencePlayer->seek(t);return true;}
bool GameCameraRuntime::stop_sequence(CameraViewportId id) noexcept{auto it=viewports_.find(id);if(it==viewports_.end()||!it->second.sequencePlayer)return false;it->second.sequencePlayer->stop();return true;}
float GameCameraRuntime::sequence_time(CameraViewportId id) const noexcept{auto it=viewports_.find(id);return it==viewports_.end()||!it->second.sequencePlayer?0.0F:it->second.sequencePlayer->time_seconds();}
bool GameCameraRuntime::sequence_playing(CameraViewportId id) const noexcept{auto it=viewports_.find(id);return it!=viewports_.end()&&it->second.sequencePlayer&&it->second.sequencePlayer->playing();}
void GameCameraRuntime::clear_sequence(CameraViewportId id) noexcept{auto it=viewports_.find(id);if(it==viewports_.end())return;it->second.sequencePlayer.reset();it->second.sequence.reset();it->second.previousShotId.reset();}
void GameCameraRuntime::set_reduced_motion(bool reduced) noexcept {
    reducedMotion_=reduced;
    const bool effective = reduced ||
        accessibility_.preset != CameraAccessibilityPreset::Standard;
    for(auto&[_,v]:viewports_)v.director.set_reduced_motion(effective);
}
bool GameCameraRuntime::set_accessibility(CameraAccessibilitySettings settings,std::string* error) {
    if(!settings.validate(error))return false;
    accessibility_=settings;
    const bool reduced=reducedMotion_||settings.preset!=CameraAccessibilityPreset::Standard;
    for(auto&[_,v]:viewports_){
        v.director.set_reduced_motion(reduced);
        float shake=settings.shakeScale;
        if(settings.preset==CameraAccessibilityPreset::ReducedMotion)shake*=0.2F;
        if(settings.preset==CameraAccessibilityPreset::Photosensitive)shake=0.0F;
        v.director.set_shake_scale(shake);
    }
    return true;
}

std::string GameCameraRuntime::serialize_state() const {
    std::ostringstream out;
    out << "DVE_CAMERA_RUNTIME_STATE 2\n";
    out << "accessibility " << static_cast<int>(accessibility_.preset) << ' '
        << accessibility_.horizonLock << ' ' << accessibility_.motionScale << ' '
        << accessibility_.shakeScale << ' ' << accessibility_.bloomScale << ' '
        << accessibility_.maximumDepthOfFieldWeight << '\n';
    out << viewports_.size() << '\n';
    out << std::setprecision(9);
    for (const auto& [id, viewport] : viewports_) {
        const auto telemetry = viewport.director.telemetry();
        out << id << ' ' << telemetry.liveRig << ' ' << std::quoted(std::string(viewport.director.state())) << ' '
            << (viewport.sequencePlayer ? 1 : 0) << ' '
            << (viewport.sequencePlayer ? viewport.sequencePlayer->time_seconds() : 0.0F) << ' '
            << (viewport.sequencePlayer && viewport.sequencePlayer->playing() ? 1 : 0) << ' '
            << viewport.cutGeneration << '\n';
    }
    return out.str();
}

bool GameCameraRuntime::restore_state(std::string_view text, std::string* error) {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    std::istringstream in{std::string(text)};
    std::string magic; int version{}; std::size_t count{};
    if(!(in>>magic>>version)||magic!="DVE_CAMERA_RUNTIME_STATE"||(version!=1&&version!=2))
        return fail("unsupported camera runtime state");
    CameraAccessibilitySettings restoredAccessibility=accessibility_;
    if(version==2){
        std::string key;int preset{};
        if(!(in>>key>>preset>>restoredAccessibility.horizonLock>>restoredAccessibility.motionScale
             >>restoredAccessibility.shakeScale>>restoredAccessibility.bloomScale
             >>restoredAccessibility.maximumDepthOfFieldWeight)||key!="accessibility"||preset<0||preset>2)
            return fail("malformed camera accessibility state");
        restoredAccessibility.preset=static_cast<CameraAccessibilityPreset>(preset);
        if(!restoredAccessibility.validate(error))return false;
    }
    if(!(in>>count)||count>4096U) return fail("invalid camera runtime viewport count");
    struct Entry{CameraViewportId id{};CameraRigId live{};std::string state;bool hasSequence{};float time{};bool playing{};std::uint32_t cut{};};
    std::vector<Entry> entries; entries.reserve(count);
    for(std::size_t i=0;i<count;++i){Entry entry;int has{},playing{};if(!(in>>entry.id>>entry.live>>std::quoted(entry.state)>>has>>entry.time>>playing>>entry.cut)||(has!=0&&has!=1)||(playing!=0&&playing!=1)||!std::isfinite(entry.time)||entry.time<0.0F)return fail("malformed camera runtime state entry");entry.hasSequence=has!=0;entry.playing=playing!=0;if(!viewports_.contains(entry.id))return fail("camera runtime state references an unknown viewport");if(entry.hasSequence&&!viewports_.at(entry.id).sequencePlayer)return fail("camera runtime state requires a sequence that is not loaded");entries.push_back(std::move(entry));}
    in>>std::ws;if(!in.eof())return fail("trailing camera runtime state data");
    for(const Entry& entry:entries){const auto& viewport=viewports_.at(entry.id);if(entry.live!=0U&&viewport.director.find_rig(entry.live)==nullptr)return fail("camera runtime state references an unknown rig");if(!entry.state.empty()&&!viewport.director.has_state_binding(entry.state))return fail("camera runtime state references an unknown camera state");}
    if(!set_accessibility(restoredAccessibility,error))return false;
    for(const Entry& entry:entries){auto& viewport=viewports_.at(entry.id);if(entry.live!=0U)(void)viewport.director.force_live(entry.live,true);if(!entry.state.empty())(void)viewport.director.set_state(entry.state);if(entry.hasSequence){viewport.sequencePlayer->seek(entry.time);if(entry.playing)viewport.sequencePlayer->play(false);else viewport.sequencePlayer->pause();}viewport.cutGeneration=entry.cut;}
    return true;
}

void GameCameraRuntime::update(const GameWorld& world,float elapsedSeconds) {
    GameWorldCameraCollisionWorld collision(world);
    for(auto&[_,v]:viewports_) {
        if(!v.desc.enabled) continue;
        for(const auto&[targetId,objectId]:v.targetBindings) {
            const auto transform=world.transform(objectId); if(!transform){v.director.remove_target(targetId);continue;}
            CameraTargetState state{};state.position=transform->position;state.forward=rotate(transform->rotation,{0,0,-1});state.up=rotate(transform->rotation,{0,1,0});state.velocity=world.linear_velocity(objectId).value_or(Float3{});v.director.set_target(targetId,state);
        }
        CameraSequenceSample sequenceSample{};
        if(v.sequencePlayer) {
            sequenceSample=v.sequencePlayer->update(elapsedSeconds);
            if(sequenceSample.shotId!=v.previousShotId) {
                if(sequenceSample.rigId) {
                    v.director.set_default_blend(v.sequence->shots.empty()?CameraBlend{}:([&]{for(const auto& shot:v.sequence->shots)if(shot.id==*sequenceSample.shotId)return shot.blendIn;return CameraBlend{};})());
                    (void)v.director.force_live(*sequenceSample.rigId,sequenceSample.cut);
                }
                if(!sequenceSample.stateTrigger.empty()) (void)v.director.set_state(sequenceSample.stateTrigger);
                v.previousShotId=sequenceSample.shotId;
            }
        }
        const float cameraElapsed=elapsedSeconds*std::clamp(accessibility_.motionScale,0.0F,2.0F);
        CameraPose pose=v.director.update(cameraElapsed,&collision);const auto telemetry=v.director.telemetry();
        bool cut=telemetry.liveRig!=v.previousLiveRig && (telemetry.outgoingRig==0U||telemetry.blendWeight>=1.0F);
        if(sequenceSample.pose) {pose=*sequenceSample.pose;cut=cut||(sequenceSample.cut&&sequenceSample.shotId!=v.frame.activeShotId);}
        if(cut) ++v.cutGeneration;
        v.previousLiveRig=telemetry.liveRig;
        CameraPostProcessProfile post{};
        if(const auto it=v.postProcessStacks.find(telemetry.liveRig);it!=v.postProcessStacks.end()) post=it->second.evaluate();
        if(telemetry.outgoingRig!=0U && telemetry.blendWeight<1.0F) {
            CameraPostProcessProfile outgoing{};
            if(const auto it=v.postProcessStacks.find(telemetry.outgoingRig);it!=v.postProcessStacks.end()) outgoing=it->second.evaluate();
            post=blend_camera_post_process(outgoing,post,telemetry.blendWeight);
        }
        if(sequenceSample.pose) post=sequenceSample.postProcess;
        if(accessibility_.horizonLock)pose.worldUp={0.0F,1.0F,0.0F};
        float bloomScale=accessibility_.bloomScale;
        if(accessibility_.preset==CameraAccessibilityPreset::Photosensitive)bloomScale=std::min(bloomScale,0.15F);
        post.bloomIntensity*=bloomScale;
        post.depthOfFieldWeight=std::min(post.depthOfFieldWeight,accessibility_.maximumDepthOfFieldWeight);
        if(accessibility_.preset==CameraAccessibilityPreset::Photosensitive){
            post.depthOfFieldWeight=std::min(post.depthOfFieldWeight,0.25F);
            post.vignette=std::min(post.vignette,0.2F);
            post.cinematic.lens.anamorphicFlareIntensity=std::min(post.cinematic.lens.anamorphicFlareIntensity,0.15F);
            post.cinematic.film.motionBlurWeight=std::min(post.cinematic.film.motionBlurWeight,0.25F);
            post.cinematic.film.grainIntensity=std::min(post.cinematic.film.grainIntensity,0.35F);
        }
        // The physical lens is authoritative for focus and aperture only when it is actually
        // enabled. This ran unconditionally, so (a) a camera with no physical lens had its
        // authored post-process focus and aperture replaced by the physical defaults of 10 m and
        // f/2.8, silently overriding presets such as SplitDiopter which sets f/2.0, and (b) it ran
        // after the sequencer sample was applied, so any focus pull keyframed on the post-process
        // profile was discarded.
        if(pose.lens.physical.enabled){
            post.focusDistanceMeters=pose.lens.physical.focusDistanceMeters;
            post.apertureFStop=pose.lens.physical.apertureFStop;
        }
        v.frame={v.desc,pose,post,pack_camera_gpu_packet(v.desc,pose,post,v.cutGeneration,cut),sequenceSample.shotId,sequenceSample.marker,{}};
        for(const auto& event:sequenceSample.triggeredEvents)
            v.frame.triggeredEvents.push_back({event.id,event.name,event.payload});
    }
}

} // namespace dve::camera
