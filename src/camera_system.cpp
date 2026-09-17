#include "dve/camera_system.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace dve::camera {
namespace {

constexpr float kEpsilon = 1.0e-6F;

Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Float3 normalized(Float3 value, Float3 fallback) noexcept {
    const float squared = length_squared(value);
    if (!(squared > 1.0e-12F) || !std::isfinite(squared)) return fallback;
    return multiply(value, 1.0F/std::sqrt(squared));
}
Float3 lerp(Float3 a, Float3 b, float t) noexcept { return add(a, multiply(subtract(b,a),t)); }
float lerp(float a,float b,float t) noexcept { return a+(b-a)*t; }
float smoothing_alpha(float elapsed,float damping) noexcept {
    if (!(elapsed>0.0F)) return 0.0F;
    if (!(damping>1.0e-5F)) return 1.0F;
    return 1.0F-std::exp(-elapsed/damping);
}
float blend_weight(CameraBlendCurve curve,float t) noexcept {
    t=std::clamp(t,0.0F,1.0F);
    switch(curve){
        case CameraBlendCurve::Cut:return 1.0F;
        case CameraBlendCurve::Linear:return t;
        case CameraBlendCurve::EaseIn:return t*t;
        case CameraBlendCurve::EaseOut:{const float u=1.0F-t;return 1.0F-u*u;}
        case CameraBlendCurve::EaseInOut:return t<0.5F?2.0F*t*t:1.0F-2.0F*(1.0F-t)*(1.0F-t);
        case CameraBlendCurve::SmoothStep:return t*t*(3.0F-2.0F*t);
    }
    return t;
}
CameraLens blend_lens(const CameraLens& a,const CameraLens& b,float t) noexcept {
    CameraLens result=t<0.5F?a:b;
    result.verticalFieldOfViewRadians=blend_camera_vertical_fov(a.verticalFieldOfViewRadians,b.verticalFieldOfViewRadians,t);
    result.orthographicHeightMeters=lerp(a.orthographicHeightMeters,b.orthographicHeightMeters,t);
    result.nearPlaneMeters=lerp(a.nearPlaneMeters,b.nearPlaneMeters,t);
    result.farPlaneMeters=lerp(a.farPlaneMeters,b.farPlaneMeters,t);
    result.aspectRatio=lerp(a.aspectRatio,b.aspectRatio,t);
    result.physical=t<0.5F?a.physical:b.physical;
    // Angular field is what the eye tracks and it is roughly logarithmic in focal length, so a
    // 24 mm to 200 mm move interpolated linearly in millimetres spends most of the blend already
    // at the telephoto end. Focus is linear in diopters and aperture is logarithmic in stops for
    // the same reason: those are the units the physical controls are graduated in.
    result.physical.focalLengthMillimeters=blend_camera_focal_length(a.physical.focalLengthMillimeters,b.physical.focalLengthMillimeters,t);
    result.physical.sensorWidthMillimeters=lerp(a.physical.sensorWidthMillimeters,b.physical.sensorWidthMillimeters,t);
    result.physical.sensorHeightMillimeters=lerp(a.physical.sensorHeightMillimeters,b.physical.sensorHeightMillimeters,t);
    result.physical.lensShiftX=lerp(a.physical.lensShiftX,b.physical.lensShiftX,t);
    result.physical.lensShiftY=lerp(a.physical.lensShiftY,b.physical.lensShiftY,t);
    result.physical.apertureFStop=blend_camera_f_stop(a.physical.apertureFStop,b.physical.apertureFStop,t);
    result.physical.focusDistanceMeters=blend_camera_focus_distance(a.physical.focusDistanceMeters,b.physical.focusDistanceMeters,t);
    return result;
}
CameraPose blend_pose(const CameraPose& a,const CameraPose& b,float t) noexcept {
    CameraPose result;
    result.position=lerp(a.position,b.position,t);
    result.target=lerp(a.target,b.target,t);
    result.worldUp=normalized(lerp(a.worldUp,b.worldUp,t),{0,1,0});
    result.lens=blend_lens(a.lens,b.lens,t);
    result.postProcessWeight=lerp(a.postProcessWeight,b.postProcessWeight,t);
    return result;
}
float finite_or(float value,float fallback) noexcept { return std::isfinite(value)?value:fallback; }
// Integer lattice hash. sin()-based hashing loses precision badly once the argument grows, which
// for a shake that runs for minutes means the noise becomes visibly correlated.
float lattice_hash(std::uint32_t seed,std::int32_t step,std::uint32_t axis) noexcept {
    std::uint32_t value=static_cast<std::uint32_t>(step)*0x9e3779b9U+seed*0x85ebca6bU+axis*0x27d4eb2fU;
    value^=value>>16U;value*=0x7feb352dU;value^=value>>15U;value*=0x846ca68bU;value^=value>>16U;
    return static_cast<float>(value&0x00ffffffU)/16777215.0F*2.0F-1.0F;
}
// Smoothly interpolated value noise. The previous implementation hashed a continuously varying
// argument, so consecutive frames were uncorrelated: that is white noise, which reads as buzz and
// whose apparent character changes with frame rate. CameraShakePattern::PerlinLikeNoise is
// supposed to be smooth, and handheld motion is smooth.
float hash_noise(std::uint32_t seed,float time,float axis) noexcept {
    const auto axisIndex=static_cast<std::uint32_t>(std::max(0.0F,axis));
    const float floored=std::floor(time);
    const auto step=static_cast<std::int32_t>(floored);
    const float fraction=time-floored;
    const float smooth=fraction*fraction*(3.0F-2.0F*fraction);
    const float a=lattice_hash(seed,step,axisIndex);
    const float b=lattice_hash(seed,step+1,axisIndex);
    const float coarse=lattice_hash(seed*7919U+1U,step/4,axisIndex);
    const float fine=a+(b-a)*smooth;
    return std::clamp(fine*0.75F+coarse*0.25F,-1.0F,1.0F);
}
float shake_envelope(const CameraShake& shake,float elapsed) noexcept {
    float result=1.0F;
    if(shake.blendInSeconds>0.0F) result=std::min(result,elapsed/shake.blendInSeconds);
    if(shake.durationSeconds>0.0F&&shake.blendOutSeconds>0.0F){
        const float remaining=shake.durationSeconds-elapsed;
        result=std::min(result,remaining/shake.blendOutSeconds);
    }
    return std::clamp(result,0.0F,1.0F);
}
Float3 target_right(const CameraTargetState& target) noexcept {
    return normalized(cross(normalized(target.forward,{0,0,-1}),normalized(target.up,{0,1,0})),{1,0,0});
}
Float3 rotate_basis_offset(const CameraTargetState& target,Float3 local) noexcept {
    const Float3 forward=normalized(target.forward,{0,0,-1});
    const Float3 up=normalized(target.up,{0,1,0});
    const Float3 right=target_right(target);
    return add(add(multiply(right,local.x),multiply(up,local.y)),multiply(forward,local.z));
}
std::string mode_name(CameraRigMode mode){
    switch(mode){case CameraRigMode::Fixed:return"Fixed";case CameraRigMode::FreeFly:return"FreeFly";
    case CameraRigMode::Orbit:return"Orbit";case CameraRigMode::Follow:return"Follow";
    case CameraRigMode::ThirdPerson:return"ThirdPerson";case CameraRigMode::FirstPerson:return"FirstPerson";
    case CameraRigMode::Cinematic:return"Cinematic";}return"Fixed";
}
std::optional<CameraRigMode> parse_mode(std::string_view value) {
    if (value == "Fixed") return CameraRigMode::Fixed;
    if (value == "FreeFly") return CameraRigMode::FreeFly;
    if (value == "Orbit") return CameraRigMode::Orbit;
    if (value == "Follow") return CameraRigMode::Follow;
    if (value == "ThirdPerson") return CameraRigMode::ThirdPerson;
    if (value == "FirstPerson") return CameraRigMode::FirstPerson;
    if (value == "Cinematic") return CameraRigMode::Cinematic;
    return std::nullopt;
}
std::string curve_name(CameraBlendCurve curve){
    switch(curve){case CameraBlendCurve::Cut:return"Cut";case CameraBlendCurve::Linear:return"Linear";
    case CameraBlendCurve::EaseIn:return"EaseIn";case CameraBlendCurve::EaseOut:return"EaseOut";
    case CameraBlendCurve::EaseInOut:return"EaseInOut";case CameraBlendCurve::SmoothStep:return"SmoothStep";}return"Cut";
}
std::optional<CameraBlendCurve> parse_curve(std::string_view value) {
    if (value == "Cut") return CameraBlendCurve::Cut;
    if (value == "Linear") return CameraBlendCurve::Linear;
    if (value == "EaseIn") return CameraBlendCurve::EaseIn;
    if (value == "EaseOut") return CameraBlendCurve::EaseOut;
    if (value == "EaseInOut") return CameraBlendCurve::EaseInOut;
    if (value == "SmoothStep") return CameraBlendCurve::SmoothStep;
    return std::nullopt;
}
std::string obstruction_name(CameraObstructionStrategy strategy) {
    switch (strategy) {
        case CameraObstructionStrategy::PullForward: return "PullForward";
        case CameraObstructionStrategy::PreserveTargetFraming: return "PreserveTargetFraming";
        case CameraObstructionStrategy::FadeOccluders: return "FadeOccluders";
        case CameraObstructionStrategy::ShoulderSwap: return "ShoulderSwap";
    }
    return "PullForward";
}
std::optional<CameraObstructionStrategy> parse_obstruction(std::string_view value) {
    if (value == "PullForward") return CameraObstructionStrategy::PullForward;
    if (value == "PreserveTargetFraming") return CameraObstructionStrategy::PreserveTargetFraming;
    if (value == "FadeOccluders") return CameraObstructionStrategy::FadeOccluders;
    if (value == "ShoulderSwap") return CameraObstructionStrategy::ShoulderSwap;
    return std::nullopt;
}
template<class T> bool parse_number(std::string_view text,T& value){
    const char* begin=text.data();const char* end=text.data()+text.size();
    const auto result=std::from_chars(begin,end,value);return result.ec==std::errc{}&&result.ptr==end;
}

} // namespace

CameraPhysicalLens camera_physical_lens_preset(CameraFilmbackPreset preset,
                                                       float focalLengthMillimeters) noexcept {
    CameraPhysicalLens lens{};
    lens.enabled=true;
    lens.focalLengthMillimeters=std::clamp(focalLengthMillimeters,1.0F,2000.0F);
    switch(preset){
        case CameraFilmbackPreset::Custom:break;
        case CameraFilmbackPreset::Super16:lens.sensorWidthMillimeters=12.52F;lens.sensorHeightMillimeters=7.41F;break;
        case CameraFilmbackPreset::Super35:lens.sensorWidthMillimeters=24.89F;lens.sensorHeightMillimeters=18.66F;break;
        case CameraFilmbackPreset::FullFrame35:lens.sensorWidthMillimeters=36.0F;lens.sensorHeightMillimeters=24.0F;break;
        case CameraFilmbackPreset::Anamorphic35:lens.sensorWidthMillimeters=21.95F;lens.sensorHeightMillimeters=18.60F;break;
        case CameraFilmbackPreset::Imax15Perf:lens.sensorWidthMillimeters=70.41F;lens.sensorHeightMillimeters=52.63F;break;
        case CameraFilmbackPreset::ImaxDigital:lens.sensorWidthMillimeters=54.12F;lens.sensorHeightMillimeters=25.59F;break;
    }
    return lens;
}

bool CameraPhysicalLens::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(!std::isfinite(focalLengthMillimeters)||focalLengthMillimeters<1.0F||focalLengthMillimeters>2000.0F)return fail("physical focal length must be 1 to 2000 mm");
    if(!std::isfinite(sensorWidthMillimeters)||sensorWidthMillimeters<=0.0F||sensorWidthMillimeters>200.0F)return fail("sensor width is invalid");
    if(!std::isfinite(sensorHeightMillimeters)||sensorHeightMillimeters<=0.0F||sensorHeightMillimeters>200.0F)return fail("sensor height is invalid");
    if(!std::isfinite(lensShiftX)||!std::isfinite(lensShiftY)||std::abs(lensShiftX)>4.0F||std::abs(lensShiftY)>4.0F)return fail("lens shift is invalid");
    if(!std::isfinite(apertureFStop)||apertureFStop<0.5F||apertureFStop>64.0F)return fail("aperture must be f/0.5 to f/64");
    if(!std::isfinite(focusDistanceMeters)||focusDistanceMeters<=0.0F||focusDistanceMeters>1000000.0F)return fail("focus distance is invalid");
    return true;
}
float CameraPhysicalLens::vertical_field_of_view_radians(float viewportAspectRatio) const noexcept {
    const float aspect=std::max(0.01F,finite_or(viewportAspectRatio,16.0F/9.0F));
    float sensorHeight=sensorHeightMillimeters;
    switch(gateFit){
        case CameraGateFit::Horizontal:sensorHeight=sensorWidthMillimeters/aspect;break;
        case CameraGateFit::Fill:sensorHeight=std::min(sensorHeightMillimeters,sensorWidthMillimeters/aspect);break;
        case CameraGateFit::Overscan:sensorHeight=std::max(sensorHeightMillimeters,sensorWidthMillimeters/aspect);break;
        case CameraGateFit::Vertical:case CameraGateFit::Stretch:break;
    }
    return 2.0F*std::atan(std::max(0.001F,sensorHeight)/(2.0F*std::max(0.001F,focalLengthMillimeters)));
}
bool CameraLens::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(!std::isfinite(verticalFieldOfViewRadians)||verticalFieldOfViewRadians<1.0F*kDegreesToRadians||verticalFieldOfViewRadians>179.0F*kDegreesToRadians)return fail("vertical field of view must be 1 to 179 degrees");
    if(!std::isfinite(orthographicHeightMeters)||orthographicHeightMeters<=0.0F||orthographicHeightMeters>1000000.0F)return fail("orthographic height is invalid");
    if(!std::isfinite(nearPlaneMeters)||nearPlaneMeters<=0.0F)return fail("near plane must be positive");
    if(!std::isfinite(farPlaneMeters)||farPlaneMeters<=nearPlaneMeters)return fail("far plane must be greater than near plane");
    if(!std::isfinite(aspectRatio)||aspectRatio<=0.0F||aspectRatio>100.0F)return fail("camera aspect ratio is invalid");
    return physical.validate(error);
}
float CameraLens::effective_vertical_field_of_view_radians() const noexcept {
    return physical.enabled?physical.vertical_field_of_view_radians(aspectRatio):verticalFieldOfViewRadians;
}
bool CameraFramingSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{localOffset.x,localOffset.y,localOffset.z,distanceMeters,heightMeters,shoulderOffsetMeters,lookAheadSeconds,screenOffsetX,screenOffsetY,positionDampingSeconds,aimDampingSeconds,deadZoneFraction,softZoneFraction,orbitYawRadians,orbitPitchRadians,minimumPitchRadians,maximumPitchRadians};
    for(float value:values)if(!std::isfinite(value))return fail("camera framing contains a non-finite value");
    if(distanceMeters<0.0F||distanceMeters>100000.0F)return fail("camera distance is invalid");
    if(lookAheadSeconds<0.0F||lookAheadSeconds>10.0F)return fail("camera look-ahead is invalid");
    if(positionDampingSeconds<0.0F||positionDampingSeconds>60.0F||aimDampingSeconds<0.0F||aimDampingSeconds>60.0F)return fail("camera damping is invalid");
    if(deadZoneFraction<0.0F||deadZoneFraction>1.0F||softZoneFraction<deadZoneFraction||softZoneFraction>1.0F)return fail("camera dead/soft zones are invalid");
    if(minimumPitchRadians>=maximumPitchRadians)return fail("minimum camera pitch must be below maximum pitch");
    return true;
}
bool CameraCollisionSettings::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{probeRadiusMeters,minimumTargetDistanceMeters,collisionPullInSeconds,
                         recoverySeconds,shoulderSwapSearchMeters,occluderFadeOpacity};
    for(float value:values)if(!std::isfinite(value)||value<0.0F)return fail("camera collision setting is invalid");
    if(probeRadiusMeters>100.0F||minimumTargetDistanceMeters>1000.0F||collisionPullInSeconds>60.0F||
       recoverySeconds>60.0F||shoulderSwapSearchMeters>100.0F||occluderFadeOpacity>1.0F)
        return fail("camera collision setting is outside supported range");
    return true;
}
bool CameraVolumeConstraint::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    const float values[]{minimum.x,minimum.y,minimum.z,maximum.x,maximum.y,maximum.z,softnessMeters};
    for(float value:values)if(!std::isfinite(value))return fail("camera volume contains a non-finite value");
    if(minimum.x>maximum.x||minimum.y>maximum.y||minimum.z>maximum.z)
        return fail("camera volume minimum exceeds maximum");
    if(softnessMeters<0.0F||softnessMeters>100000.0F)return fail("camera volume softness is invalid");
    return true;
}
bool CameraRig::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(id==0)return fail("camera rig id must be nonzero");
    if(name.empty())return fail("camera rig name is empty");
    if(outputChannels==0U)return fail("camera rig must publish at least one channel");
    if(!lens.validate(error)||!framing.validate(error)||!collision.validate(error)||!volumeConstraint.validate(error))return false;
    if(!std::isfinite(postProcessWeight)||postProcessWeight<0.0F||postProcessWeight>1.0F)return fail("camera post-process weight is invalid");
    const float poseValues[]{authoredPose.position.x,authoredPose.position.y,authoredPose.position.z,authoredPose.target.x,authoredPose.target.y,authoredPose.target.z,authoredPose.worldUp.x,authoredPose.worldUp.y,authoredPose.worldUp.z};
    for(float value:poseValues)if(!std::isfinite(value))return fail("camera rig pose contains a non-finite value");
    if(length_squared(authoredPose.worldUp)<1.0e-10F)return fail("camera rig world-up vector is zero");
    return true;
}
bool CameraBlend::validate(std::string* error) const {
    if(!std::isfinite(durationSeconds)||durationSeconds<0.0F||durationSeconds>3600.0F){if(error)*error="camera blend duration is invalid";return false;}return true;
}
bool CameraShake::validate(std::string* error) const {
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(id==0)return fail("camera shake id must be nonzero");
    const float values[]{positionAmplitudeMeters.x,positionAmplitudeMeters.y,positionAmplitudeMeters.z,rotationAmplitudeRadians.x,rotationAmplitudeRadians.y,rotationAmplitudeRadians.z,fieldOfViewAmplitudeRadians,frequencyHertz,durationSeconds,blendInSeconds,blendOutSeconds,scale,fullIntensityRadiusMeters,zeroIntensityRadiusMeters};
    for(float value:values)if(!std::isfinite(value))return fail("camera shake contains a non-finite value");
    if(frequencyHertz<0.0F||frequencyHertz>1000.0F)return fail("camera shake frequency is invalid");
    if(durationSeconds<0.0F||blendInSeconds<0.0F||blendOutSeconds<0.0F||scale<0.0F)return fail("camera shake timing or scale is invalid");
    if(fullIntensityRadiusMeters<0.0F||zeroIntensityRadiusMeters<0.0F||
       (zeroIntensityRadiusMeters>0.0F&&zeroIntensityRadiusMeters<fullIntensityRadiusMeters))return fail("camera shake attenuation radii are invalid");
    return true;
}

bool CameraDirector::add_or_replace_rig(CameraRig rig,std::string* error){
    if(!rig.validate(error))return false;
    const auto duplicateName=std::find_if(rigs_.begin(),rigs_.end(),[&](const CameraRig& existing){return existing.id!=rig.id&&existing.name==rig.name;});
    if(duplicateName!=rigs_.end()){if(error)*error="camera rig name is already in use";return false;}
    const auto it=std::find_if(rigs_.begin(),rigs_.end(),[&](const CameraRig& existing){return existing.id==rig.id;});
    if(it==rigs_.end())rigs_.push_back(std::move(rig));else *it=std::move(rig);
    return true;
}
bool CameraDirector::remove_rig(CameraRigId id) noexcept {
    const auto before=rigs_.size();std::erase_if(rigs_,[&](const CameraRig& rig){return rig.id==id;});
    if(forcedRig_==id)forcedRig_.reset();
    collisionDistances_.erase(id);
    for(auto it=stateBindings_.begin();it!=stateBindings_.end();)if(it->second==id)it=stateBindings_.erase(it);else++it;
    if(liveRig_==id){liveRig_=0;outgoingRig_=0;transitionDuration_=0.0F;}
    return rigs_.size()!=before;
}
const CameraRig* CameraDirector::find_rig(CameraRigId id) const noexcept {const auto it=std::find_if(rigs_.begin(),rigs_.end(),[&](const CameraRig& rig){return rig.id==id;});return it==rigs_.end()?nullptr:&*it;}
CameraRig* CameraDirector::find_rig(CameraRigId id) noexcept {return const_cast<CameraRig*>(std::as_const(*this).find_rig(id));}
void CameraDirector::set_target(CameraTargetId id,CameraTargetState state) noexcept {state.forward=normalized(state.forward,{0,0,-1});state.up=normalized(state.up,{0,1,0});targets_[id]=state;}
void CameraDirector::remove_target(CameraTargetId id) noexcept {targets_.erase(id);}
void CameraDirector::set_custom_blend(CameraRigId from,CameraRigId to,CameraBlend blend){customBlends_[{from,to}]=blend;}
void CameraDirector::bind_state(std::string stateName,CameraRigId rigId){if(!stateName.empty())stateBindings_[std::move(stateName)]=rigId;}
bool CameraDirector::set_state(std::string_view stateName) noexcept {const auto it=stateBindings_.find(stateName);if(it==stateBindings_.end())return false;state_=std::string(stateName);return true;}
bool CameraDirector::force_live(CameraRigId id,bool cut) noexcept {if(!find_rig(id))return false;forcedRig_=id;if(cut)begin_transition(find_rig(id),true);return true;}
void CameraDirector::clear_forced_live(bool cut) noexcept {forcedRig_.reset();if(cut)begin_transition(choose_live_rig(),true);}
bool CameraDirector::start_shake(CameraShake shake,std::string* error){if(!shake.validate(error))return false;std::erase_if(shakes_,[&](const ActiveShake& active){return active.definition.id==shake.id;});shakes_.push_back({std::move(shake),0.0F});return true;}
bool CameraDirector::stop_shake(CameraShakeId id) noexcept {const auto before=shakes_.size();std::erase_if(shakes_,[&](const ActiveShake& active){return active.definition.id==id;});return before!=shakes_.size();}
void CameraDirector::stop_all_shakes() noexcept {shakes_.clear();}
void CameraDirector::set_shake_scale(float scale) noexcept {shakeScale_=std::clamp(finite_or(scale,1.0F),0.0F,10.0F);}

const CameraRig* CameraDirector::choose_live_rig() const noexcept {
    if(forcedRig_)if(const CameraRig* rig=find_rig(*forcedRig_);rig&&rig->enabled&&(rig->outputChannels&channelMask_)!=0U)return rig;
    if(!state_.empty()){const auto binding=stateBindings_.find(state_);if(binding!=stateBindings_.end())if(const CameraRig* rig=find_rig(binding->second);rig&&rig->enabled&&(rig->outputChannels&channelMask_)!=0U)return rig;}
    const CameraRig* best=nullptr;
    for(const CameraRig& rig:rigs_){if(!rig.enabled||(rig.outputChannels&channelMask_)==0U)continue;if(!best||rig.priority>best->priority||(rig.priority==best->priority&&rig.id>best->id))best=&rig;}
    return best;
}
CameraBlend CameraDirector::blend_for(CameraRigId from,CameraRigId to) const noexcept {const auto it=customBlends_.find({from,to});return it==customBlends_.end()?defaultBlend_:it->second;}
void CameraDirector::begin_transition(const CameraRig* next,bool forceCut) noexcept {
    const CameraRigId nextId=next?next->id:0;
    if(nextId==liveRig_&&!forceCut)return;
    outgoingRig_=liveRig_;liveRig_=nextId;telemetry_.rigChanges++;telemetry_.outgoingRig=outgoingRig_;telemetry_.liveRig=liveRig_;
    transitionFrom_=currentPose_;transitionTo_=next?next->authoredPose:currentPose_;
    CameraBlend blend=(forceCut || outgoingRig_ == 0U)
        ? CameraBlend{CameraBlendCurve::Cut, 0.0F}
        : blend_for(outgoingRig_, liveRig_);
    if(blend.curve==CameraBlendCurve::Cut||blend.durationSeconds<=0.0F){transitionDuration_=0.0F;transitionElapsed_=0.0F;transitionCurve_=CameraBlendCurve::Cut;telemetry_.cuts++;}
    else{transitionDuration_=blend.durationSeconds;transitionElapsed_=0.0F;transitionCurve_=blend.curve;telemetry_.blendsStarted++;}
}
CameraPose CameraDirector::evaluate_rig(const CameraRig& rig,float elapsedSeconds,const ICameraCollisionWorld* collisionWorld){
    telemetry_.shoulderSwapActive=false;telemetry_.occluderFadeActive=false;telemetry_.volumeConstraintActive=false;
    CameraPose desired=rig.authoredPose;desired.lens=rig.lens;desired.postProcessWeight=rig.postProcessWeight;
    const CameraTargetState* follow=nullptr;const CameraTargetState* look=nullptr;
    if(rig.followTarget){const auto it=targets_.find(*rig.followTarget);if(it!=targets_.end())follow=&it->second;}
    if(rig.lookAtTarget){const auto it=targets_.find(*rig.lookAtTarget);if(it!=targets_.end())look=&it->second;}
    if(!look)look=follow;
    Float3 pivot=look?add(look->position,rotate_basis_offset(*look,rig.framing.localOffset)):rig.authoredPose.target;
    if(look){
        pivot=add(pivot,multiply(look->velocity,rig.framing.lookAheadSeconds));
        const Float3 forward=normalized(look->forward,{0,0,-1});
        const Float3 up=normalized(look->up,{0,1,0});
        const Float3 right=normalized(cross(forward,up),{1,0,0});
        const float framingScale=std::max(0.1F,rig.framing.distanceMeters);
        pivot=add(pivot,add(multiply(right,rig.framing.screenOffsetX*framingScale),
                            multiply(up,rig.framing.screenOffsetY*framingScale)));
        if(liveRig_==rig.id&&telemetry_.updates>1&&transitionDuration_<=0.0F){
            const Float3 displacement=subtract(pivot,currentPose_.target);
            const float halfHeight=std::tan(std::clamp(
                rig.lens.effective_vertical_field_of_view_radians(),0.01F,3.13F)*0.5F);
            const float screenScale=framingScale*2.0F*std::max(0.01F,halfHeight);
            const float normalizedDisplacement=length(displacement)/std::max(1.0e-4F,screenScale);
            if(normalizedDisplacement<=rig.framing.deadZoneFraction){
                pivot=currentPose_.target;
            }else if(normalizedDisplacement<rig.framing.softZoneFraction&&
                     rig.framing.softZoneFraction>rig.framing.deadZoneFraction){
                const float zoneWeight=(normalizedDisplacement-rig.framing.deadZoneFraction)/
                    (rig.framing.softZoneFraction-rig.framing.deadZoneFraction);
                pivot=lerp(currentPose_.target,pivot,std::clamp(zoneWeight,0.0F,1.0F));
            }
        }
    }
    switch(rig.mode){
        case CameraRigMode::Fixed:case CameraRigMode::FreeFly:case CameraRigMode::Cinematic:break;
        case CameraRigMode::FirstPerson:
            if(follow){desired.position=add(follow->position,rotate_basis_offset(*follow,rig.framing.localOffset));desired.target=add(desired.position,multiply(normalized(follow->forward,{0,0,-1}),10.0F));desired.worldUp=follow->up;}
            break;
        case CameraRigMode::Follow:
            if(follow){desired.position=add(follow->position,rotate_basis_offset(*follow,rig.framing.localOffset));desired.target=pivot;desired.worldUp=follow->up;}
            break;
        case CameraRigMode::ThirdPerson:
            if(follow){const Float3 forward=normalized(follow->forward,{0,0,-1});const Float3 up=normalized(follow->up,{0,1,0});const Float3 right=target_right(*follow);
                desired.position=add(add(add(pivot,multiply(forward,-rig.framing.distanceMeters)),multiply(up,rig.framing.heightMeters)),multiply(right,rig.framing.shoulderOffsetMeters));desired.target=pivot;desired.worldUp=up;}
            break;
        case CameraRigMode::Orbit:
            if(follow){const float pitch=std::clamp(rig.framing.orbitPitchRadians,rig.framing.minimumPitchRadians,rig.framing.maximumPitchRadians);const float yaw=rig.framing.orbitYawRadians;const float cp=std::cos(pitch);const Float3 offset{std::sin(yaw)*cp,std::sin(pitch),std::cos(yaw)*cp};desired.position=add(pivot,multiply(offset,rig.framing.distanceMeters));desired.target=pivot;desired.worldUp=follow->up;}
            break;
    }
    if((rig.mode==CameraRigMode::ThirdPerson||rig.mode==CameraRigMode::Orbit)&&rig.collision.enabled&&look){
        Float3 direction=normalized(subtract(desired.position,pivot),{0,0,1});
        float idealDistance=length(subtract(desired.position,pivot));
        float requestedDistance=idealDistance;
        bool corrected=false;
        CameraCollisionHit hit{};
        bool obstructed=collisionWorld&&collisionWorld->sweep_sphere(
            pivot,desired.position,rig.collision.probeRadiusMeters,hit);
        if(obstructed&&rig.collision.strategy==CameraObstructionStrategy::ShoulderSwap&&
           rig.mode==CameraRigMode::ThirdPerson&&follow){
            const Float3 right=target_right(*follow);
            const float swapDistance=std::max(std::abs(rig.framing.shoulderOffsetMeters),
                                              rig.collision.shoulderSwapSearchMeters);
            const Float3 alternate=add(desired.position,multiply(right,
                rig.framing.shoulderOffsetMeters>=0.0F?-2.0F*swapDistance:2.0F*swapDistance));
            CameraCollisionHit alternateHit{};
            if(!collisionWorld->sweep_sphere(pivot,alternate,rig.collision.probeRadiusMeters,alternateHit)){
                desired.position=alternate;
                direction=normalized(subtract(desired.position,pivot),direction);
                idealDistance=length(subtract(desired.position,pivot));
                requestedDistance=idealDistance;
                obstructed=false;
                telemetry_.shoulderSwaps++;
                telemetry_.shoulderSwapActive=true;
            }
        }
        if(obstructed){
            if(rig.collision.strategy==CameraObstructionStrategy::FadeOccluders){
                telemetry_.occluderFadeRequests++;
                telemetry_.occluderFadeActive=true;
                const CameraOccluderFadeRequest request{
                    rig.id, hit.objectId, hit.materialId, rig.collision.occluderFadeOpacity,
                    hit.position};
                const auto duplicate = std::find_if(
                    occluderFadeRequests_.begin(), occluderFadeRequests_.end(),
                    [&](const CameraOccluderFadeRequest& existing) {
                        return existing.rigId == request.rigId && existing.objectId == request.objectId &&
                               existing.materialId == request.materialId;
                    });
                if (duplicate == occluderFadeRequests_.end()) occluderFadeRequests_.push_back(request);
                else if (request.targetOpacity < duplicate->targetOpacity) *duplicate = request;
            }else{
                requestedDistance=std::max(rig.collision.minimumTargetDistanceMeters,
                    idealDistance*std::clamp(hit.fraction,0.0F,1.0F)-rig.collision.probeRadiusMeters);
                corrected=true;
                telemetry_.collisionCorrections++;
            }
        }
        auto [distanceIt,inserted]=collisionDistances_.try_emplace(rig.id,requestedDistance);
        if(!inserted){
            const float responseSeconds=corrected?rig.collision.collisionPullInSeconds:rig.collision.recoverySeconds;
            distanceIt->second=std::lerp(distanceIt->second,requestedDistance,smoothing_alpha(elapsedSeconds,responseSeconds));
        }
        distanceIt->second=std::clamp(distanceIt->second,rig.collision.minimumTargetDistanceMeters,idealDistance);
        if(!telemetry_.occluderFadeActive)desired.position=add(pivot,multiply(direction,distanceIt->second));
    }
    if(rig.volumeConstraint.enabled){
        const Float3 constrained{
            std::clamp(desired.position.x,rig.volumeConstraint.minimum.x,rig.volumeConstraint.maximum.x),
            std::clamp(desired.position.y,rig.volumeConstraint.minimum.y,rig.volumeConstraint.maximum.y),
            std::clamp(desired.position.z,rig.volumeConstraint.minimum.z,rig.volumeConstraint.maximum.z)};
        if(length_squared(subtract(constrained,desired.position))>1.0e-10F){
            desired.position=rig.volumeConstraint.softnessMeters>0.0F
                ?lerp(desired.position,constrained,smoothing_alpha(elapsedSeconds,rig.volumeConstraint.softnessMeters))
                :constrained;
            telemetry_.volumeCorrections++;
            telemetry_.volumeConstraintActive=true;
        }
    }
    const float positionAlpha=smoothing_alpha(elapsedSeconds,rig.framing.positionDampingSeconds);
    const float aimAlpha=smoothing_alpha(elapsedSeconds,rig.framing.aimDampingSeconds);
    if (liveRig_ == rig.id && telemetry_.updates > 1 && transitionDuration_ <= 0.0F) {
        desired.position = lerp(currentPose_.position, desired.position, positionAlpha);
        desired.target = lerp(currentPose_.target, desired.target, aimAlpha);
    }
    return desired;
}
void CameraDirector::apply_shakes(CameraPose& pose,float elapsedSeconds){
    if(shakes_.empty()||shakeScale_<=0.0F)return;
    const Float3 forward=normalized(subtract(pose.target,pose.position),{0,0,-1});const Float3 right=normalized(cross(forward,pose.worldUp),{1,0,0});const Float3 up=normalized(cross(right,forward),{0,1,0});
    Float3 positionOffset{};Float3 rotation{};float fov{};
    for(ActiveShake& active:shakes_){active.elapsed+=elapsedSeconds;const CameraShake& shake=active.definition;float attenuation=1.0F;
        if(shake.sourcePosition&&shake.zeroIntensityRadiusMeters>shake.fullIntensityRadiusMeters){const float distance=length(subtract(pose.position,*shake.sourcePosition));if(distance>=shake.zeroIntensityRadiusMeters)attenuation=0.0F;else if(distance>shake.fullIntensityRadiusMeters)attenuation=1.0F-(distance-shake.fullIntensityRadiusMeters)/(shake.zeroIntensityRadiusMeters-shake.fullIntensityRadiusMeters);}
        const float envelope=shake_envelope(shake,active.elapsed)*shake.scale*attenuation*shakeScale_*(reducedMotion_?0.2F:1.0F);if(envelope<=0.0F)continue;
        Float3 sample{};for(int axis=0;axis<3;++axis){const float phase=active.elapsed*shake.frequencyHertz*2.0F*std::numbers::pi_v<float>+static_cast<float>(axis)*1.731F;const float value=shake.pattern==CameraShakePattern::SineWave?std::sin(phase):hash_noise(shake.seed,active.elapsed*shake.frequencyHertz,static_cast<float>(axis));if(axis==0)sample.x=value;else if(axis==1)sample.y=value;else sample.z=value;}
        const Float3 localPosition{
            sample.x * shake.positionAmplitudeMeters.x,
            sample.y * shake.positionAmplitudeMeters.y,
            sample.z * shake.positionAmplitudeMeters.z};
        const Float3 worldPosition = shake.space == CameraShakeSpace::CameraLocal
            ? add(add(multiply(right, localPosition.x), multiply(up, localPosition.y)),
                  multiply(forward, localPosition.z))
            : localPosition;
        positionOffset = add(positionOffset, multiply(worldPosition, envelope));
        rotation = add(rotation, {
            sample.x * shake.rotationAmplitudeRadians.x * envelope,
            sample.y * shake.rotationAmplitudeRadians.y * envelope,
            sample.z * shake.rotationAmplitudeRadians.z * envelope});
        fov += hash_noise(shake.seed, active.elapsed * shake.frequencyHertz, 3.0F) *
               shake.fieldOfViewAmplitudeRadians * envelope;
        telemetry_.shakeSamples++;
    }
    pose.position=add(pose.position,positionOffset);
    const Float3 look=normalized(subtract(pose.target,pose.position),forward);const Quaternion yaw=quaternion_from_axis_angle(up,rotation.y);const Quaternion pitch=quaternion_from_axis_angle(right,rotation.x);const Quaternion roll=quaternion_from_axis_angle(look,rotation.z);const Float3 rotated=rotate(multiply(roll,multiply(yaw,pitch)),look);pose.target=add(pose.position,multiply(rotated,std::max(1.0e-3F,length(subtract(pose.target,pose.position)))));if(fov!=0.0F){
        if(pose.lens.physical.enabled){
            // A physical lens derives its field of view from sensor height and focal length, so
            // writing to verticalFieldOfViewRadians had no effect at all on those cameras. Shake
            // the focal length instead, which is what a physical camera would actually do.
            const float current=pose.lens.physical.vertical_field_of_view_radians(pose.lens.aspectRatio);
            const float shaken=std::clamp(current+fov,1.0F*kDegreesToRadians,179.0F*kDegreesToRadians);
            const float sensorHeight=std::max(0.001F,pose.lens.physical.sensorHeightMillimeters);
            pose.lens.physical.focalLengthMillimeters=
                std::max(0.001F,sensorHeight/(2.0F*std::tan(shaken*0.5F)));
        }else{
            pose.lens.verticalFieldOfViewRadians=std::clamp(pose.lens.verticalFieldOfViewRadians+fov,1.0F*kDegreesToRadians,179.0F*kDegreesToRadians);
        }
    }
    std::erase_if(shakes_,[](const ActiveShake& active){return active.definition.durationSeconds>0.0F&&active.elapsed>=active.definition.durationSeconds;});
}
CameraPose CameraDirector::update(float elapsedSeconds,const ICameraCollisionWorld* collisionWorld){
    occluderFadeRequests_.clear();
    elapsedSeconds=std::clamp(finite_or(elapsedSeconds,0.0F),0.0F,1.0F);telemetry_.updates++;
    const CameraRig* desired=choose_live_rig();if((desired?desired->id:0)!=liveRig_)begin_transition(desired,false);
    CameraPose evaluated=desired?evaluate_rig(*desired,elapsedSeconds,collisionWorld):currentPose_;
    if(transitionDuration_>0.0F){transitionElapsed_+=elapsedSeconds;transitionTo_=evaluated;const float raw=transitionElapsed_/transitionDuration_;const float weight=blend_weight(transitionCurve_,raw);currentPose_=blend_pose(transitionFrom_,transitionTo_,weight);telemetry_.blendWeight=weight;if(raw>=1.0F){transitionDuration_=0.0F;outgoingRig_=0;telemetry_.outgoingRig=0;}}
    else{currentPose_=evaluated;telemetry_.blendWeight=1.0F;}
    apply_shakes(currentPose_,elapsedSeconds);telemetry_.liveRig=liveRig_;return currentPose_;
}

bool CameraRigLibrary::validate(std::string* error) const {
    std::unordered_set<CameraRigId> ids;std::unordered_set<std::string> names;
    for(const CameraRig& rig:rigs){if(!rig.validate(error))return false;if(!ids.insert(rig.id).second){if(error)*error="duplicate camera rig id";return false;}if(!names.insert(rig.name).second){if(error)*error="duplicate camera rig name";return false;}}
    if(!defaultBlend.validate(error))return false;
    for(const auto& [pair,blend]:customBlends){if(!ids.contains(pair.first)||!ids.contains(pair.second)){if(error)*error="custom camera blend references a missing rig";return false;}if(!blend.validate(error))return false;}
    for(const auto& [state,id]:stateBindings){if(state.empty()||!ids.contains(id)){if(error)*error="camera state binding is invalid";return false;}}
    return true;
}
std::string CameraRigLibrary::serialize() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "DVE_CAMERA_LIBRARY 3\n";
    out << "defaultBlend " << curve_name(defaultBlend.curve) << ' ' << defaultBlend.durationSeconds << "\n";
    out << "rigs " << rigs.size() << "\n";
    for (const CameraRig& rig : rigs) {
        out << "rig " << rig.id << ' ' << std::quoted(rig.name) << ' ' << mode_name(rig.mode) << ' '
            << rig.enabled << ' ' << rig.priority << ' ' << rig.outputChannels << ' '
            << (rig.followTarget ? *rig.followTarget : 0) << ' '
            << (rig.lookAtTarget ? *rig.lookAtTarget : 0) << ' ' << rig.postProcessWeight << "\n";
        out << "pose " << rig.authoredPose.position.x << ' ' << rig.authoredPose.position.y << ' '
            << rig.authoredPose.position.z << ' ' << rig.authoredPose.target.x << ' '
            << rig.authoredPose.target.y << ' ' << rig.authoredPose.target.z << ' '
            << rig.authoredPose.worldUp.x << ' ' << rig.authoredPose.worldUp.y << ' '
            << rig.authoredPose.worldUp.z << "\n";
        out << "lens " << static_cast<int>(rig.lens.projection) << ' '
            << rig.lens.verticalFieldOfViewRadians << ' ' << rig.lens.orthographicHeightMeters << ' '
            << rig.lens.nearPlaneMeters << ' ' << rig.lens.farPlaneMeters << ' ' << rig.lens.aspectRatio << "\n";
        out << "physical " << rig.lens.physical.enabled << ' '
            << rig.lens.physical.focalLengthMillimeters << ' '
            << rig.lens.physical.sensorWidthMillimeters << ' '
            << rig.lens.physical.sensorHeightMillimeters << ' '
            << rig.lens.physical.lensShiftX << ' ' << rig.lens.physical.lensShiftY << ' '
            << rig.lens.physical.apertureFStop << ' ' << rig.lens.physical.focusDistanceMeters << ' '
            << static_cast<int>(rig.lens.physical.gateFit) << "\n";
        out << "framing " << rig.framing.localOffset.x << ' ' << rig.framing.localOffset.y << ' '
            << rig.framing.localOffset.z << ' ' << rig.framing.distanceMeters << ' '
            << rig.framing.heightMeters << ' ' << rig.framing.shoulderOffsetMeters << ' '
            << rig.framing.lookAheadSeconds << ' ' << rig.framing.screenOffsetX << ' '
            << rig.framing.screenOffsetY << ' ' << rig.framing.positionDampingSeconds << ' '
            << rig.framing.aimDampingSeconds << ' ' << rig.framing.deadZoneFraction << ' '
            << rig.framing.softZoneFraction << ' ' << rig.framing.orbitYawRadians << ' '
            << rig.framing.orbitPitchRadians << ' ' << rig.framing.minimumPitchRadians << ' '
            << rig.framing.maximumPitchRadians << "\n";
        out << "collision " << rig.collision.enabled << ' ' << rig.collision.preserveLineOfSight << ' '
            << obstruction_name(rig.collision.strategy) << ' ' << rig.collision.probeRadiusMeters << ' '
            << rig.collision.minimumTargetDistanceMeters << ' ' << rig.collision.collisionPullInSeconds << ' '
            << rig.collision.recoverySeconds << ' ' << rig.collision.shoulderSwapSearchMeters << ' '
            << rig.collision.occluderFadeOpacity << "\n";
        out << "volume " << rig.volumeConstraint.enabled << ' ' << rig.volumeConstraint.minimum.x << ' '
            << rig.volumeConstraint.minimum.y << ' ' << rig.volumeConstraint.minimum.z << ' '
            << rig.volumeConstraint.maximum.x << ' ' << rig.volumeConstraint.maximum.y << ' '
            << rig.volumeConstraint.maximum.z << ' ' << rig.volumeConstraint.softnessMeters << "\n";
        out << "endrig\n";
    }
    out << "customBlends " << customBlends.size() << "\n";
    for (const auto& [pair, blend] : customBlends)
        out << pair.first << ' ' << pair.second << ' ' << curve_name(blend.curve) << ' '
            << blend.durationSeconds << "\n";
    out << "states " << stateBindings.size() << "\n";
    for (const auto& [state, id] : stateBindings) out << std::quoted(state) << ' ' << id << "\n";
    return out.str();
}

std::optional<CameraRigLibrary> CameraRigLibrary::parse(std::string_view text, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<CameraRigLibrary> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    std::istringstream in{std::string(text)};
    std::string magic;
    int version{};
    if (!(in >> magic >> version) || magic != "DVE_CAMERA_LIBRARY" || (version != 1 && version != 2 && version != 3))
        return fail("unsupported camera library format");

    CameraRigLibrary result;
    std::string key;
    std::string curveText;
    if (!(in >> key >> curveText >> result.defaultBlend.durationSeconds) || key != "defaultBlend")
        return fail("missing default camera blend");
    const auto defaultCurve = parse_curve(curveText);
    if (!defaultCurve) return fail("invalid default camera blend curve");
    result.defaultBlend.curve = *defaultCurve;

    std::size_t count{};
    if (!(in >> key >> count) || key != "rigs" || count > 4096U) return fail("invalid camera rig count");
    for (std::size_t index = 0; index < count; ++index) {
        CameraRig rig;
        std::string modeText;
        CameraTargetId follow{};
        CameraTargetId look{};
        int projection{};
        if (version == 1) {
            if (!(in >> key >> rig.id >> std::quoted(rig.name) >> modeText >> rig.enabled >> rig.priority
                  >> rig.outputChannels >> rig.authoredPose.position.x >> rig.authoredPose.position.y
                  >> rig.authoredPose.position.z >> rig.authoredPose.target.x >> rig.authoredPose.target.y
                  >> rig.authoredPose.target.z >> rig.lens.verticalFieldOfViewRadians
                  >> rig.lens.orthographicHeightMeters >> rig.lens.nearPlaneMeters >> rig.lens.farPlaneMeters
                  >> projection >> follow >> look >> rig.framing.distanceMeters >> rig.framing.heightMeters
                  >> rig.framing.shoulderOffsetMeters >> rig.framing.positionDampingSeconds
                  >> rig.framing.aimDampingSeconds >> rig.collision.enabled
                  >> rig.collision.probeRadiusMeters >> rig.postProcessWeight) || key != "rig")
                return fail("malformed legacy camera rig");
            if (projection < 0 || projection > 1) return fail("invalid camera projection");
            rig.lens.projection = static_cast<CameraProjection>(projection);
        } else {
            if (!(in >> key >> rig.id >> std::quoted(rig.name) >> modeText >> rig.enabled >> rig.priority
                  >> rig.outputChannels >> follow >> look >> rig.postProcessWeight) || key != "rig")
                return fail("malformed camera rig header");
            if (!(in >> key >> rig.authoredPose.position.x >> rig.authoredPose.position.y
                  >> rig.authoredPose.position.z >> rig.authoredPose.target.x >> rig.authoredPose.target.y
                  >> rig.authoredPose.target.z >> rig.authoredPose.worldUp.x >> rig.authoredPose.worldUp.y
                  >> rig.authoredPose.worldUp.z) || key != "pose")
                return fail("malformed camera pose");
            if (!(in >> key >> projection >> rig.lens.verticalFieldOfViewRadians
                  >> rig.lens.orthographicHeightMeters >> rig.lens.nearPlaneMeters
                  >> rig.lens.farPlaneMeters >> rig.lens.aspectRatio) || key != "lens")
                return fail("malformed camera lens");
            int gateFit{};
            if (!(in >> key >> rig.lens.physical.enabled >> rig.lens.physical.focalLengthMillimeters
                  >> rig.lens.physical.sensorWidthMillimeters >> rig.lens.physical.sensorHeightMillimeters
                  >> rig.lens.physical.lensShiftX >> rig.lens.physical.lensShiftY
                  >> rig.lens.physical.apertureFStop >> rig.lens.physical.focusDistanceMeters >> gateFit)
                || key != "physical") return fail("malformed physical camera lens");
            if (projection < 0 || projection > 1 || gateFit < 0 || gateFit > 4)
                return fail("invalid camera lens enum");
            rig.lens.projection = static_cast<CameraProjection>(projection);
            rig.lens.physical.gateFit = static_cast<CameraGateFit>(gateFit);
            if (!(in >> key >> rig.framing.localOffset.x >> rig.framing.localOffset.y
                  >> rig.framing.localOffset.z >> rig.framing.distanceMeters >> rig.framing.heightMeters
                  >> rig.framing.shoulderOffsetMeters >> rig.framing.lookAheadSeconds
                  >> rig.framing.screenOffsetX >> rig.framing.screenOffsetY
                  >> rig.framing.positionDampingSeconds >> rig.framing.aimDampingSeconds
                  >> rig.framing.deadZoneFraction >> rig.framing.softZoneFraction
                  >> rig.framing.orbitYawRadians >> rig.framing.orbitPitchRadians
                  >> rig.framing.minimumPitchRadians >> rig.framing.maximumPitchRadians) || key != "framing")
                return fail("malformed camera framing");
            if (version == 2) {
                if (!(in >> key >> rig.collision.enabled >> rig.collision.preserveLineOfSight
                      >> rig.collision.probeRadiusMeters >> rig.collision.minimumTargetDistanceMeters
                      >> rig.collision.collisionPullInSeconds >> rig.collision.recoverySeconds) || key != "collision")
                    return fail("malformed camera collision settings");
            } else {
                std::string obstructionText;
                if (!(in >> key >> rig.collision.enabled >> rig.collision.preserveLineOfSight
                      >> obstructionText >> rig.collision.probeRadiusMeters
                      >> rig.collision.minimumTargetDistanceMeters >> rig.collision.collisionPullInSeconds
                      >> rig.collision.recoverySeconds >> rig.collision.shoulderSwapSearchMeters
                      >> rig.collision.occluderFadeOpacity) || key != "collision")
                    return fail("malformed camera collision settings");
                const auto strategy=parse_obstruction(obstructionText);
                if(!strategy)return fail("unknown camera obstruction strategy");
                rig.collision.strategy=*strategy;
                if (!(in >> key >> rig.volumeConstraint.enabled >> rig.volumeConstraint.minimum.x
                      >> rig.volumeConstraint.minimum.y >> rig.volumeConstraint.minimum.z
                      >> rig.volumeConstraint.maximum.x >> rig.volumeConstraint.maximum.y
                      >> rig.volumeConstraint.maximum.z >> rig.volumeConstraint.softnessMeters) || key != "volume")
                    return fail("malformed camera volume constraint");
            }
            if (!(in >> key) || key != "endrig") return fail("missing camera rig terminator");
        }
        const auto mode = parse_mode(modeText);
        if (!mode) return fail("invalid camera rig mode");
        rig.mode = *mode;
        if (follow != 0U) rig.followTarget = follow;
        if (look != 0U) rig.lookAtTarget = look;
        rig.authoredPose.lens = rig.lens;
        rig.authoredPose.postProcessWeight = rig.postProcessWeight;
        result.rigs.push_back(std::move(rig));
    }

    if (!(in >> key >> count) || key != "customBlends" || count > 65536U)
        return fail("invalid custom camera blend count");
    for (std::size_t index = 0; index < count; ++index) {
        CameraRigId from{};
        CameraRigId to{};
        CameraBlend blend;
        if (!(in >> from >> to >> curveText >> blend.durationSeconds)) return fail("malformed custom camera blend");
        const auto curve = parse_curve(curveText);
        if (!curve) return fail("invalid custom camera blend curve");
        blend.curve = *curve;
        result.customBlends[{from, to}] = blend;
    }
    if (!(in >> key >> count) || key != "states" || count > 4096U)
        return fail("invalid camera state count");
    for (std::size_t index = 0; index < count; ++index) {
        std::string state;
        CameraRigId id{};
        if (!(in >> std::quoted(state) >> id)) return fail("malformed camera state binding");
        result.stateBindings[state] = id;
    }
    in >> std::ws;
    if (!in.eof()) return fail("trailing data in camera library");
    std::string validation;
    if (!result.validate(&validation)) return fail(validation);
    return result;
}

} // namespace dve::camera
