#include "dve/render/environment_lighting_gpu.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <bit>
#include <cmath>
#include <span>
#include <vector>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) { if (error) *error=std::move(message); }

std::uint16_t pack_half(float value) noexcept {
    // Separate, branch-safe IEEE-754 binary16 conversion used by texture uploads.
    const std::uint32_t x=std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign=(x>>16U)&0x8000U;
    std::uint32_t mantissa=x&0x007FFFFFU;
    int exponent=static_cast<int>((x>>23U)&0xFFU)-127+15;
    if(((x>>23U)&0xFFU)==0xFFU)return static_cast<std::uint16_t>(sign|(mantissa?0x7E00U:0x7C00U));
    if(exponent<=0){if(exponent<-10)return static_cast<std::uint16_t>(sign);mantissa|=0x00800000U;const unsigned shift=static_cast<unsigned>(14-exponent);std::uint32_t half=mantissa>>shift;if((mantissa>>(shift-1U))&1U)++half;return static_cast<std::uint16_t>(sign|half);}
    if(exponent>=31)return static_cast<std::uint16_t>(sign|0x7C00U);
    mantissa+=0x00001000U;if(mantissa&0x00800000U){mantissa=0;++exponent;if(exponent>=31)return static_cast<std::uint16_t>(sign|0x7C00U);}return static_cast<std::uint16_t>(sign|(static_cast<std::uint32_t>(exponent)<<10U)|(mantissa>>13U));
}

std::vector<std::byte> pack_cube_face(const EnvironmentCube& cube, CubeFace face) {
    std::vector<std::uint16_t> half(static_cast<std::size_t>(cube.resolution)*cube.resolution*4U);
    const auto offset=cube.face_offset(face);
    for(std::size_t i=0;i<static_cast<std::size_t>(cube.resolution)*cube.resolution;++i){const auto p=cube.texels[offset+i];half[i*4U]=pack_half(p.x);half[i*4U+1U]=pack_half(p.y);half[i*4U+2U]=pack_half(p.z);half[i*4U+3U]=pack_half(1.0F);}
    return {reinterpret_cast<const std::byte*>(half.data()),reinterpret_cast<const std::byte*>(half.data()+half.size())};
}
std::vector<std::byte> pack_brdf(const IblBakeResult& ibl) {
    std::vector<std::uint16_t> half(ibl.brdfLut.size()*4U);
    for(std::size_t i=0;i<ibl.brdfLut.size();++i){half[i*4U]=pack_half(ibl.brdfLut[i].scale);half[i*4U+1U]=pack_half(ibl.brdfLut[i].bias);half[i*4U+2U]=0U;half[i*4U+3U]=pack_half(1.0F);}
    return {reinterpret_cast<const std::byte*>(half.data()),reinterpret_cast<const std::byte*>(half.data()+half.size())};
}

bool upload_cube(rhi::IDevice& device,rhi::TextureHandle texture,const EnvironmentCubeMipChain& chain,std::uint64_t& bytes,std::string* error){
    for(std::uint32_t mip=0;mip<chain.levels.size();++mip){const auto& level=chain.levels[mip];for(std::uint32_t face=0;face<6U;++face){auto packed=pack_cube_face(level,static_cast<CubeFace>(face));if(!device.write_texture(texture,mip,face,packed,static_cast<std::size_t>(level.resolution)*8U,error))return false;bytes+=packed.size();}}
    return true;
}
bool upload_cube(rhi::IDevice& device,rhi::TextureHandle texture,const EnvironmentCube& cube,std::uint64_t& bytes,std::string* error){EnvironmentCubeMipChain chain;chain.levels.push_back(cube);return upload_cube(device,texture,chain,bytes,error);}
}

bool EnvironmentLightingGpuResources::valid() const noexcept {return diffuseIrradiance&&diffuseIrradianceView&&specularPrefilter&&specularPrefilterView&&brdfLut&&brdfLutView&&sampler&&bindGroupLayout&&bindGroup&&specularMipLevels>0U;}

bool destroy_environment_lighting(rhi::IDevice& device,EnvironmentLightingGpuResources& r,std::string* error){bool ok=true;std::string local;auto fail=[&](bool result){if(!result){ok=false;if(error&&error->empty())*error=local;}local.clear();};if(r.bindGroup)fail(device.destroy_bind_group(r.bindGroup,&local));if(r.bindGroupLayout)fail(device.destroy_bind_group_layout(r.bindGroupLayout,&local));if(r.diffuseIrradianceView)fail(device.destroy_texture_view(r.diffuseIrradianceView,&local));if(r.specularPrefilterView)fail(device.destroy_texture_view(r.specularPrefilterView,&local));if(r.brdfLutView)fail(device.destroy_texture_view(r.brdfLutView,&local));if(r.sampler)fail(device.destroy_sampler(r.sampler,&local));if(r.diffuseIrradiance)fail(device.destroy_texture(r.diffuseIrradiance,&local));if(r.specularPrefilter)fail(device.destroy_texture(r.specularPrefilter,&local));if(r.brdfLut)fail(device.destroy_texture(r.brdfLut,&local));r={};return ok;}

bool upload_environment_lighting(rhi::IDevice& device,const EnvironmentLightingAsset& asset,EnvironmentLightingGpuResources& out,std::string* error){std::string validation;if(!asset.validate(&validation)){set_error(error,validation);return false;}EnvironmentLightingGpuResources r;auto fail=[&](std::string message){std::string ignored;(void)destroy_environment_lighting(device,r,&ignored);set_error(error,std::move(message));return false;};
    rhi::TextureDesc diffuse;diffuse.dimension=rhi::TextureDimension::TextureCube;diffuse.format=rhi::TextureFormat::RGBA16Float;diffuse.width=asset.baked.diffuseIrradiance.resolution;diffuse.height=diffuse.width;diffuse.arrayLayers=6U;diffuse.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination|rhi::TextureUsage::CopySource;diffuse.initialState=rhi::ResourceState::ShaderRead;diffuse.debugName="IBL diffuse irradiance";std::string local;r.diffuseIrradiance=device.create_texture(diffuse,&local);if(!r.diffuseIrradiance)return fail(local);
    rhi::TextureDesc spec=diffuse;spec.width=asset.baked.specularPrefilter.levels.front().resolution;spec.height=spec.width;spec.mipLevels=static_cast<std::uint32_t>(asset.baked.specularPrefilter.levels.size());spec.debugName="IBL specular prefilter";r.specularPrefilter=device.create_texture(spec,&local);if(!r.specularPrefilter)return fail(local);
    rhi::TextureDesc brdf;brdf.format=rhi::TextureFormat::RGBA16Float;brdf.width=asset.baked.brdfResolution;brdf.height=asset.baked.brdfResolution;brdf.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination|rhi::TextureUsage::CopySource;brdf.initialState=rhi::ResourceState::ShaderRead;brdf.debugName="IBL BRDF LUT";r.brdfLut=device.create_texture(brdf,&local);if(!r.brdfLut)return fail(local);
    if(!upload_cube(device,r.diffuseIrradiance,asset.baked.diffuseIrradiance,r.uploadedBytes,&local)||!upload_cube(device,r.specularPrefilter,asset.baked.specularPrefilter,r.uploadedBytes,&local))return fail(local);
    auto brdfBytes=pack_brdf(asset.baked);if(!device.write_texture(r.brdfLut,0U,0U,brdfBytes,static_cast<std::size_t>(asset.baked.brdfResolution)*8U,&local))return fail(local);r.uploadedBytes+=brdfBytes.size();
    rhi::TextureViewDesc view;view.texture=r.diffuseIrradiance;view.layerCount=6U;view.dimension=rhi::TextureViewDimension::TextureCube;view.debugName="IBL diffuse cube view";r.diffuseIrradianceView=device.create_texture_view(view,&local);if(!r.diffuseIrradianceView)return fail(local);view.texture=r.specularPrefilter;view.mipCount=spec.mipLevels;view.debugName="IBL specular cube view";r.specularPrefilterView=device.create_texture_view(view,&local);if(!r.specularPrefilterView)return fail(local);view={};view.texture=r.brdfLut;view.debugName="IBL BRDF view";r.brdfLutView=device.create_texture_view(view,&local);if(!r.brdfLutView)return fail(local);
    rhi::SamplerDesc sampler;sampler.addressU=sampler.addressV=sampler.addressW=rhi::AddressMode::ClampToEdge;sampler.maximumLod=static_cast<float>(spec.mipLevels-1U);sampler.debugName="IBL trilinear sampler";r.sampler=device.create_sampler(sampler,&local);if(!r.sampler)return fail(local);
    rhi::BindGroupLayoutDesc layout;layout.debugName="IBL sampled texture layout";layout.bindings={{0U,rhi::BindingType::SampledTexture,rhi::ShaderStage::Compute|rhi::ShaderStage::Fragment},{1U,rhi::BindingType::SampledTexture,rhi::ShaderStage::Compute|rhi::ShaderStage::Fragment},{2U,rhi::BindingType::SampledTexture,rhi::ShaderStage::Compute|rhi::ShaderStage::Fragment}};r.bindGroupLayout=device.create_bind_group_layout(layout,&local);if(!r.bindGroupLayout)return fail(local);
    rhi::BindGroupDesc group;group.layout=r.bindGroupLayout;group.debugName="IBL sampled textures";group.entries={{0U,{},r.diffuseIrradianceView,0U,0U,r.sampler},{1U,{},r.specularPrefilterView,0U,0U,r.sampler},{2U,{},r.brdfLutView,0U,0U,r.sampler}};r.bindGroup=device.create_bind_group(group,&local);if(!r.bindGroup)return fail(local);r.specularMipLevels=spec.mipLevels;
    if(out.valid()){std::string ignored;(void)destroy_environment_lighting(device,out,&ignored);}out=r;return true;}
} // namespace dve::render
