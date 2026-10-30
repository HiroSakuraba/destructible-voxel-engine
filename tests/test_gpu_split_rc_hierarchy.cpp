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
struct V{float x,y,z,w;};struct Q{unsigned slot,dir;};struct GQ{std::array<unsigned,4> slot;V normal;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
unsigned hash(unsigned k){k^=k>>16;k*=0x7feb352dU;k^=k>>15;k*=0x846ca68bU;return k^(k>>16);}
unsigned key(unsigned level){return (128u|(128u<<8)|(128u<<16)|(level<<24))+1u;}
unsigned insert(std::vector<unsigned>& keys,unsigned k){unsigned start=hash(k)%unsigned(keys.size());for(unsigned j=0;j<keys.size();++j){unsigned i=(start+j)%unsigned(keys.size());if(!keys[i]){keys[i]=k;return i;}}throw std::runtime_error("table full");}
}
int main(int argc,char** argv){try{
 if(argc<6||argc>7)throw std::runtime_error("pass compact, queries, merge, scatter, gather SPIR-V paths and optional PPM");
 VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC hierarchy: Vulkan unavailable");}
 constexpr unsigned capacity=32, entries=32+128+512+2048;std::vector<unsigned> keys(capacity),offsets(capacity,0xffffffffu);std::array<unsigned,4> slots{};for(unsigned l=0;l<4;++l)slots[l]=insert(keys,key(l));unsigned total=0;for(unsigned i=0;i<capacity;++i)if(keys[i]){offsets[i]=total;total+=32u<<(2*((keys[i]-1)>>24));}check(total==entries,"fixture allocation");
 std::vector<V> intervals(entries,V{0,0,0,-1}),zero(entries,V{0,0,0,0});std::array<V,32> dirs{};
 constexpr double pi=3.14159265358979323846;for(unsigned j=0;j<32;++j){double u=(double(j%8)+.5)/8,v=(double(j/8)+.5)/4,y=1-2*v,r=std::sqrt(1-y*y);dirs[j]={float(r*std::cos(2*pi*u)),float(y),float(r*std::sin(2*pi*u)),1};intervals[offsets[slots[0]]+j]={.05F,.05F,.05F,1};intervals[offsets[slots[1]]+j*4]={0,0,0,1};intervals[offsets[slots[2]]+j*16]={0,0,0,1};intervals[offsets[slots[3]]+j*64]=y>0?V{.8F,.25F,.1F,0}:V{.1F,.3F,.8F,0};}
 constexpr unsigned width=128,height=128;std::vector<GQ> gatherQueries;gatherQueries.push_back({{slots[0],0,0,0},{0,1,0,0}});
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){float nx=(2*(float(x)+.5F)/width-1)*1.08F,ny=(1-2*(float(y)+.5F)/height)*1.08F;float r2=nx*nx+ny*ny;gatherQueries.push_back({{slots[0],0,0,0},r2<1?V{nx,ny,std::sqrt(1-r2),0}:V{0,0,0,0}});}
 const auto make=[&](std::size_t n){BufferDesc b;b.bytes=n;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 const auto kb=make(keys.size()*4),ob=make(capacity*4),limit=make(4),diag=make(12),ib=make(entries*sizeof(V)),ab=make(entries*sizeof(V)),bb=make(entries*sizeof(V)),qb=make(entries*sizeof(Q)),vb=make(entries*sizeof(V)),db=make(sizeof(dirs)),gqb=make(gatherQueries.size()*sizeof(GQ)),gb=make(gatherQueries.size()*sizeof(V));check(kb&&ob&&limit&&diag&&ib&&ab&&bb&&qb&&vb&&db&&gqb&&gb,"buffers");
 check(d.write_buffer(kb,0,std::as_bytes(std::span(keys)),&error)&&d.write_buffer(limit,0,std::as_bytes(std::span(&entries,1)),&error)&&d.write_buffer(ib,0,std::as_bytes(std::span(intervals)),&error)&&d.write_buffer(ab,0,std::as_bytes(std::span(zero)),&error)&&d.write_buffer(bb,0,std::as_bytes(std::span(zero)),&error)&&d.write_buffer(db,0,std::as_bytes(std::span(dirs)),&error)&&d.write_buffer(gqb,0,std::as_bytes(std::span(gatherQueries)),&error),"upload");
 struct Stage{BindGroupLayoutHandle layout;BindGroupHandle group;ComputePipelineHandle pipe;};std::vector<Stage> stages;
 const auto stage=[&](const char* path,std::vector<BindingType> types,std::vector<BindGroupEntry> bindings){BindGroupLayoutDesc ld;for(unsigned i=0;i<types.size();++i)ld.bindings.push_back({i,types[i],ShaderStage::Compute});auto l=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=l;gd.entries=std::move(bindings);auto g=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(path);pd.bindGroupLayouts={l};auto p=d.create_compute_pipeline(pd,&error);check(l&&g&&p,"stage");stages.push_back({l,g,p});return stages.back();};
 const auto R=BindingType::StorageBufferReadOnly,W=BindingType::StorageBufferReadWrite;
 auto compact=stage(argv[1],{R,R,W,W},{{0,kb,{},0,capacity*4,{}},{1,limit,{},0,4,{}},{2,ob,{},0,capacity*4,{}},{3,diag,{},0,12,{}}});
 auto queries=stage(argv[2],{R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,qb,{},0,entries*sizeof(Q),{}}});
 const auto merge=[&](BufferHandle parent){return stage(argv[3],{R,R,R,R,W,R},{{0,kb,{},0,capacity*4,{}},{1,ib,{},0,entries*sizeof(V),{}},{2,parent,{},0,entries*sizeof(V),{}},{3,qb,{},0,entries*sizeof(Q),{}},{4,vb,{},0,entries*sizeof(V),{}},{5,ob,{},0,capacity*4,{}}});};
 const auto scatter=[&](BufferHandle target){return stage(argv[4],{R,R,R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,qb,{},0,entries*sizeof(Q),{}},{3,vb,{},0,entries*sizeof(V),{}},{4,target,{},0,entries*sizeof(V),{}}});};
 auto mergeA=merge(ab),mergeB=merge(bb),scatterA=scatter(ab),scatterB=scatter(bb);
 auto gather=stage(argv[5],{R,R,R,R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,ab,{},0,entries*sizeof(V),{}},{3,db,{},0,sizeof(dirs),{}},{4,gqb,{},0,gatherQueries.size()*sizeof(GQ),{}},{5,gb,{},0,gatherQueries.size()*sizeof(V),{}}});
 const auto dispatch=[&](Stage s,unsigned groups){auto cmd=d.begin_commands(QueueKind::Compute,"split RC hierarchy fixture",&error);check(d.bind_compute_bind_group(cmd,0,s.group,&error)&&d.dispatch(cmd,s.pipe,groups,1,1,&error),"dispatch");auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");};
 dispatch(compact,1);dispatch(queries,(capacity+63)/64);std::vector<unsigned> actualOffsets(capacity);std::vector<Q> actualQueries(entries);std::array<unsigned,3> diagnostics{};check(d.read_buffer(ob,0,std::as_writable_bytes(std::span(actualOffsets)),&error)&&d.read_buffer(qb,0,std::as_writable_bytes(std::span(actualQueries)),&error)&&d.read_buffer(diag,0,std::as_writable_bytes(std::span(diagnostics)),&error),"allocation readback");check(actualOffsets==offsets&&diagnostics==std::array<unsigned,3>{entries,0,0},"compact allocation");
 for(unsigned level=0;level<4;++level){unsigned n=32u<<(2*level);for(unsigned i=0;i<n;++i){Q q=actualQueries[offsets[slots[level]]+i];check(q.slot==slots[level]&&q.dir==i,"generated query");}}
 for(unsigned iteration=0;iteration<4;++iteration){dispatch(iteration%2?mergeB:mergeA,(entries+63)/64);dispatch(iteration%2?scatterA:scatterB,(entries+63)/64);}
 std::vector<V> atlas(entries);check(d.read_buffer(ab,0,std::as_writable_bytes(std::span(atlas)),&error),"atlas readback");const V first=atlas[offsets[slots[0]]];check(std::abs(first.x-.85F)<1e-5F&&std::abs(first.y-.30F)<1e-5F&&std::abs(first.z-.15F)<1e-5F&&first.w==1,"four-level radiance composition");
 dispatch(gather,static_cast<unsigned>((gatherQueries.size()+63)/64));std::vector<V> pixels(gatherQueries.size());check(d.read_buffer(gb,0,std::as_writable_bytes(std::span(pixels)),&error),"gather readback");check(pixels[0].w==1&&pixels[0].x>.5F,"gathered light");
 if(argc==7){std::ofstream ppm(argv[6],std::ios::binary);check(bool(ppm),"open fixture image");ppm<<"P6\n"<<width<<" "<<height<<"\n255\n";for(unsigned i=1;i<pixels.size();++i){const V p=pixels[i];for(float c:{p.w>.5F?p.x:.035F,p.w>.5F?p.y:.045F,p.w>.5F?p.z:.065F}){const float s=std::pow(std::fmax(0.F,std::fmin(1.F,c)),1.F/2.2F);const unsigned char b=static_cast<unsigned char>(std::lround(s*255));ppm.write(reinterpret_cast<const char*>(&b),1);}}}
 // Change the source lighting and rerun without keeping the previous atlas.
 // Four iterations must replace stale parent values rather than preserve them.
 for(unsigned j=0;j<32;++j)intervals[offsets[slots[3]]+j*64]={0,0,0,0};
 check(d.write_buffer(ib,0,std::as_bytes(std::span(intervals)),&error),"update source light");
 for(unsigned iteration=0;iteration<4;++iteration){dispatch(iteration%2?mergeB:mergeA,(entries+63)/64);dispatch(iteration%2?scatterA:scatterB,(entries+63)/64);}
 dispatch(gather,static_cast<unsigned>((gatherQueries.size()+63)/64));
 V changed{};check(d.read_buffer(gb,0,std::as_writable_bytes(std::span(&changed,1)),&error),"updated light readback");
 check(changed.w==1&&std::abs(changed.x-.05F)<1e-5F&&std::abs(changed.y-.05F)<1e-5F&&std::abs(changed.z-.05F)<1e-5F,"changed source replaces stale light");
 for(auto s:stages)check(d.destroy_compute_pipeline(s.pipe,&error)&&d.destroy_bind_group(s.group,&error)&&d.destroy_bind_group_layout(s.layout,&error),"stage cleanup");for(auto h:{kb,ob,limit,diag,ib,ab,bb,qb,vb,db,gqb,gb})check(d.destroy_buffer(h,&error),"buffer cleanup");
 if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<"GPU-generated queries and four-level merge through diffuse gather passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
