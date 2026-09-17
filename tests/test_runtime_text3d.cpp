#include "dve/render/text3d_renderer.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/runtime_text3d_world.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while (false)

namespace {
using Bytes = std::vector<std::uint8_t>;
void u16(Bytes& b, std::uint16_t v){b.push_back(static_cast<std::uint8_t>(v>>8U));b.push_back(static_cast<std::uint8_t>(v));}
void i16(Bytes& b, std::int16_t v){u16(b,static_cast<std::uint16_t>(v));}
void u32(Bytes& b, std::uint32_t v){b.push_back(static_cast<std::uint8_t>(v>>24U));b.push_back(static_cast<std::uint8_t>(v>>16U));b.push_back(static_cast<std::uint8_t>(v>>8U));b.push_back(static_cast<std::uint8_t>(v));}
void set16(Bytes& b,std::size_t o,std::uint16_t v){b[o]=static_cast<std::uint8_t>(v>>8U);b[o+1]=static_cast<std::uint8_t>(v);}
void set32(Bytes& b,std::size_t o,std::uint32_t v){b[o]=static_cast<std::uint8_t>(v>>24U);b[o+1]=static_cast<std::uint8_t>(v>>16U);b[o+2]=static_cast<std::uint8_t>(v>>8U);b[o+3]=static_cast<std::uint8_t>(v);}
std::uint32_t tag(const char* s){return(static_cast<std::uint32_t>(s[0])<<24U)|(static_cast<std::uint32_t>(s[1])<<16U)|(static_cast<std::uint32_t>(s[2])<<8U)|static_cast<std::uint32_t>(s[3]);}
Bytes make_font(){
    std::map<std::string,Bytes> tables;
    Bytes head(54,0);set16(head,18,1000);set16(head,50,1);tables["head"]=head;
    Bytes maxp;u32(maxp,0x00010000U);u16(maxp,2);tables["maxp"]=maxp;
    Bytes hhea(36,0);set32(hhea,0,0x00010000U);set16(hhea,4,800);set16(hhea,6,static_cast<std::uint16_t>(-200));set16(hhea,8,200);set16(hhea,34,2);tables["hhea"]=hhea;
    Bytes hmtx;u16(hmtx,500);i16(hmtx,0);u16(hmtx,1100);i16(hmtx,0);tables["hmtx"]=hmtx;
    Bytes glyf;i16(glyf,1);i16(glyf,0);i16(glyf,0);i16(glyf,1000);i16(glyf,1000);u16(glyf,2);u16(glyf,0);glyf.insert(glyf.end(),{1,1,1});i16(glyf,0);i16(glyf,500);i16(glyf,500);i16(glyf,0);i16(glyf,1000);i16(glyf,-1000);tables["glyf"]=glyf;
    Bytes loca;u32(loca,0);u32(loca,0);u32(loca,static_cast<std::uint32_t>(glyf.size()));tables["loca"]=loca;
    Bytes cmap;u16(cmap,0);u16(cmap,1);u16(cmap,3);u16(cmap,1);u32(cmap,12);u16(cmap,4);u16(cmap,32);u16(cmap,0);u16(cmap,4);u16(cmap,4);u16(cmap,1);u16(cmap,0);u16(cmap,65);u16(cmap,0xFFFF);u16(cmap,0);u16(cmap,65);u16(cmap,0xFFFF);i16(cmap,-64);i16(cmap,1);u16(cmap,0);u16(cmap,0);tables["cmap"]=cmap;
    const auto count=static_cast<std::uint16_t>(tables.size());Bytes out(12U+static_cast<std::size_t>(count)*16U,0);set32(out,0,0x00010000U);set16(out,4,count);std::size_t record=12,cursor=out.size();
    for(const auto&[name,data]:tables){while(cursor%4U){out.push_back(0);++cursor;}set32(out,record,tag(name.c_str()));set32(out,record+8,static_cast<std::uint32_t>(cursor));set32(out,record+12,static_cast<std::uint32_t>(data.size()));out.insert(out.end(),data.begin(),data.end());cursor+=data.size();record+=16;}
    return out;
}

std::filesystem::path temp_font(){
    static int sequence=0;
    const auto path=std::filesystem::temp_directory_path()/("dve_runtime_text3d_"+std::to_string(++sequence)+".ttf");
    const auto bytes=make_font();std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));return path;
}

void run(){
    const auto font=temp_font();
    dve::Text3DCookOptions options;options.objectId=1;options.style.horizontalBands=4;options.style.verticalBands=4;
    const auto cooked=dve::cook_text3d(font,"AAA",options);CHECK(cooked);

    dve::ReferenceRuntimeText3DWorld world;
    dve::RuntimeText3DCreateDesc create;create.objectId=500;create.asset=&cooked.asset;create.selected=true;create.worldTransform.position={1.0F,2.0F,3.0F};
    const auto handle=world.create_object(create);CHECK(handle!=dve::kInvalidRuntimeText3DHandle);
    CHECK(world.create_object(create)==dve::kInvalidRuntimeText3DHandle);
    CHECK(world.counts().objects==1U);CHECK(world.counts().glyphs==3U);CHECK(world.counts().residentBytes>0U);
    CHECK(world.object_id(handle).value()==500U);CHECK(world.readback_hash(handle).has_value());
    CHECK(world.asset(handle)!=nullptr);CHECK(world.asset(handle)->objectId==500U);CHECK(world.asset(handle)->sideMesh.objectId==500U);

    auto update=create;update.objectId=501;update.visible=false;update.selected=false;update.worldTransform.position.x=7.0F;
    CHECK(world.update_object(handle,update));CHECK(world.object_id(handle).value()==501U);CHECK(world.world_transform(handle)->position.x==7.0F);
    auto snapshot=world.snapshot();CHECK(snapshot.assets.size()==1U);CHECK(snapshot.instances.size()==1U);CHECK(!snapshot.instances.front().visible);

    dve::rhi::NullDevice device;dve::render::Text3DGpuCache cache(device);std::string error;
    CHECK(cache.upload(snapshot.assets.front(),&error));
    auto hiddenPlan=dve::render::make_text3d_frame_plan(snapshot,cache);CHECK(hiddenPlan.submittedInstances==0U);CHECK(hiddenPlan.packets.empty());

    update.visible=true;update.selected=true;CHECK(world.update_object(handle,update));snapshot=world.snapshot();
    CHECK(cache.upload(snapshot.assets.front(),&error));
    const auto plan=dve::render::make_text3d_frame_plan(snapshot,cache);CHECK(plan.validate(&error));CHECK(plan.submittedInstances==1U);CHECK(plan.missingAssets==0U);CHECK(plan.faceDraws==6U);CHECK(plan.sideDraws==1U);CHECK(plan.selectionDraws==1U);

    dve::ReferenceRuntimeText3DWorld transactional;
    dve::RuntimeText3DCreateDesc good=create;good.objectId=800;
    dve::RuntimeText3DCreateDesc bad=create;bad.objectId=0;
    const std::vector<dve::RuntimeText3DCreateDesc> batch{good,bad};
    CHECK(transactional.create_objects(batch).empty());CHECK(transactional.counts().objects==0U);

    CHECK(world.destroy_object(handle));CHECK(!world.destroy_object(handle));CHECK(world.counts().objects==0U);
    const auto reused=world.create_object(create);CHECK(reused==handle);
    std::filesystem::remove(font);
}
}

int main(){try{run();std::cout<<"runtime text3d tests: PASS\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
