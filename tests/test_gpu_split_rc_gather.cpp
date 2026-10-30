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
struct V{float x,y,z,w;};struct Q{std::array<unsigned,4> probe;V normal;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
bool eq(float a,float b){return std::abs(a-b)<1e-5F;}
}
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("pass diffuse gather shader");VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC diffuse gather: Vulkan unavailable");}
 std::array<unsigned,5> keys{1,2,0,4,(1u<<24)+1},offsets{0,32,0xffffffffu,64,96};std::array<V,32> dirs{};std::array<V,128> lights{};
 constexpr double pi=3.14159265358979323846;for(unsigned i=0;i<32;++i){double u=(double(i%8)+.5)/8,v=(double(i/8)+.5)/4,y=1-2*v,r=std::sqrt(1-y*y);dirs[i]={float(r*std::cos(2*pi*u)),float(y),float(r*std::sin(2*pi*u)),1};lights[i]={.8F,.4F,.2F,1};lights[32+i]=y>0?V{1,.5F,.25F,1}:V{0,0,0,1};}
 std::vector<Q> q={{{0,0,0,0},{0,1,0,0}},{{0,0,0,0},{1,0,0,0}},{{1,0,0,0},{0,1,0,0}},{{1,0,0,0},{0,-1,0,0}},{{1,0,0,0},{1,0,0,0}},{{2,0,0,0},{0,1,0,0}},{{0,0,0,0},{0,0,0,0}},{{3,0,0,0},{0,1,0,0}},{{4,0,0,0},{0,1,0,0}}};
 const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 const auto a=make(sizeof(keys)),b=make(sizeof(offsets)),c=make(sizeof(lights)),e=make(sizeof(dirs)),f=make(q.size()*sizeof(Q)),g=make(q.size()*sizeof(V));check(a&&b&&c&&e&&f&&g,"buffers");check(d.write_buffer(a,0,std::as_bytes(std::span(keys)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(offsets)),&error)&&d.write_buffer(c,0,std::as_bytes(std::span(lights)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(dirs)),&error)&&d.write_buffer(f,0,std::as_bytes(std::span(q)),&error),"upload");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{5,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,a,{},0,sizeof(keys),{}},{1,b,{},0,sizeof(offsets),{}},{2,c,{},0,sizeof(lights),{}},{3,e,{},0,sizeof(dirs),{}},{4,f,{},0,q.size()*sizeof(Q),{}},{5,g,{},0,q.size()*sizeof(V),{}}};const auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};const auto pipe=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipe,"pipeline");
 const auto cmd=d.begin_commands(QueueKind::Compute,"split RC diffuse gather",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipe,1,1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");std::vector<V> out(q.size());check(d.read_buffer(g,0,std::as_writable_bytes(std::span(out)),&error),"readback");
 for(unsigned i:{0u,1u})check(eq(out[i].x,.8F)&&eq(out[i].y,.4F)&&eq(out[i].z,.2F)&&eq(out[i].w,1),"uniform radiance");
 check(eq(out[2].x,1)&&eq(out[2].y,.5F)&&eq(out[2].z,.25F)&&eq(out[2].w,1),"upward light");check(eq(out[3].x,0)&&eq(out[3].w,1),"blocked directions remain dark");check(eq(out[4].x,.5F)&&eq(out[4].y,.25F)&&eq(out[4].z,.125F)&&eq(out[4].w,1),"cosine-weighted side light");
 for(unsigned i:{5u,6u,7u,8u})check(eq(out[i].w,0),"invalid or missing coverage");
 check(d.destroy_compute_pipeline(pipe,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error),"cleanup");for(auto h:{a,b,c,e,f,g})check(d.destroy_buffer(h,&error),"buffer cleanup");if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<"Diffuse gather, hemisphere occlusion and missing coverage passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
