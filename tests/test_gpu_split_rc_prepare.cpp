#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <algorithm>
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
struct V{float x,y,z,w;};struct Surface{V origin;std::array<unsigned,4> slots;std::array<unsigned,4> sampleInfo;};struct Ray{V origin,dir,unused;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
}
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("pass prepare shader SPIR-V path");VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Vulkan unavailable");}
 constexpr unsigned ns=64,nr=ns*32,ni=nr*4;std::array<Surface,ns> surfaces{};for(unsigned i=0;i<ns;++i)surfaces[i]={{float(i),2,3,.25F},{10,20,30,40},{63u-i,0,0,0}};
 std::array<V,32> directions{};std::array<V,1> config{{{12,0,0,0}}};std::vector<Ray> rays(nr);std::vector<unsigned> slots(ni),bins(ni);
 const auto make=[&](std::size_t n){return d.create_buffer({n,BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination,MemoryDomain::Upload,"prepare test",ResourceState::ShaderRead},&error);};
 auto sb=make(sizeof(surfaces)),db=make(sizeof(directions)),cb=make(sizeof(config)),rb=make(rays.size()*sizeof(Ray)),sl=make(slots.size()*4),di=make(bins.size()*4);check(sb&&db&&cb&&rb&&sl&&di,"buffers");
 check(d.write_buffer(sb,0,std::as_bytes(std::span(surfaces)),&error)&&d.write_buffer(db,0,std::as_bytes(std::span(directions)),&error)&&d.write_buffer(cb,0,std::as_bytes(std::span(config)),&error),"upload");
 const auto R=BindingType::StorageBufferReadOnly,W=BindingType::StorageBufferReadWrite;BindGroupLayoutDesc ld;ld.bindings={{0,R,ShaderStage::Compute},{1,R,ShaderStage::Compute},{2,R,ShaderStage::Compute},{3,W,ShaderStage::Compute},{4,W,ShaderStage::Compute},{5,W,ShaderStage::Compute}};auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,sb,{},0,sizeof(surfaces),{}},{1,db,{},0,sizeof(directions),{}},{2,cb,{},0,sizeof(config),{}},{3,rb,{},0,rays.size()*sizeof(Ray),{}},{4,sl,{},0,slots.size()*4,{}},{5,di,{},0,bins.size()*4,{}}};auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};auto pipe=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipe,"pipeline");
 auto cmd=d.begin_commands(QueueKind::Compute,"split RC angular fixture",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipe,(nr+63)/64,1,1,&error),"dispatch");auto f=d.submit(cmd,&error);check(f&&d.fence_complete(f),"fence");check(d.read_buffer(rb,0,std::as_writable_bytes(std::span(rays)),&error)&&d.read_buffer(sl,0,std::as_writable_bytes(std::span(slots)),&error)&&d.read_buffer(di,0,std::as_writable_bytes(std::span(bins)),&error),"readback");
 constexpr float pi=3.14159265358979323846F;for(unsigned j=0;j<32;++j){std::array<bool,64> seen{};for(unsigned i=0;i<ns;++i){unsigned n=i*32+j;const auto r=rays[n];check(r.origin.x==float(i)&&r.origin.w==.25F&&r.dir.w==12,"ray values");check(std::abs(r.dir.x*r.dir.x+r.dir.y*r.dir.y+r.dir.z*r.dir.z-1)<1e-5F,"unit direction");
  const unsigned fine=bins[n*4+3];check((fine>>6)==j,"base bin");check((fine&63u)==(((63u-i)*37u+j*17u)&63u),"screen sample seed");seen[fine&63]=true;
  for(unsigned l=0;l<4;++l){check(slots[n*4+l]==10*(l+1),"probe slot");check(bins[n*4+l]==(fine>>(2*(3-l))),"nested direction bin");}
  float angle=std::atan2(r.dir.z,r.dir.x);if(angle<0)angle+=2*pi;const unsigned x=std::min(63u,unsigned(angle/(2*pi)*64));const unsigned y=std::min(31u,unsigned((1-r.dir.y)*16));
  unsigned xbin=j&7u,ybin=j>>3;for(unsigned level=0;level<3;++level){unsigned bits=(fine>>(2*(2-level)))&3u;xbin=xbin*2+(bits&1);ybin=ybin*2+(bits>>1);}check(x==xbin&&y==ybin,"ray falls inside assigned bin");
 }for(bool present:seen)check(present,"all 64 child directions covered");}
 check(d.destroy_compute_pipeline(pipe,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error),"pipeline cleanup");for(auto b:{sb,db,cb,rb,sl,di})check(d.destroy_buffer(b,&error),"buffer cleanup");if(std::getenv("DVE_VULKAN_VALIDATION"))check(d.validation_enabled()&&d.validation_error_count()==0,"Vulkan validation");std::cout<<"angular distribution and nested bin mapping passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
