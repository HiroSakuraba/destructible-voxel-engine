#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>
using namespace dve::rhi;
namespace {
std::string error;void check(bool b,const std::string& m){if(!b)throw std::runtime_error(m+": "+error);}
struct V{float x,y,z,w;};struct Surface{V position,normal;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
}
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("pass surface and upsample SPIR-V paths");VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC screen surface: Vulkan unavailable");}
 constexpr unsigned width=8,height=8;std::vector<std::uint16_t> pixels(width*height*4,0);for(unsigned i=0;i<width*height;++i)pixels[i*4]=0x3800;pixels[0]=0x3c00;
 TextureDesc td;td.width=width;td.height=height;td.format=TextureFormat::RGBA16Float;td.usage=TextureUsage::Sampled|TextureUsage::CopyDestination;td.initialState=ResourceState::ShaderRead;auto texture=d.create_texture(td,&error);auto view=d.create_texture_view({texture,0,1,0,1,"depth fixture"},&error);SamplerDesc sd;sd.minFilter=sd.magFilter=FilterMode::Nearest;sd.addressU=sd.addressV=sd.addressW=AddressMode::ClampToEdge;auto sampler=d.create_sampler(sd,&error);check(texture&&view&&sampler&&d.write_texture(texture,0,0,std::as_bytes(std::span(pixels)),width*8,&error),"depth texture");
 std::array<float,128> constants{};for(unsigned i=0;i<4;++i){constants[i*5]=1;constants[16+i*5]=1;}constants[34]=2;constants[35]=1;constants[40]=width;constants[41]=height;constants[42]=1.F/width;constants[43]=1.F/height;constants[70]=1;
 BufferDesc cb;cb.bytes=sizeof(constants);cb.usage=BufferUsage::Constant;cb.memory=MemoryDomain::Upload;cb.initialState=ResourceState::ShaderRead;auto buffer=d.create_buffer(cb,&error);BufferDesc output;output.bytes=width*height*sizeof(Surface);output.usage=BufferUsage::Storage|BufferUsage::CopySource;output.memory=MemoryDomain::Upload;output.initialState=ResourceState::ShaderRead;auto result=d.create_buffer(output,&error);check(buffer&&result&&d.write_buffer(buffer,0,std::as_bytes(std::span(constants)),&error),"buffers");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::SampledTexture,ShaderStage::Compute},{1,BindingType::UniformBuffer,ShaderStage::Compute},{9,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,{},view,0,0,sampler},{1,buffer,{},0,sizeof(constants),{}},{9,result,{},0,output.bytes,{}}};auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"pipeline");
 const auto run=[&](){auto commands=d.begin_commands(QueueKind::Compute,"split RC screen surfaces",&error);check(d.bind_compute_bind_group(commands,0,group,&error)&&d.dispatch(commands,pipeline,1,1,1,&error),"dispatch");auto fence=d.submit(commands,&error);check(fence&&d.fence_complete(fence),"fence");std::array<Surface,width*height> out{};check(d.read_buffer(result,0,std::as_writable_bytes(std::span(out)),&error),"readback");return out;};
 auto out=run();auto hit=out[4*width+4];check(hit.position.w==1&&std::abs(hit.position.x-.125F)<1e-5F&&std::abs(hit.position.y-.125F)<1e-5F&&std::abs(hit.position.z-.5F)<1e-5F,"reconstructed world position");check(hit.normal.w==1&&hit.normal.z>.999F,"camera-facing plane normal");check(out[0].position.w==0,"sky pixel is not a surface");
 pixels[(4*width+4)*4]=0x3c00;check(d.write_texture(texture,0,0,std::as_bytes(std::span(pixels)),width*8,&error),"edit depth");out=run();check(out[4*width+4].position.w==0,"changed depth removes surface");

 // A single 8x8 tile contains two parallel depth planes. Its coarse centre
 // belongs to the right-hand face. Full-resolution upsampling must light the
 // right face without leaking onto the left face or onto a sky pixel.
 pixels[(4*width+4)*4]=0x3800;
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<4;++x)pixels[(y*width+x)*4]=0x3a00; // depth .75
 pixels[0]=0x3c00; // sky, even though the coarse tile is populated
 check(d.write_texture(texture,0,0,std::as_bytes(std::span(pixels)),width*8,&error),"two-plane depth");
 const auto twoPlane=run();
 check(twoPlane[4*width+4].position.w==1,"right-hand coarse receiver exists");
 const std::array<Surface,1> coarse{{twoPlane[4*width+4]}};
 const std::array<V,1> radiance{{{2,1,0,1}}}, tolerance{{{.05F,0,0,0}}};
 const auto storage=[&](std::size_t bytes){
     BufferDesc desc;desc.bytes=bytes;desc.usage=BufferUsage::Storage;
     desc.memory=MemoryDomain::Upload;desc.initialState=ResourceState::ShaderRead;
     return d.create_buffer(desc,&error);
 };
 const auto coarseBuf=storage(sizeof(coarse)),lightBuf=storage(sizeof(radiance)),settingsBuf=storage(sizeof(tolerance));
 check(coarseBuf&&lightBuf&&settingsBuf&&
       d.write_buffer(coarseBuf,0,std::as_bytes(std::span(coarse)),&error)&&
       d.write_buffer(lightBuf,0,std::as_bytes(std::span(radiance)),&error)&&
       d.write_buffer(settingsBuf,0,std::as_bytes(std::span(tolerance)),&error),"upsample buffers");
 TextureDesc outDesc;outDesc.width=width;outDesc.height=height;outDesc.format=TextureFormat::RGBA16Float;
 outDesc.usage=TextureUsage::Storage|TextureUsage::CopySource;outDesc.initialState=ResourceState::ShaderRead;
 const auto outTex=d.create_texture(outDesc,&error);
 const auto outView=d.create_texture_view({outTex,0,1,0,1,"split RC edge fixture"},&error);
 check(outTex&&outView,"upsample texture");
 BindGroupLayoutDesc upLd;upLd.bindings={
     {0,BindingType::SampledTexture,ShaderStage::Compute},
     {1,BindingType::UniformBuffer,ShaderStage::Compute},
     {2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},
     {3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},
     {4,BindingType::StorageBufferReadOnly,ShaderStage::Compute},
     {5,BindingType::StorageTexture,ShaderStage::Compute}};
 const auto upLayout=d.create_bind_group_layout(upLd,&error);
 BindGroupDesc upGd;upGd.layout=upLayout;
 upGd.entries={{0,{},view,0,0,sampler},{1,buffer,{},0,sizeof(constants),{}},
               {2,coarseBuf,{},0,sizeof(coarse),{}},{3,lightBuf,{},0,sizeof(radiance),{}},
               {4,settingsBuf,{},0,sizeof(tolerance),{}},{5,{},outView,0,0,{}}};
 const auto upGroup=d.create_bind_group(upGd,&error);
 ComputePipelineDesc upPd;upPd.bytecode=load(argv[2]);upPd.bindGroupLayouts={upLayout};
 const auto upPipeline=d.create_compute_pipeline(upPd,&error);
 check(upLayout&&upGroup&&upPipeline,"upsample pipeline");
 const auto upCmd=d.begin_commands(QueueKind::Compute,"split RC depth-aware upsample",&error);
 check(upCmd&&d.transition_texture(upCmd,outTex,ResourceState::ShaderRead,ResourceState::ShaderWrite,&error)&&
       d.bind_compute_bind_group(upCmd,0,upGroup,&error)&&d.dispatch(upCmd,upPipeline,1,1,1,&error)&&
       d.transition_texture(upCmd,outTex,ResourceState::ShaderWrite,ResourceState::ShaderRead,&error),"upsample dispatch");
 const auto upFence=d.submit(upCmd,&error);
 check(upFence&&d.fence_complete(upFence),"upsample fence");
 std::array<std::uint16_t,width*height*4> litPixels{};
 check(d.read_texture(outTex,0,0,std::as_writable_bytes(std::span(litPixels)),width*8,&error),"upsample readback");
 const auto pixelAlpha=[&](unsigned x,unsigned y){return litPixels[(y*width+x)*4+3];};
 check(pixelAlpha(6,4)==0x3c00,"same-plane receiver must retain indirect light");
 check(pixelAlpha(1,4)==0,"different depth plane must not receive indirect light");
 check(pixelAlpha(0,0)==0,"sky pixel must not receive indirect light");
 check(litPixels[(4*width+6)*4]==0x4000,"same-plane indirect radiance must survive upsample");
 check(d.destroy_compute_pipeline(upPipeline,&error)&&d.destroy_bind_group(upGroup,&error)&&
       d.destroy_bind_group_layout(upLayout,&error)&&d.destroy_texture_view(outView,&error)&&
       d.destroy_texture(outTex,&error)&&d.destroy_buffer(coarseBuf,&error)&&
       d.destroy_buffer(lightBuf,&error)&&d.destroy_buffer(settingsBuf,&error),"upsample cleanup");
 check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error)&&d.destroy_buffer(buffer,&error)&&d.destroy_buffer(result,&error)&&d.destroy_sampler(sampler,&error)&&d.destroy_texture_view(view,&error)&&d.destroy_texture(texture,&error),"cleanup");if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}std::cout<<"GPU depth reconstruction, invalidation and receiver-aware upsample passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
