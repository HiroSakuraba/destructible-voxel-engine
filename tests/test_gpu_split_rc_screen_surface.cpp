#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
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
 if(argc!=2)throw std::runtime_error("pass surface shader SPIR-V path");VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC screen surface: Vulkan unavailable");}
 constexpr unsigned width=8,height=8;std::vector<std::uint16_t> pixels(width*height*4,0);for(unsigned i=0;i<width*height;++i)pixels[i*4]=0x3800;pixels[0]=0x3c00;
 TextureDesc td;td.width=width;td.height=height;td.format=TextureFormat::RGBA16Float;td.usage=TextureUsage::Sampled|TextureUsage::CopyDestination;td.initialState=ResourceState::ShaderRead;auto texture=d.create_texture(td,&error);auto view=d.create_texture_view({texture,0,1,0,1,"depth fixture"},&error);SamplerDesc sd;sd.minFilter=sd.magFilter=FilterMode::Nearest;sd.addressU=sd.addressV=sd.addressW=AddressMode::ClampToEdge;auto sampler=d.create_sampler(sd,&error);check(texture&&view&&sampler&&d.write_texture(texture,0,0,std::as_bytes(std::span(pixels)),width*8,&error),"depth texture");
 std::array<float,128> constants{};for(unsigned i=0;i<4;++i){constants[i*5]=1;constants[16+i*5]=1;}constants[34]=2;constants[35]=1;constants[40]=width;constants[41]=height;constants[42]=1.F/width;constants[43]=1.F/height;constants[70]=1;
 BufferDesc cb;cb.bytes=sizeof(constants);cb.usage=BufferUsage::Constant;cb.memory=MemoryDomain::Upload;cb.initialState=ResourceState::ShaderRead;auto buffer=d.create_buffer(cb,&error);BufferDesc output;output.bytes=width*height*sizeof(Surface);output.usage=BufferUsage::Storage|BufferUsage::CopySource;output.memory=MemoryDomain::Upload;output.initialState=ResourceState::ShaderRead;auto result=d.create_buffer(output,&error);check(buffer&&result&&d.write_buffer(buffer,0,std::as_bytes(std::span(constants)),&error),"buffers");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::SampledTexture,ShaderStage::Compute},{1,BindingType::UniformBuffer,ShaderStage::Compute},{9,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,{},view,0,0,sampler},{1,buffer,{},0,sizeof(constants),{}},{9,result,{},0,output.bytes,{}}};auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"pipeline");
 const auto run=[&](){auto commands=d.begin_commands(QueueKind::Compute,"split RC screen surfaces",&error);check(d.bind_compute_bind_group(commands,0,group,&error)&&d.dispatch(commands,pipeline,1,1,1,&error),"dispatch");auto fence=d.submit(commands,&error);check(fence&&d.fence_complete(fence),"fence");std::array<Surface,width*height> out{};check(d.read_buffer(result,0,std::as_writable_bytes(std::span(out)),&error),"readback");return out;};
 auto out=run();auto hit=out[4*width+4];check(hit.position.w==1&&std::abs(hit.position.x-.125F)<1e-5F&&std::abs(hit.position.y-.125F)<1e-5F&&std::abs(hit.position.z-.5F)<1e-5F,"reconstructed world position");check(hit.normal.w==1&&hit.normal.z>.999F,"camera-facing plane normal");check(out[0].position.w==0,"sky pixel is not a surface");
 pixels[(4*width+4)*4]=0x3c00;check(d.write_texture(texture,0,0,std::as_bytes(std::span(pixels)),width*8,&error),"edit depth");out=run();check(out[4*width+4].position.w==0,"changed depth removes surface");
 check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error)&&d.destroy_buffer(buffer,&error)&&d.destroy_buffer(result,&error)&&d.destroy_sampler(sampler,&error)&&d.destroy_texture_view(view,&error)&&d.destroy_texture(texture,&error),"cleanup");if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}std::cout<<"GPU depth surface reconstruction and invalidation passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
