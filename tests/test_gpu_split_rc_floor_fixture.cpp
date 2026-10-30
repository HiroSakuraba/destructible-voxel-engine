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
struct V{float x,y,z,w;};struct TraceRay{V origin,direction,sky;};struct Traced{V hit,sky;};struct Q{unsigned slot,dir;};struct GQ{std::array<unsigned,4> slot;V normal;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
unsigned hash(unsigned k){k^=k>>16;k*=0x7feb352dU;k^=k>>15;k*=0x846ca68bU;return k^(k>>16);}
unsigned key(unsigned x,unsigned y,unsigned z,unsigned level){return ((x+128u)|((y+128u)<<8)|((z+128u)<<16)|(level<<24))+1u;}
unsigned insert(std::vector<unsigned>& keys,unsigned k){unsigned start=hash(k)%unsigned(keys.size());for(unsigned j=0;j<keys.size();++j){unsigned i=(start+j)%unsigned(keys.size());if(!keys[i]||keys[i]==k){keys[i]=k;return i;}}throw std::runtime_error("table full");}
}
int main(int argc,char** argv){try{
 if(argc<10||argc>11)throw std::runtime_error("pass compact, queries, merge, scatter, gather, voxel, split, accumulation, resolve shaders and optional PPM");
 VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC hierarchy: Vulkan unavailable");}
 // 16x16 world-space floor points; each direction is traced from its own
 // position. Shared coarse probes are deduplicated by world cell and level.
 constexpr unsigned side=16, pointCount=side*side, raysPerPoint=32, rayCount=pointCount*raysPerPoint, sampleCount=rayCount*4, capacity=512;
 std::vector<unsigned> keys(capacity),offsets(capacity,0xffffffffu),pointSlots(pointCount*4);
 for(unsigned z=0;z<side;++z)for(unsigned x=0;x<side;++x)for(unsigned l=0;l<4;++l){const unsigned p=z*side+x;pointSlots[p*4+l]=insert(keys,key(x>>l,1u>>l,z>>l,l));}
 unsigned entries=0;for(unsigned i=0;i<capacity;++i)if(keys[i]){offsets[i]=entries;entries+=32u<<(2*((keys[i]-1)>>24));}check(entries>0,"fixture allocation");
 std::vector<V> zero(entries,V{0,0,0,0});std::array<V,32> dirs{};std::vector<TraceRay> rays(rayCount);std::vector<unsigned> raySlots(sampleCount),rayDirections(sampleCount);
 constexpr double pi=3.14159265358979323846;for(unsigned j=0;j<32;++j){double u=(double(j%8)+.5)/8,v=(double(j/8)+.5)/4,y=1-2*v,r=std::sqrt(1-y*y);dirs[j]={float(r*std::cos(2*pi*u)),float(y),float(r*std::sin(2*pi*u)),1};}
 for(unsigned z=0;z<side;++z)for(unsigned x=0;x<side;++x){const unsigned point=z*side+x;for(unsigned j=0;j<32;++j){const unsigned ray=point*32+j;rays[ray]={{float(x)+.5F,1.5F,float(z)+.5F,.5F},{dirs[j].x,dirs[j].y,dirs[j].z,32},{.05F,.08F,.16F,0}};for(unsigned l=0;l<4;++l){raySlots[ray*4+l]=pointSlots[point*4+l];rayDirections[ray*4+l]=j<<(2*l);}}}
 std::array<V,2> gridInfo{{{0,0,0,1},{16,8,16,0}}},palette{{{1,1,1,0},{.9F,.28F,.05F,0}}};std::vector<unsigned> grid(512);for(unsigned z=4;z<12;++z)for(unsigned x=4;x<12;++x){unsigned cell=(z*8+5)*16+x;grid[cell/4]|=1u<<((cell%4)*8);}
 std::vector<unsigned> zeroSums(entries*6);std::array<unsigned,3> zeroDiagnostics{};std::array<unsigned,4> accumConfig{capacity,entries,0,0};
 constexpr unsigned width=side,height=side;std::vector<GQ> gatherQueries;for(unsigned point=0;point<pointCount;++point)gatherQueries.push_back({{pointSlots[point*4],0,0,0},{0,1,0,0}});
 const auto make=[&](std::size_t n){BufferDesc b;b.bytes=n;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
 const auto kb=make(keys.size()*4),ob=make(capacity*4),limit=make(4),diag=make(12),ib=make(entries*sizeof(V)),ab=make(entries*sizeof(V)),bb=make(entries*sizeof(V)),qb=make(entries*sizeof(Q)),vb=make(entries*sizeof(V)),db=make(sizeof(dirs)),gqb=make(gatherQueries.size()*sizeof(GQ)),gb=make(gatherQueries.size()*sizeof(V)),rayBuffer=make(rays.size()*sizeof(TraceRay)),infoBuffer=make(sizeof(gridInfo)),gridBuffer=make(grid.size()*4),paletteBuffer=make(sizeof(palette)),tracedBuffer=make(rayCount*2*sizeof(V)),splitBuffer=make(sampleCount*sizeof(V)),slotBuffer=make(raySlots.size()*4),dirBuffer=make(rayDirections.size()*4),accConfig=make(sizeof(accumConfig)),sums=make(entries*6*4),accDiag=make(12),valid=make(entries*4);check(kb&&ob&&limit&&diag&&ib&&ab&&bb&&qb&&vb&&db&&gqb&&gb&&rayBuffer&&infoBuffer&&gridBuffer&&paletteBuffer&&tracedBuffer&&splitBuffer&&slotBuffer&&dirBuffer&&accConfig&&sums&&accDiag&&valid,"buffers");
 check(d.write_buffer(kb,0,std::as_bytes(std::span(keys)),&error)&&d.write_buffer(limit,0,std::as_bytes(std::span(&entries,1)),&error)&&d.write_buffer(rayBuffer,0,std::as_bytes(std::span(rays)),&error)&&d.write_buffer(infoBuffer,0,std::as_bytes(std::span(gridInfo)),&error)&&d.write_buffer(gridBuffer,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(paletteBuffer,0,std::as_bytes(std::span(palette)),&error)&&d.write_buffer(slotBuffer,0,std::as_bytes(std::span(raySlots)),&error)&&d.write_buffer(dirBuffer,0,std::as_bytes(std::span(rayDirections)),&error)&&d.write_buffer(accConfig,0,std::as_bytes(std::span(accumConfig)),&error)&&d.write_buffer(sums,0,std::as_bytes(std::span(zeroSums)),&error)&&d.write_buffer(accDiag,0,std::as_bytes(std::span(zeroDiagnostics)),&error)&&d.write_buffer(ab,0,std::as_bytes(std::span(zero)),&error)&&d.write_buffer(bb,0,std::as_bytes(std::span(zero)),&error)&&d.write_buffer(db,0,std::as_bytes(std::span(dirs)),&error)&&d.write_buffer(gqb,0,std::as_bytes(std::span(gatherQueries)),&error),"upload");
 struct Stage{BindGroupLayoutHandle layout;BindGroupHandle group;ComputePipelineHandle pipe;};std::vector<Stage> stages;
 const auto stage=[&](const char* path,std::vector<BindingType> types,std::vector<BindGroupEntry> bindings){BindGroupLayoutDesc ld;for(unsigned i=0;i<types.size();++i)ld.bindings.push_back({i,types[i],ShaderStage::Compute});auto l=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=l;gd.entries=std::move(bindings);auto g=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(path);pd.bindGroupLayouts={l};auto p=d.create_compute_pipeline(pd,&error);check(l&&g&&p,"stage");stages.push_back({l,g,p});return stages.back();};
 const auto R=BindingType::StorageBufferReadOnly,W=BindingType::StorageBufferReadWrite;
 auto compact=stage(argv[1],{R,R,W,W},{{0,kb,{},0,capacity*4,{}},{1,limit,{},0,4,{}},{2,ob,{},0,capacity*4,{}},{3,diag,{},0,12,{}}});
 auto queries=stage(argv[2],{R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,qb,{},0,entries*sizeof(Q),{}}});
 const auto merge=[&](BufferHandle parent){return stage(argv[3],{R,R,R,R,W,R},{{0,kb,{},0,capacity*4,{}},{1,ib,{},0,entries*sizeof(V),{}},{2,parent,{},0,entries*sizeof(V),{}},{3,qb,{},0,entries*sizeof(Q),{}},{4,vb,{},0,entries*sizeof(V),{}},{5,ob,{},0,capacity*4,{}}});};
 const auto scatter=[&](BufferHandle target){return stage(argv[4],{R,R,R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,qb,{},0,entries*sizeof(Q),{}},{3,vb,{},0,entries*sizeof(V),{}},{4,target,{},0,entries*sizeof(V),{}}});};
 auto mergeA=merge(ab),mergeB=merge(bb),scatterA=scatter(ab),scatterB=scatter(bb);
 auto trace=stage(argv[6],{R,R,R,R,W},{{0,rayBuffer,{},0,rays.size()*sizeof(TraceRay),{}},{1,infoBuffer,{},0,sizeof(gridInfo),{}},{2,gridBuffer,{},0,grid.size()*4,{}},{3,paletteBuffer,{},0,sizeof(palette),{}},{4,tracedBuffer,{},0,rayCount*2*sizeof(V),{}}});
 auto split=stage(argv[7],{R,W},{{0,tracedBuffer,{},0,rayCount*2*sizeof(V),{}},{1,splitBuffer,{},0,sampleCount*sizeof(V),{}}});
 auto accumulate=stage(argv[8],{R,R,R,R,W,W,R,R},{{0,splitBuffer,{},0,sampleCount*sizeof(V),{}},{1,slotBuffer,{},0,raySlots.size()*4,{}},{2,dirBuffer,{},0,rayDirections.size()*4,{}},{3,accConfig,{},0,sizeof(accumConfig),{}},{4,sums,{},0,entries*6*4,{}},{5,accDiag,{},0,12,{}},{6,ob,{},0,capacity*4,{}},{7,kb,{},0,capacity*4,{}}});
 auto resolve=stage(argv[9],{R,W,W},{{0,sums,{},0,entries*6*4,{}},{1,ib,{},0,entries*sizeof(V),{}},{2,valid,{},0,entries*4,{}}});
 auto gather=stage(argv[5],{R,R,R,R,R,W},{{0,kb,{},0,capacity*4,{}},{1,ob,{},0,capacity*4,{}},{2,ab,{},0,entries*sizeof(V),{}},{3,db,{},0,sizeof(dirs),{}},{4,gqb,{},0,gatherQueries.size()*sizeof(GQ),{}},{5,gb,{},0,gatherQueries.size()*sizeof(V),{}}});
 const auto dispatch=[&](Stage s,unsigned groups){auto cmd=d.begin_commands(QueueKind::Compute,"split RC hierarchy fixture",&error);check(d.bind_compute_bind_group(cmd,0,s.group,&error)&&d.dispatch(cmd,s.pipe,groups,1,1,&error),"dispatch");auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");};
 dispatch(compact,1);dispatch(queries,(capacity+63)/64);std::vector<unsigned> actualOffsets(capacity);std::vector<Q> actualQueries(entries);std::array<unsigned,3> diagnostics{};check(d.read_buffer(ob,0,std::as_writable_bytes(std::span(actualOffsets)),&error)&&d.read_buffer(qb,0,std::as_writable_bytes(std::span(actualQueries)),&error)&&d.read_buffer(diag,0,std::as_writable_bytes(std::span(diagnostics)),&error),"allocation readback");check(actualOffsets==offsets&&diagnostics==std::array<unsigned,3>{entries,0,0},"compact allocation");
 for(unsigned slot=0;slot<capacity;++slot)if(keys[slot]){unsigned level=((keys[slot]-1)>>24)&3,n=32u<<(2*level);for(unsigned i=0;i<n;++i){Q q=actualQueries[offsets[slot]+i];check(q.slot==slot&&q.dir==i,"generated query");}}
 dispatch(trace,(rayCount+63)/64);dispatch(split,(rayCount+63)/64);dispatch(accumulate,(sampleCount+63)/64);dispatch(resolve,(entries+63)/64);
 std::array<unsigned,3> accStats{};check(d.read_buffer(accDiag,0,std::as_writable_bytes(std::span(accStats)),&error)&&accStats==std::array<unsigned,3>{},"accumulation diagnostics");
 for(unsigned iteration=0;iteration<4;++iteration){dispatch(iteration%2?mergeB:mergeA,(entries+63)/64);dispatch(iteration%2?scatterA:scatterB,(entries+63)/64);}
 dispatch(gather,static_cast<unsigned>((gatherQueries.size()+63)/64));std::vector<V> pixels(gatherQueries.size());check(d.read_buffer(gb,0,std::as_writable_bytes(std::span(pixels)),&error),"gather readback");
 unsigned covered=0;for(const auto p:pixels)if(p.w==1)++covered;check(covered==pointCount,"floor probe coverage");
 const unsigned centre=8*side+8;check(pixels[centre].x>.1F,"voxel emitter lights the floor");
 if(argc==11){std::ofstream ppm(argv[10],std::ios::binary);check(bool(ppm),"open fixture image");ppm<<"P6\n"<<width<<" "<<height<<"\n255\n";for(unsigned i=0;i<pixels.size();++i){const V p=pixels[i];for(float c:{p.w>.5F?p.x:.035F,p.w>.5F?p.y:.045F,p.w>.5F?p.z:.065F}){const float s=std::pow(std::fmax(0.F,std::fmin(1.F,c)),1.F/2.2F);const unsigned char b=static_cast<unsigned char>(std::lround(s*255));ppm.write(reinterpret_cast<const char*>(&b),1);}}}
 // Turn off the emissive voxel plane and re-trace, accumulating from scratch.
 // Misses still see the blue sky; upward hits must no longer carry red light.
 palette[1]={0,0,0,0};check(d.write_buffer(paletteBuffer,0,std::as_bytes(std::span(palette)),&error)&&d.write_buffer(sums,0,std::as_bytes(std::span(zeroSums)),&error)&&d.write_buffer(accDiag,0,std::as_bytes(std::span(zeroDiagnostics)),&error),"update source light");
 dispatch(trace,(rayCount+63)/64);dispatch(split,(rayCount+63)/64);dispatch(accumulate,(sampleCount+63)/64);dispatch(resolve,(entries+63)/64);
 for(unsigned iteration=0;iteration<4;++iteration){dispatch(iteration%2?mergeB:mergeA,(entries+63)/64);dispatch(iteration%2?scatterA:scatterB,(entries+63)/64);}
 dispatch(gather,static_cast<unsigned>((gatherQueries.size()+63)/64));
 std::vector<V> changed(pointCount);check(d.read_buffer(gb,0,std::as_writable_bytes(std::span(changed)),&error),"updated light readback");
 check(changed[centre].w==1&&changed[centre].x<pixels[centre].x-.05F,"emissive voxel change updates floor lighting");
 if(argc==11){std::filesystem::path path(argv[10]);path.replace_filename(path.stem().string()+"-off"+path.extension().string());std::ofstream ppm(path,std::ios::binary);check(bool(ppm),"open light-off image");ppm<<"P6\n"<<width<<" "<<height<<"\n255\n";for(const V p:changed)for(float c:{p.w>.5F?p.x:.035F,p.w>.5F?p.y:.045F,p.w>.5F?p.z:.065F}){const float value=std::pow(std::fmax(0.F,std::fmin(1.F,c)),1.F/2.2F);const unsigned char b=static_cast<unsigned char>(std::lround(value*255));ppm.write(reinterpret_cast<const char*>(&b),1);}}

 // Remove the voxel plane and verify every direction resolves to the sky.
 std::fill(grid.begin(),grid.end(),0u);palette[1]={.8F,.25F,.1F,0};
 check(d.write_buffer(gridBuffer,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(paletteBuffer,0,std::as_bytes(std::span(palette)),&error)&&d.write_buffer(sums,0,std::as_bytes(std::span(zeroSums)),&error)&&d.write_buffer(accDiag,0,std::as_bytes(std::span(zeroDiagnostics)),&error),"remove emitter voxels");
 dispatch(trace,(rayCount+63)/64);dispatch(split,(rayCount+63)/64);dispatch(accumulate,(sampleCount+63)/64);dispatch(resolve,(entries+63)/64);
 for(unsigned iteration=0;iteration<4;++iteration){dispatch(iteration%2?mergeB:mergeA,(entries+63)/64);dispatch(iteration%2?scatterA:scatterB,(entries+63)/64);}
 dispatch(gather,static_cast<unsigned>((gatherQueries.size()+63)/64));
 V removed{};check(d.read_buffer(gb,0,std::as_writable_bytes(std::span(&removed,1)),&error),"removed light readback");
 check(removed.w==1&&std::abs(removed.x-.05F)<.001F&&std::abs(removed.y-.08F)<.001F&&std::abs(removed.z-.16F)<.001F,"removed voxels expose sky without stale light");
 for(auto s:stages)check(d.destroy_compute_pipeline(s.pipe,&error)&&d.destroy_bind_group(s.group,&error)&&d.destroy_bind_group_layout(s.layout,&error),"stage cleanup");for(auto h:{kb,ob,limit,diag,ib,ab,bb,qb,vb,db,gqb,gb,rayBuffer,infoBuffer,gridBuffer,paletteBuffer,tracedBuffer,splitBuffer,slotBuffer,dirBuffer,accConfig,sums,accDiag,valid})check(d.destroy_buffer(h,&error),"buffer cleanup");
 if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
 std::cout<<"256 world probes: voxel trace, four-level merge, and diffuse gather passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
