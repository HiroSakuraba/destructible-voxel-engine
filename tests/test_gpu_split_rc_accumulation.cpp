#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
using namespace dve::rhi;
namespace {
std::string error;
void check(bool b,const std::string& m){if(!b)throw std::runtime_error(m+": "+error);}
struct Vec4 {float x,y,z,w;};
struct Sample {float x,y,z,base;int level,sheet,p0,p1;};
struct TraceRay { Vec4 origin,direction,sky; };
struct Traced { Vec4 hit,sky; };
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
}
int main(int argc,char** argv){try{
 if(argc!=7)throw std::runtime_error("pass probe, accumulation, resolve, voxel trace, split and compact SPIR-V paths");
 VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC accumulation: Vulkan unavailable");}
 constexpr unsigned capacity=64, bins=capacity*32;
 std::vector<Sample> samples;std::vector<Vec4> intervals;std::vector<unsigned> dirs;
 for(unsigned i=0;i<256;++i){samples.push_back({i%2?2.25F:.25F,.25F,.25F,1,0,0,0,0});intervals.push_back({i%2?.5F:.25F,.125F,.75F,float((i/2)%2)});dirs.push_back((i/2)%4);}
 for(unsigned i=0;i<2;++i){samples.push_back({4.25F,.25F,.25F,1,0,0,0,0});intervals.push_back({600000,0,0,0});dirs.push_back(0);}
 samples.push_back({.25F,.25F,.25F,1,0,0,0,0});intervals.push_back({1,1,1,0});dirs.push_back(32); // invalid direction
 samples.push_back({.25F,.25F,.25F,1,0,0,0,0});intervals.push_back({std::numeric_limits<float>::quiet_NaN(),0,0,0});dirs.push_back(0);
 samples.push_back({.25F,.25F,.25F,1,0,0,0,0});intervals.push_back({0,0,0,-1});dirs.push_back(0); // absent
 const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 const auto n=samples.size();const auto a=make(n*sizeof(Sample)),b=make(capacity*4),c=make(n*4),e=make(8),f=make(n*sizeof(Vec4)),g=make(n*4),h=make(16),j=make(bins*6*4),k=make(12),l=make(bins*sizeof(Vec4)),m=make(bins*4),offsetBuffer=make(capacity*4),limitBuffer=make(4),compactDiag=make(12);
 check(a&&b&&c&&e&&f&&g&&h&&j&&k&&l&&m&&offsetBuffer&&limitBuffer&&compactDiag,"buffers");
 std::vector<unsigned> zeroSlots(capacity),zeroSums(bins*6);std::array<unsigned,2> probeDiag{};std::array<unsigned,3> diag{};std::array<unsigned,4> config{capacity,bins,0,0};
 check(d.write_buffer(a,0,std::as_bytes(std::span(samples)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(zeroSlots)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(probeDiag)),&error)&&d.write_buffer(f,0,std::as_bytes(std::span(intervals)),&error)&&d.write_buffer(g,0,std::as_bytes(std::span(dirs)),&error)&&d.write_buffer(h,0,std::as_bytes(std::span(config)),&error)&&d.write_buffer(j,0,std::as_bytes(std::span(zeroSums)),&error)&&d.write_buffer(k,0,std::as_bytes(std::span(diag)),&error)&&d.write_buffer(limitBuffer,0,std::as_bytes(std::span(&bins,1)),&error),"upload");
 BindGroupLayoutDesc pld;pld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{3,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
 const auto pl=d.create_bind_group_layout(pld,&error);BindGroupDesc pg;pg.layout=pl;pg.entries={{0,a,{},0,n*sizeof(Sample),{}},{1,b,{},0,capacity*4,{}},{2,c,{},0,n*4,{}},{3,e,{},0,8,{}}};const auto pgroup=d.create_bind_group(pg,&error);
 BindGroupLayoutDesc ald;ald.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{5,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{6,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{7,BindingType::StorageBufferReadOnly,ShaderStage::Compute}};
 const auto al=d.create_bind_group_layout(ald,&error);BindGroupDesc ag;ag.layout=al;ag.entries={{0,f,{},0,n*sizeof(Vec4),{}},{1,c,{},0,n*4,{}},{2,g,{},0,n*4,{}},{3,h,{},0,16,{}},{4,j,{},0,bins*6*4,{}},{5,k,{},0,12,{}},{6,offsetBuffer,{},0,capacity*4,{}},{7,b,{},0,capacity*4,{}}};const auto agroup=d.create_bind_group(ag,&error);
 BindGroupLayoutDesc rld;rld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
 const auto rl=d.create_bind_group_layout(rld,&error);BindGroupDesc rg;rg.layout=rl;rg.entries={{0,j,{},0,bins*6*4,{}},{1,l,{},0,bins*sizeof(Vec4),{}},{2,m,{},0,bins*4,{}}};const auto rgroup=d.create_bind_group(rg,&error);
 ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={pl};const auto pp=d.create_compute_pipeline(pd,&error);pd.bytecode=load(argv[2]);pd.bindGroupLayouts={al};const auto ap=d.create_compute_pipeline(pd,&error);pd.bytecode=load(argv[3]);pd.bindGroupLayouts={rl};const auto rp=d.create_compute_pipeline(pd,&error);check(pl&&pgroup&&al&&agroup&&rl&&rgroup&&pp&&ap&&rp,"pipelines");
 const auto dispatch=[&](ComputePipelineHandle pipe,BindGroupHandle group,unsigned count){auto cmd=d.begin_commands(QueueKind::Compute,"split RC accumulation",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipe,(count+63)/64,1,1,&error),"dispatch");auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");};
 BindGroupLayoutDesc cld;cld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{3,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
 const auto compactLayout=d.create_bind_group_layout(cld,&error);BindGroupDesc cg;cg.layout=compactLayout;cg.entries={{0,b,{},0,capacity*4,{}},{1,limitBuffer,{},0,4,{}},{2,offsetBuffer,{},0,capacity*4,{}},{3,compactDiag,{},0,12,{}}};const auto compactGroup=d.create_bind_group(cg,&error);pd.bytecode=load(argv[6]);pd.bindGroupLayouts={compactLayout};const auto compactPipe=d.create_compute_pipeline(pd,&error);check(compactLayout&&compactGroup&&compactPipe,"compact pipeline");
 dispatch(pp,pgroup,static_cast<unsigned>(n));dispatch(compactPipe,compactGroup,1);dispatch(ap,agroup,static_cast<unsigned>(n));dispatch(rp,rgroup,bins);
 std::vector<unsigned> gpuOffsets(capacity);std::array<unsigned,3> compactInfo{};check(d.read_buffer(offsetBuffer,0,std::as_writable_bytes(std::span(gpuOffsets)),&error)&&d.read_buffer(compactDiag,0,std::as_writable_bytes(std::span(compactInfo)),&error),"compact readback");check(compactInfo[0]==96&&compactInfo[1]==0&&compactInfo[2]==0,"compact allocation");
 std::vector<unsigned> slots(n),sums(bins*6),valid(bins);std::vector<Vec4> means(bins);
 check(d.read_buffer(c,0,std::as_writable_bytes(std::span(slots)),&error)&&d.read_buffer(j,0,std::as_writable_bytes(std::span(sums)),&error)&&d.read_buffer(k,0,std::as_writable_bytes(std::span(diag)),&error)&&d.read_buffer(l,0,std::as_writable_bytes(std::span(means)),&error)&&d.read_buffer(m,0,std::as_writable_bytes(std::span(valid)),&error),"readback");
 check(diag[0]==1&&diag[1]==1&&diag[2]==1,"diagnostics");
 std::vector<std::array<unsigned,5>> expected(bins);
 for(unsigned i=0;i<256;++i){check(slots[i]<capacity,"slot");auto& x=expected[gpuOffsets[slots[i]]+dirs[i]];x[0]+=unsigned(std::lround(intervals[i].x*4096));x[1]+=512;x[2]+=3072;x[3]+=unsigned(intervals[i].w*4096);++x[4];}
 for(unsigned bin=0;bin<bins;++bin){const auto& x=expected[bin];if(x[4]){check(valid[bin]==1,"sampled bin valid");for(unsigned q=0;q<5;++q)check(sums[bin*6+q]==x[q],"atomic sum");check(std::abs(means[bin].x-float(x[0])/(4096*x[4]))<1e-6F&&std::abs(means[bin].w-float(x[3])/(4096*x[4]))<1e-6F,"resolved mean");}else check(valid[bin]==0&&means[bin].w==-1.F,"empty or overflow bin invalid");}
 check(slots[256]==slots[257]&&sums[gpuOffsets[slots[256]]*6+5]==1,"overflow flagged");
 // Exercise the actual GPU handoff from voxel trace -> splitting -> the same
 // probe assignments -> accumulation -> resolve. The synthetic assignments
 // exercise the buffer chain; they are not yet a spatial cascade hierarchy.
 constexpr unsigned rayCount=64, intervalCount=rayCount*4;
 std::vector<TraceRay> rays(rayCount,TraceRay{{-1,1.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}});
 std::array<Vec4,2> gridInfo{{{0,0,0,1},{8,8,8,0}}},palette{{{1,1,1,0},{.75F,.5F,.25F,0}}};
 std::vector<unsigned> grid(128);const unsigned cell=(1*8+1)*8+2;grid[cell/4]|=1u<<((cell%4)*8);
 const auto rayBuffer=make(rays.size()*sizeof(TraceRay)),infoBuffer=make(sizeof(gridInfo)),gridBuffer=make(grid.size()*4),paletteBuffer=make(sizeof(palette)),tracedBuffer=make(rayCount*sizeof(Traced));
 check(rayBuffer&&infoBuffer&&gridBuffer&&paletteBuffer&&tracedBuffer,"voxel buffers");
 check(d.write_buffer(rayBuffer,0,std::as_bytes(std::span(rays)),&error)&&d.write_buffer(infoBuffer,0,std::as_bytes(std::span(gridInfo)),&error)&&d.write_buffer(gridBuffer,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(paletteBuffer,0,std::as_bytes(std::span(palette)),&error)&&d.write_buffer(j,0,std::as_bytes(std::span(zeroSums)),&error)&&d.write_buffer(k,0,std::as_bytes(std::span(diag=std::array<unsigned,3>{})),&error),"voxel pipeline upload");
 BindGroupLayoutDesc tld;tld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
 const auto tl=d.create_bind_group_layout(tld,&error);BindGroupDesc tg;tg.layout=tl;tg.entries={{0,rayBuffer,{},0,rays.size()*sizeof(TraceRay),{}},{1,infoBuffer,{},0,sizeof(gridInfo),{}},{2,gridBuffer,{},0,grid.size()*4,{}},{3,paletteBuffer,{},0,sizeof(palette),{}},{4,tracedBuffer,{},0,rayCount*sizeof(Traced),{}}};const auto tgroup=d.create_bind_group(tg,&error);
 BindGroupLayoutDesc sld;sld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
 const auto sl=d.create_bind_group_layout(sld,&error);BindGroupDesc sg;sg.layout=sl;sg.entries={{0,tracedBuffer,{},0,rayCount*sizeof(Traced),{}},{1,f,{},0,intervalCount*sizeof(Vec4),{}}};const auto sgroup=d.create_bind_group(sg,&error);
 BindGroupDesc secondAg;secondAg.layout=al;secondAg.entries={{0,f,{},0,intervalCount*sizeof(Vec4),{}},{1,c,{},0,intervalCount*4,{}},{2,g,{},0,intervalCount*4,{}},{3,h,{},0,16,{}},{4,j,{},0,bins*6*4,{}},{5,k,{},0,12,{}},{6,offsetBuffer,{},0,capacity*4,{}},{7,b,{},0,capacity*4,{}}};const auto secondGroup=d.create_bind_group(secondAg,&error);
 pd.bytecode=load(argv[4]);pd.bindGroupLayouts={tl};const auto tp=d.create_compute_pipeline(pd,&error);pd.bytecode=load(argv[5]);pd.bindGroupLayouts={sl};const auto sp=d.create_compute_pipeline(pd,&error);check(tl&&tgroup&&sl&&sgroup&&secondGroup&&tp&&sp,"voxel pipeline");
 dispatch(tp,tgroup,rayCount);dispatch(sp,sgroup,rayCount);dispatch(ap,secondGroup,intervalCount);dispatch(rp,rgroup,bins);
 check(d.read_buffer(j,0,std::as_writable_bytes(std::span(sums)),&error)&&d.read_buffer(k,0,std::as_writable_bytes(std::span(diag)),&error)&&d.read_buffer(l,0,std::as_writable_bytes(std::span(means)),&error)&&d.read_buffer(m,0,std::as_writable_bytes(std::span(valid)),&error),"voxel chain readback");
 check(diag==std::array<unsigned,3>{},"voxel chain diagnostics");
 std::vector<std::array<unsigned,5>> tracedExpected(bins);
 for(unsigned i=0;i<intervalCount;++i){const unsigned level=i%4;if(level==3)continue;auto& x=tracedExpected[gpuOffsets[slots[i]]+dirs[i]];if(level==2){x[0]+=3072;x[1]+=2048;x[2]+=1024;}else x[3]+=4096;++x[4];}
 for(unsigned bin=0;bin<bins;++bin){const auto& x=tracedExpected[bin];check(valid[bin]==(x[4]?1u:0u),"voxel chain valid");if(x[4])for(unsigned field=0;field<5;++field)check(sums[bin*6+field]==x[field],"voxel chain sums");}
 check(d.destroy_compute_pipeline(tp,&error)&&d.destroy_compute_pipeline(sp,&error)&&d.destroy_bind_group(tgroup,&error)&&d.destroy_bind_group(sgroup,&error)&&d.destroy_bind_group(secondGroup,&error)&&d.destroy_bind_group_layout(tl,&error)&&d.destroy_bind_group_layout(sl,&error),"voxel pipeline cleanup");
 for(auto buffer:{rayBuffer,infoBuffer,gridBuffer,paletteBuffer,tracedBuffer})check(d.destroy_buffer(buffer,&error),"voxel buffer cleanup");
 check(d.destroy_compute_pipeline(compactPipe,&error)&&d.destroy_bind_group(compactGroup,&error)&&d.destroy_bind_group_layout(compactLayout,&error)&&d.destroy_compute_pipeline(pp,&error)&&d.destroy_compute_pipeline(ap,&error)&&d.destroy_compute_pipeline(rp,&error)&&d.destroy_bind_group(pgroup,&error)&&d.destroy_bind_group(agroup,&error)&&d.destroy_bind_group(rgroup,&error)&&d.destroy_bind_group_layout(pl,&error)&&d.destroy_bind_group_layout(al,&error)&&d.destroy_bind_group_layout(rl,&error),"pipeline cleanup");
 for(auto buffer:{a,b,c,e,f,g,h,j,k,l,m,offsetBuffer,limitBuffer,compactDiag})check(d.destroy_buffer(buffer,&error),"buffer cleanup");
 if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<"GPU probe assignment, concurrent interval accumulation, resolve and overflow passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
