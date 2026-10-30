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
std::string error;
void check(bool v,const std::string& m){if(!v)throw std::runtime_error(m+": "+error);}
struct Vec4 {float x,y,z,w;};
struct Ray {Vec4 origin, direction, sky;};
struct SplitRay {Vec4 hit,sky;};
static_assert(sizeof(Ray)==48 && sizeof(SplitRay)==32 && sizeof(Vec4)==16);
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
bool eq(float a,float b){return std::abs(a-b)<1e-4F;}
}
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("pass voxel-trace, split-interval and merge SPIR-V files");
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC voxel trace: Vulkan unavailable");}
    std::vector<Ray> rays={{{-1,1.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}},{{1.5F,1.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}},{{-1,5.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}},{{-1,1.5F,1.5F,.5F},{1,0,0,2.5F},{.1F,.2F,.3F,0}}};
    std::array<Vec4,2> info{{{0,0,0,1},{8,8,8,0}}};std::array<Vec4,2> palette{{{1,1,1,0},{.7F,.4F,.2F,0}}};std::vector<unsigned> grid(128,0);const unsigned cell=(1*8+1)*8+2;grid[cell/4]|=1u<<((cell%4)*8);
    const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
    const auto a=make(rays.size()*sizeof(Ray)),b=make(sizeof(info)),c=make(grid.size()*4),e=make(sizeof(palette)),f=make(rays.size()*sizeof(SplitRay)),g=make(rays.size()*4*sizeof(Vec4)),h=make(rays.size()*4*sizeof(Vec4));check(a&&b&&c&&e&&f&&g&&h,"buffers");
    check(d.write_buffer(a,0,std::as_bytes(std::span(rays)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(info)),&error)&&d.write_buffer(c,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(palette)),&error),"upload grid");
    BindGroupLayoutDesc traceLd;traceLd.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
    const auto traceLayout=d.create_bind_group_layout(traceLd,&error);BindGroupDesc traceGroup;traceGroup.layout=traceLayout;traceGroup.entries={{0,a,{},0,rays.size()*sizeof(Ray),{}},{1,b,{},0,sizeof(info),{}},{2,c,{},0,grid.size()*4,{}},{3,e,{},0,sizeof(palette),{}},{4,f,{},0,rays.size()*sizeof(SplitRay),{}}};const auto tg=d.create_bind_group(traceGroup,&error);
    BindGroupLayoutDesc splitLd;splitLd.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto splitLayout=d.create_bind_group_layout(splitLd,&error);BindGroupDesc splitGroup;splitGroup.layout=splitLayout;splitGroup.entries={{0,f,{},0,rays.size()*sizeof(SplitRay),{}},{1,g,{},0,rays.size()*4*sizeof(Vec4),{}}};const auto sg=d.create_bind_group(splitGroup,&error);
    BindGroupDesc mergeGroup;mergeGroup.layout=splitLayout;mergeGroup.entries={{0,g,{},0,rays.size()*4*sizeof(Vec4),{}},{1,h,{},0,rays.size()*4*sizeof(Vec4),{}}};const auto mg=d.create_bind_group(mergeGroup,&error);
    ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={traceLayout};const auto tp=d.create_compute_pipeline(pd,&error);pd.bytecode=load(argv[2]);pd.bindGroupLayouts={splitLayout};const auto sp=d.create_compute_pipeline(pd,&error);pd.bytecode=load(argv[3]);const auto mp=d.create_compute_pipeline(pd,&error);check(traceLayout&&tg&&splitLayout&&sg&&mg&&tp&&sp&&mp,"pipelines");
    const auto dispatch=[&](ComputePipelineHandle pipeline,BindGroupHandle group){const auto cmd=d.begin_commands(QueueKind::Compute,"split RC voxel conformance",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipeline,1,1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");};
    const auto evaluate=[&](bool destroyed){dispatch(tp,tg);dispatch(sp,sg);dispatch(mp,mg);std::vector<SplitRay> traced(rays.size());std::vector<Vec4> intervals(rays.size()*4), merged(rays.size()*4);check(d.read_buffer(f,0,std::as_writable_bytes(std::span(traced)),&error)&&d.read_buffer(g,0,std::as_writable_bytes(std::span(intervals)),&error)&&d.read_buffer(h,0,std::as_writable_bytes(std::span(merged)),&error),"readback");
        for(unsigned i=0;i<rays.size();++i){const bool hit=!destroyed&&i<2;const unsigned term=hit?(i==0?2:1):3;
            check(eq(traced[i].hit.w,hit?(i==0?3.F:.5F):-1.F),"traversal distance "+std::to_string(i));
            for(unsigned level=0;level<4;++level){const Vec4 v=intervals[i*4+level];const float beta=level<term?1.F:level==term?0.F:-1.F;
                const float x=level==term?(hit?.7F:.1F):0.F,y=level==term?(hit?.4F:.2F):0.F,z=level==term?(hit?.2F:.3F):0.F;
                check(eq(v.x,x)&&eq(v.y,y)&&eq(v.z,z)&&eq(v.w,beta),"trace/split interval "+std::to_string(i)+"/"+std::to_string(level));
                const Vec4 m=merged[i*4+level];const bool valid=level<=term;
                check(eq(m.w,valid?1.F:0.F)&&eq(m.x,valid?(hit?.7F:.1F):0.F)&&eq(m.y,valid?(hit?.4F:.2F):0.F)&&eq(m.z,valid?(hit?.2F:.3F):0.F),"merged radiance "+std::to_string(i)+"/"+std::to_string(level));}}
    };
    evaluate(false);grid[cell/4]=0;check(d.write_buffer(c,0,std::as_bytes(std::span(grid)),&error),"destroy voxel");evaluate(true);
    // Non-binary throughput and a missing parent must compose without
    // manufacturing light through absent probe coverage.
    std::vector<Vec4> synthetic(rays.size()*4,Vec4{0,0,0,-1});
    synthetic[0]={.1F,.1F,.1F,.5F};synthetic[1]={0,.1F,0,.5F};
    synthetic[2]={.1F,0,0,.5F};synthetic[3]={.4F,.3F,.2F,0};
    synthetic[4]={.2F,0,0,1};synthetic[6]={.5F,.5F,.5F,0};
    check(d.write_buffer(g,0,std::as_bytes(std::span(synthetic)),&error),"upload merge fixtures");dispatch(mp,mg);
    std::vector<Vec4> mergedFixture(synthetic.size());check(d.read_buffer(h,0,std::as_writable_bytes(std::span(mergedFixture)),&error),"read merge fixtures");
    check(eq(mergedFixture[0].x,.175F)&&eq(mergedFixture[0].y,.1875F)&&eq(mergedFixture[0].z,.125F)&&eq(mergedFixture[0].w,1),"fractional transmittance composition");
    check(eq(mergedFixture[4].w,0)&&eq(mergedFixture[5].w,0)&&eq(mergedFixture[6].w,1),"missing parent remains unavailable");
    check(d.destroy_compute_pipeline(tp,&error)&&d.destroy_compute_pipeline(sp,&error)&&d.destroy_compute_pipeline(mp,&error)&&d.destroy_bind_group(tg,&error)&&d.destroy_bind_group(sg,&error)&&d.destroy_bind_group(mg,&error)&&d.destroy_bind_group_layout(traceLayout,&error)&&d.destroy_bind_group_layout(splitLayout,&error),"pipeline cleanup");
    for(const auto buffer:{a,b,c,e,f,g,h})check(d.destroy_buffer(buffer,&error),"buffer cleanup");
    if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
    std::cout<<"Voxel hits, misses, distance limit and destruction split correctly on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
