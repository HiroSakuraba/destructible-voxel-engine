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
struct V{float x,y,z,w;};struct Sample{V position;int level,sheet,p0,p1;};struct Request{unsigned level,direction,p0,p1;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
unsigned key(const Sample& s){float spacing=s.position.w*float(1u<<s.level);unsigned x=unsigned(int(std::floor(s.position.x/spacing))+128),y=unsigned(int(std::floor(s.position.y/spacing))+128),z=unsigned(int(std::floor(s.position.z/spacing))+128);return (x|(y<<8)|(z<<16)|(unsigned(s.level)<<24)|(unsigned(s.sheet)<<26))+1;}
}
int main(int argc,char** argv){try{
 if(argc!=4)throw std::runtime_error("pass expansion, assignment, and direction shaders");
 VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC geometry: Vulkan unavailable");}
 const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 const auto dispatch=[&](ComputePipelineHandle pipe,BindGroupHandle group,unsigned n){const auto cmd=d.begin_commands(QueueKind::Compute,"split RC geometry",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipe,(n+63)/64,1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");};
 std::vector<Sample> src;for(int i=0;i<70;++i)src.push_back({{float(i-35)+.25F,-.25F,2.25F,1},-9,i%6,0,0});
 std::vector<Sample> candidates(src.size()*4);std::vector<unsigned> slots(512),assigned(candidates.size());std::array<unsigned,2> diag{};
 const auto a=make(src.size()*sizeof(Sample)),b=make(candidates.size()*sizeof(Sample)),c=make(slots.size()*4),e=make(assigned.size()*4),f=make(sizeof(diag));check(a&&b&&c&&e&&f,"probe buffers");
 check(d.write_buffer(a,0,std::as_bytes(std::span(src)),&error)&&d.write_buffer(c,0,std::as_bytes(std::span(slots)),&error)&&d.write_buffer(f,0,std::as_bytes(std::span(diag)),&error),"probe upload");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto l=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=l;gd.entries={{0,a,{},0,src.size()*sizeof(Sample),{}},{1,b,{},0,candidates.size()*sizeof(Sample),{}}};const auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={l};const auto p=d.create_compute_pipeline(pd,&error);
 BindGroupLayoutDesc ald;ald.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{3,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto al=d.create_bind_group_layout(ald,&error);BindGroupDesc ag;ag.layout=al;ag.entries={{0,b,{},0,candidates.size()*sizeof(Sample),{}},{1,c,{},0,slots.size()*4,{}},{2,e,{},0,assigned.size()*4,{}},{3,f,{},0,sizeof(diag),{}}};const auto assignGroup=d.create_bind_group(ag,&error);pd.bytecode=load(argv[2]);pd.bindGroupLayouts={al};const auto assignPipe=d.create_compute_pipeline(pd,&error);check(l&&group&&p&&al&&assignGroup&&assignPipe,"probe pipelines");
 dispatch(p,group,static_cast<unsigned>(src.size()));dispatch(assignPipe,assignGroup,static_cast<unsigned>(candidates.size()));
 check(d.read_buffer(b,0,std::as_writable_bytes(std::span(candidates)),&error)&&d.read_buffer(c,0,std::as_writable_bytes(std::span(slots)),&error)&&d.read_buffer(e,0,std::as_writable_bytes(std::span(assigned)),&error)&&d.read_buffer(f,0,std::as_writable_bytes(std::span(diag)),&error),"probe readback");
 check(diag==std::array<unsigned,2>{},"probe diagnostics");for(unsigned i=0;i<src.size();++i)for(unsigned level=0;level<4;++level){const unsigned j=i*4+level;check(candidates[j].level==int(level)&&candidates[j].sheet==src[i].sheet&&candidates[j].position.x==src[i].position.x,"expanded candidate");check(assigned[j]<slots.size()&&slots[assigned[j]]==key(candidates[j]),"candidate key");}
 std::vector<Request> req;for(unsigned level=0;level<4;++level)for(unsigned dir=0;dir<(32u<<(2*level));++dir)req.push_back({level,dir,0,0});req.push_back({4,0,0,0});req.push_back({0,32,0,0});
 std::vector<V> directions(req.size());const auto q=make(req.size()*sizeof(Request)),o=make(req.size()*sizeof(V));check(q&&o&&d.write_buffer(q,0,std::as_bytes(std::span(req)),&error),"directions upload");BindGroupDesc dd;dd.layout=l;dd.entries={{0,q,{},0,req.size()*sizeof(Request),{}},{1,o,{},0,req.size()*sizeof(V),{}}};const auto dg=d.create_bind_group(dd,&error);pd.bytecode=load(argv[3]);pd.bindGroupLayouts={l};const auto dp=d.create_compute_pipeline(pd,&error);check(dg&&dp,"direction pipeline");dispatch(dp,dg,static_cast<unsigned>(req.size()));check(d.read_buffer(o,0,std::as_writable_bytes(std::span(directions)),&error),"direction readback");
 constexpr double pi=3.14159265358979323846;for(unsigned i=0;i+2<req.size();++i){const auto r=req[i];const auto v=directions[i];unsigned base=r.direction>>(2*r.level),x=base&7u,y=base>>3;for(unsigned k=0;k<r.level;++k){const unsigned bit=(r.direction>>(2*(r.level-k-1)))&3;x=x*2+(bit&1);y=y*2+(bit>>1);}const double u=(double(x)+.5)/double(8u<<r.level),vv=(double(y)+.5)/double(4u<<r.level),yy=1-2*vv,rr=std::sqrt(1-yy*yy);check(v.w==1&&std::abs(v.x-rr*std::cos(2*pi*u))<1e-5&&std::abs(v.y-yy)<1e-5&&std::abs(v.z-rr*std::sin(2*pi*u))<1e-5,"equal-area direction "+std::to_string(i));}
 check(directions[req.size()-2].w==0&&directions.back().w==0,"invalid direction rejection");
 check(d.destroy_compute_pipeline(p,&error)&&d.destroy_compute_pipeline(assignPipe,&error)&&d.destroy_compute_pipeline(dp,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group(assignGroup,&error)&&d.destroy_bind_group(dg,&error)&&d.destroy_bind_group_layout(l,&error)&&d.destroy_bind_group_layout(al,&error),"cleanup");for(auto h:{a,b,c,e,f,q,o})check(d.destroy_buffer(h,&error),"buffer cleanup");
 if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<src.size()*4<<" world probes and "<<req.size()-2<<" hierarchical directions passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
