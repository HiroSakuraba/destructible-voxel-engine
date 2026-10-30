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
std::string error;
void check(bool b,const std::string& m){if(!b)throw std::runtime_error(m+": "+error);}
struct V {float x,y,z,w;}; struct Q {unsigned slot,dir;}; struct GatherQ {std::array<unsigned,4> slot; V normal;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
unsigned hash(unsigned k){k^=k>>16;k*=0x7feb352dU;k^=k>>15;k*=0x846ca68bU;return k^(k>>16);}
unsigned key(int x,int y,int z,unsigned level,unsigned sheet){return (unsigned(x+128)|(unsigned(y+128)<<8)|(unsigned(z+128)<<16)|(level<<24)|(sheet<<26))+1;}
unsigned insert(std::vector<unsigned>& keys,unsigned k){const unsigned start=hash(k)%static_cast<unsigned>(keys.size());for(unsigned n=0;n<keys.size();++n){const auto i=(start+n)%keys.size();if(!keys[i]||keys[i]==k){keys[i]=k;return static_cast<unsigned>(i);}}throw std::runtime_error("fixture table full");}
bool eq(float a,float b){return std::abs(a-b)<1e-5F;}
}
int main(int argc,char** argv){try{
 if(argc!=5)throw std::runtime_error("pass spatial merge, compact allocator, scatter, and gather SPIR-V paths");
 VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC spatial merge: Vulkan unavailable");}
 constexpr unsigned capacity=64;std::vector<unsigned> keys(capacity);
 const auto child=insert(keys,key(0,0,0,0,0)),far=insert(keys,key(20,0,0,0,0)),other=insert(keys,key(0,0,0,0,1)),top=insert(keys,key(0,0,0,3,0));
 std::array<unsigned,8> parentSlots{};unsigned cornerSlot{},index=0;
 for(int z=-1;z<=0;++z)for(int y=-1;y<=0;++y)for(int x=-1;x<=0;++x){const auto slot=insert(keys,key(x,y,z,1,0));parentSlots[index++]=slot;if(x==0&&y==0&&z==0)cornerSlot=slot;}
 std::vector<unsigned> offsets(capacity,0xffffffffu);unsigned entries=0;
 for(unsigned i=0;i<capacity;++i)if(keys[i]){offsets[i]=entries;const unsigned level=((keys[i]-1)>>24)&3;entries+=32u<<(2*level);}
 std::vector<V> local(entries,V{0,0,0,-1}),parent(entries,V{0,0,0,0});
 local[offsets[child]+0]={.1F,.2F,.3F,.5F};local[offsets[child]+1]={.2F,.3F,.4F,0};local[offsets[child]+2]={0,0,0,1};
 local[offsets[far]]={0,0,0,1};local[offsets[other]]={0,0,0,1};local[offsets[top]]={.25F,.5F,.75F,0};local[offsets[top]+1]={0,0,0,1};
 index=0;for(int z=-1;z<=0;++z)for(int y=-1;y<=0;++y)for(int x=-1;x<=0;++x){const auto slot=parentSlots[index++];for(unsigned j=0;j<4;++j)parent[offsets[slot]+j]={float(x+1)+.1F*j,float(y+1),float(z+1),1};}
 const std::vector<Q> query{{child,0},{child,1},{child,2},{far,0},{other,0},{top,0},{top,1},{capacity,0},{child,32}};
 const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 std::vector<unsigned> probeCounts(capacity,1);const auto weightBuffer=make(capacity*4);check(weightBuffer&&d.write_buffer(weightBuffer,0,std::as_bytes(std::span(probeCounts)),&error),"probe weights");
 const auto a=make(keys.size()*4),b=make(local.size()*sizeof(V)),c=make(parent.size()*sizeof(V)),e=make(query.size()*sizeof(Q)),f=make(query.size()*sizeof(V)),offsetBuffer=make(capacity*4),configBuffer=make(4),diagBuffer=make(12);check(a&&b&&c&&e&&f&&offsetBuffer&&configBuffer&&diagBuffer,"buffers");
 check(d.write_buffer(a,0,std::as_bytes(std::span(keys)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(local)),&error)&&d.write_buffer(c,0,std::as_bytes(std::span(parent)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(query)),&error)&&d.write_buffer(configBuffer,0,std::as_bytes(std::span(&entries,1)),&error),"upload");
 BindGroupLayoutDesc compactLd;compactLd.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{3,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto compactLayout=d.create_bind_group_layout(compactLd,&error);
 BindGroupDesc compactGd;compactGd.layout=compactLayout;compactGd.entries={{0,a,{},0,keys.size()*4,{}},{1,configBuffer,{},0,4,{}},{2,offsetBuffer,{},0,capacity*4,{}},{3,diagBuffer,{},0,12,{}}};const auto compactGroup=d.create_bind_group(compactGd,&error);
 ComputePipelineDesc compactPd;compactPd.bytecode=load(argv[2]);compactPd.bindGroupLayouts={compactLayout};const auto compactPipe=d.create_compute_pipeline(compactPd,&error);check(compactLayout&&compactGroup&&compactPipe,"compact pipeline");
 const auto compact=[&](){const auto cmd=d.begin_commands(QueueKind::Compute,"compact split RC probe directions",&error);check(d.bind_compute_bind_group(cmd,0,compactGroup,&error)&&d.dispatch(cmd,compactPipe,1,1,1,&error),"compact dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"compact fence");};
 compact();std::vector<unsigned> gpuOffsets(capacity);std::array<unsigned,3> diag{};check(d.read_buffer(offsetBuffer,0,std::as_writable_bytes(std::span(gpuOffsets)),&error)&&d.read_buffer(diagBuffer,0,std::as_writable_bytes(std::span(diag)),&error),"compact readback");check(gpuOffsets==offsets&&diag==std::array<unsigned,3>{entries,0,0},"compact offsets and capacity");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{5,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{6,BindingType::StorageBufferReadOnly,ShaderStage::Compute}};const auto layout=d.create_bind_group_layout(ld,&error);
 BindGroupDesc gd;gd.layout=layout;gd.entries={{0,a,{},0,keys.size()*4,{}},{1,b,{},0,local.size()*sizeof(V),{}},{2,c,{},0,parent.size()*sizeof(V),{}},{3,e,{},0,query.size()*sizeof(Q),{}},{4,f,{},0,query.size()*sizeof(V),{}},{5,offsetBuffer,{},0,capacity*4,{}},{6,weightBuffer,{},0,capacity*4,{}}};const auto group=d.create_bind_group(gd,&error);
 ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};const auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"pipeline");
 const auto run=[&](){const auto cmd=d.begin_commands(QueueKind::Compute,"split RC spatial merge",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipeline,1,1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");std::vector<V> out(query.size());check(d.read_buffer(f,0,std::as_writable_bytes(std::span(out)),&error),"readback");return out;};
 const auto out=run();check(eq(out[0].x,.55F)&&eq(out[0].y,.575F)&&eq(out[0].z,.675F)&&eq(out[0].w,1),"weighted spatial and angular merge");
 check(eq(out[1].x,.2F)&&eq(out[1].y,.3F)&&eq(out[1].z,.4F)&&eq(out[1].w,1),"opaque interval");
 for(unsigned i:{2u,3u,4u,6u,7u,8u})check(eq(out[i].w,0),"missing or invalid probe must remain unavailable");
 check(eq(out[5].x,.25F)&&eq(out[5].y,.5F)&&eq(out[5].z,.75F)&&eq(out[5].w,1),"top-level terminal interval");
 // Feed the real spatial-merge GPU output through compact scatter and the
 // diffuse gather. Only two of this probe's 32 bins are populated here.
 std::vector<V> zeroAtlas(entries,V{0,0,0,0});std::array<V,32> directions{};
 constexpr double pi=3.14159265358979323846;
 for(unsigned i=0;i<32;++i){const double u=(double(i%8)+.5)/8,v=(double(i/8)+.5)/4,y=1-2*v,r=std::sqrt(1-y*y);directions[i]={float(r*std::cos(2*pi*u)),float(y),float(r*std::sin(2*pi*u)),1};}
 const std::array<GatherQ,2> gatherQueries{{{{child,0,0,0},{0,1,0,0}},{{other,0,0,0},{0,1,0,0}}}};
 const auto atlasBuffer=make(entries*sizeof(V)),dirBuffer=make(sizeof(directions)),gatherQueryBuffer=make(sizeof(gatherQueries)),gatherOut=make(gatherQueries.size()*sizeof(V));check(atlasBuffer&&dirBuffer&&gatherQueryBuffer&&gatherOut,"gather buffers");
 check(d.write_buffer(atlasBuffer,0,std::as_bytes(std::span(zeroAtlas)),&error)&&d.write_buffer(dirBuffer,0,std::as_bytes(std::span(directions)),&error)&&d.write_buffer(gatherQueryBuffer,0,std::as_bytes(std::span(gatherQueries)),&error),"gather upload");
 BindGroupLayoutDesc scatterLd;scatterLd.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto scatterLayout=d.create_bind_group_layout(scatterLd,&error);
 BindGroupDesc scatterGd;scatterGd.layout=scatterLayout;scatterGd.entries={{0,a,{},0,keys.size()*4,{}},{1,offsetBuffer,{},0,capacity*4,{}},{2,e,{},0,query.size()*sizeof(Q),{}},{3,f,{},0,query.size()*sizeof(V),{}},{4,atlasBuffer,{},0,entries*sizeof(V),{}}};const auto scatterGroup=d.create_bind_group(scatterGd,&error);ComputePipelineDesc scatterPd;scatterPd.bytecode=load(argv[3]);scatterPd.bindGroupLayouts={scatterLayout};const auto scatterPipe=d.create_compute_pipeline(scatterPd,&error);
 BindGroupLayoutDesc gatherLd;gatherLd.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{5,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto gatherLayout=d.create_bind_group_layout(gatherLd,&error);
 BindGroupDesc gatherGd;gatherGd.layout=gatherLayout;gatherGd.entries={{0,a,{},0,keys.size()*4,{}},{1,offsetBuffer,{},0,capacity*4,{}},{2,atlasBuffer,{},0,entries*sizeof(V),{}},{3,dirBuffer,{},0,sizeof(directions),{}},{4,gatherQueryBuffer,{},0,sizeof(gatherQueries),{}},{5,gatherOut,{},0,gatherQueries.size()*sizeof(V),{}}};const auto gatherGroup=d.create_bind_group(gatherGd,&error);ComputePipelineDesc gatherPd;gatherPd.bytecode=load(argv[4]);gatherPd.bindGroupLayouts={gatherLayout};const auto gatherPipe=d.create_compute_pipeline(gatherPd,&error);check(scatterLayout&&scatterGroup&&scatterPipe&&gatherLayout&&gatherGroup&&gatherPipe,"scatter and gather pipelines");
 const auto dispatch=[&](ComputePipelineHandle pipe,BindGroupHandle bind){const auto cmd=d.begin_commands(QueueKind::Compute,"split RC scatter/gather",&error);check(d.bind_compute_bind_group(cmd,0,bind,&error)&&d.dispatch(cmd,pipe,1,1,1,&error),"scatter/gather dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"scatter/gather fence");};
 dispatch(scatterPipe,scatterGroup);dispatch(gatherPipe,gatherGroup);std::array<V,2> gathered{};check(d.read_buffer(gatherOut,0,std::as_writable_bytes(std::span(gathered)),&error),"gather readback");
 check(eq(gathered[0].x,.375F)&&eq(gathered[0].y,.4375F)&&eq(gathered[0].z,.5375F)&&eq(gathered[0].w,1),"spatial merge to diffuse gather");check(eq(gathered[1].w,0),"missing probe does not invent light");
 check(d.destroy_compute_pipeline(scatterPipe,&error)&&d.destroy_compute_pipeline(gatherPipe,&error)&&d.destroy_bind_group(scatterGroup,&error)&&d.destroy_bind_group(gatherGroup,&error)&&d.destroy_bind_group_layout(scatterLayout,&error)&&d.destroy_bind_group_layout(gatherLayout,&error),"scatter/gather cleanup");for(auto h:{atlasBuffer,dirBuffer,gatherQueryBuffer,gatherOut})check(d.destroy_buffer(h,&error),"scatter/gather buffer cleanup");
 std::fill(parent.begin(),parent.end(),V{0,0,0,0});parent[offsets[cornerSlot]]={1,1,1,1};check(d.write_buffer(c,0,std::as_bytes(std::span(parent)),&error),"upload sparse parent");const auto partial=run();check(eq(partial[0].x,.6F)&&eq(partial[0].y,.7F)&&eq(partial[0].z,.8F)&&eq(partial[0].w,1),"partial support normalisation");
 // Equal spatial treatment inside each parent: a better-sampled probe has
 // three times the effective weight even when it has fewer available angles.
 std::fill(parent.begin(),parent.end(),V{0,0,0,0});
 for(unsigned j=0;j<4;++j)parent[offsets[cornerSlot]+j]={1,0,0,1};
 const unsigned blueSlot=parentSlots[6];parent[offsets[blueSlot]]={0,0,1,1};
 probeCounts[cornerSlot]=1;probeCounts[blueSlot]=9;
 check(d.write_buffer(c,0,std::as_bytes(std::span(parent)),&error)&&d.write_buffer(weightBuffer,0,std::as_bytes(std::span(probeCounts)),&error),"upload weighted probes");
 const auto weighted=run();check(eq(weighted[0].x,.225F)&&eq(weighted[0].y,.2F)&&eq(weighted[0].z,.675F)&&eq(weighted[0].w,1),"probe confidence controls spatial blend, not angular sample count");
 probeCounts[blueSlot]=0;check(d.write_buffer(weightBuffer,0,std::as_bytes(std::span(probeCounts)),&error),"zero probe weight");
 const auto ignored=run();check(eq(ignored[0].x,.6F)&&eq(ignored[0].z,.3F),"zero-count probe is ignored");
 // Capacity exhaustion must never provide an out-of-range slice. A subsequent
 // merge also verifies the missing allocation remains unavailable.
 unsigned tight=32;check(d.write_buffer(configBuffer,0,std::as_bytes(std::span(&tight,1)),&error),"tight capacity");compact();
 check(d.read_buffer(offsetBuffer,0,std::as_writable_bytes(std::span(gpuOffsets)),&error)&&d.read_buffer(diagBuffer,0,std::as_writable_bytes(std::span(diag)),&error),"tight readback");
 check(diag[0]<=tight&&diag[1]>0&&diag[2]==0,"tight allocation diagnostics");for(unsigned i=0;i<capacity;++i)if(gpuOffsets[i]!=0xffffffffu){const unsigned size=32u<<(2*(((keys[i]-1)>>24)&3));check(gpuOffsets[i]+size<=tight,"allocation stays in bounds");}
 const auto limited=run();check(eq(limited[0].w,0),"missing parent allocation remains unavailable");
 check(d.destroy_compute_pipeline(compactPipe,&error)&&d.destroy_bind_group(compactGroup,&error)&&d.destroy_bind_group_layout(compactLayout,&error)&&d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error),"pipeline cleanup");for(auto h:{a,b,c,e,f,offsetBuffer,configBuffer,diagBuffer,weightBuffer})check(d.destroy_buffer(h,&error),"buffer cleanup");
 if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<"Sparse spatial/angular merge, negative coordinates, missing support and sheet isolation passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
