#include "dve/render/text3d_renderer.hpp"
#include "dve/rhi/null_device.hpp"

#include <cstddef>
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
    std::map<std::string,Bytes> t;
    Bytes head(54,0);set16(head,18,1000);set16(head,50,1);t["head"]=head;
    Bytes maxp;u32(maxp,0x00010000U);u16(maxp,2);t["maxp"]=maxp;
    Bytes hhea(36,0);set32(hhea,0,0x00010000U);set16(hhea,4,800);set16(hhea,6,static_cast<std::uint16_t>(-200));set16(hhea,8,200);set16(hhea,34,2);t["hhea"]=hhea;
    Bytes hmtx;u16(hmtx,500);i16(hmtx,0);u16(hmtx,1100);i16(hmtx,0);t["hmtx"]=hmtx;
    Bytes glyf;i16(glyf,1);i16(glyf,0);i16(glyf,0);i16(glyf,1000);i16(glyf,1000);u16(glyf,2);u16(glyf,0);glyf.insert(glyf.end(),{1,1,1});i16(glyf,0);i16(glyf,500);i16(glyf,500);i16(glyf,0);i16(glyf,1000);i16(glyf,-1000);t["glyf"]=glyf;
    Bytes loca;u32(loca,0);u32(loca,0);u32(loca,static_cast<std::uint32_t>(glyf.size()));t["loca"]=loca;
    Bytes cmap;u16(cmap,0);u16(cmap,1);u16(cmap,3);u16(cmap,1);u32(cmap,12);u16(cmap,4);u16(cmap,32);u16(cmap,0);u16(cmap,4);u16(cmap,4);u16(cmap,1);u16(cmap,0);u16(cmap,65);u16(cmap,0xFFFF);u16(cmap,0);u16(cmap,65);u16(cmap,0xFFFF);i16(cmap,-64);i16(cmap,1);u16(cmap,0);u16(cmap,0);t["cmap"]=cmap;
    const auto count=static_cast<std::uint16_t>(t.size());Bytes out(12U+static_cast<std::size_t>(count)*16U,0);set32(out,0,0x00010000U);set16(out,4,count);std::size_t record=12,cursor=out.size();
    for(const auto&[name,data]:t){while(cursor%4U){out.push_back(0);++cursor;}set32(out,record,tag(name.c_str()));set32(out,record+8,static_cast<std::uint32_t>(cursor));set32(out,record+12,static_cast<std::uint32_t>(data.size()));out.insert(out.end(),data.begin(),data.end());cursor+=data.size();record+=16;}
    return out;
}
std::filesystem::path temp_font(){static int n=0;auto p=std::filesystem::temp_directory_path()/std::filesystem::path("dve_text_gpu_"+std::to_string(++n)+".ttf");const auto b=make_font();std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));return p;}

void run(){
    const auto font=temp_font();
    dve::Text3DCookOptions options;options.objectId=77;options.style.faceMaterialId=11;options.style.sideMaterialId=12;options.style.horizontalBands=4;options.style.verticalBands=4;
    auto cooked=dve::cook_text3d(font,"AA",options);CHECK(cooked);
    dve::rhi::NullDevice device;
    dve::render::Text3DGpuCache cache(device);
    std::string error;
    CHECK(cache.upload(cooked.asset,&error));
    const auto* gpu=cache.find(77);CHECK(gpu);CHECK(gpu->glyphCount==2U);CHECK(gpu->curveHeight>0U);CHECK(gpu->bandHeight>0U);CHECK(gpu->residentBytes>0U);
    CHECK(cache.upload(cooked.asset,&error));CHECK(cache.stats().unchanged==1U);

    std::vector<std::byte> curve(static_cast<std::size_t>(gpu->curveWidth)*gpu->curveHeight*8U);
    CHECK(device.read_texture(gpu->curveTexture,0U,0U,curve,static_cast<std::size_t>(gpu->curveWidth)*8U,&error));
    std::vector<std::byte> band(static_cast<std::size_t>(gpu->bandWidth)*gpu->bandHeight*4U);
    CHECK(device.read_texture(gpu->bandTexture,0U,0U,band,static_cast<std::size_t>(gpu->bandWidth)*4U,&error));

    const std::vector<dve::render::Text3DRenderInstance> instances{{900,77,{},true,true,true,true}};
    const std::vector<dve::CookedText3DAsset> assets{cooked.asset};
    const auto plan=dve::render::make_text3d_frame_plan(instances,cache,assets);
    CHECK(plan.validate(&error));CHECK(plan.faceDraws==4U);CHECK(plan.sideDraws==1U);CHECK(plan.selectionDraws==1U);CHECK(plan.missingAssets==0U);
    CHECK(plan.packets.front().pass==dve::render::Text3DPassKind::BackFaces);
    CHECK(plan.packets[2].pass==dve::render::Text3DPassKind::ExtrusionSides);
    CHECK(plan.packets[3].pass==dve::render::Text3DPassKind::FrontFaces);
    CHECK(plan.packets.front().materialId==11U);CHECK(plan.packets[2].materialId==12U);

    dve::rhi::TextureDesc colorDesc; colorDesc.width=64;colorDesc.height=64;colorDesc.usage=dve::rhi::TextureUsage::RenderTarget;colorDesc.initialState=dve::rhi::ResourceState::RenderTarget;
    const auto color=device.create_texture(colorDesc,&error);CHECK(color);
    dve::rhi::TextureDesc depthDesc=colorDesc;depthDesc.format=dve::rhi::TextureFormat::D32Float;depthDesc.usage=dve::rhi::TextureUsage::DepthStencil;depthDesc.initialState=dve::rhi::ResourceState::DepthWrite;
    const auto depth=device.create_texture(depthDesc,&error);CHECK(depth);
    dve::rhi::BindGroupLayoutDesc constantsLayoutDesc;
    constantsLayoutDesc.debugName="text constants layout";
    constantsLayoutDesc.bindings={{0U,dve::rhi::BindingType::UniformBuffer,dve::rhi::ShaderStage::Vertex}};
    const auto constantsLayout=device.create_bind_group_layout(constantsLayoutDesc,&error);CHECK(constantsLayout);
    dve::rhi::GraphicsPipelineDesc facePipelineDesc;facePipelineDesc.vertexBytecode={std::byte{1}};facePipelineDesc.fragmentBytecode={std::byte{2}};facePipelineDesc.bindGroupLayouts={constantsLayout,gpu->atlasLayout};
    const auto facePipeline=device.create_graphics_pipeline(facePipelineDesc,&error);CHECK(facePipeline);
    dve::rhi::GraphicsPipelineDesc sidePipelineDesc;sidePipelineDesc.vertexBytecode={std::byte{1}};sidePipelineDesc.fragmentBytecode={std::byte{2}};
    const auto sidePipeline=device.create_graphics_pipeline(sidePipelineDesc,&error);CHECK(sidePipeline);
    const auto commands=device.begin_commands(dve::rhi::QueueKind::Graphics,"text test",&error);CHECK(commands);
    dve::rhi::RenderPassDesc pass;pass.colors.push_back({color,true,0,0,0,1});pass.depth=dve::rhi::RenderPassDepthAttachment{depth,true,1};CHECK(device.begin_render_pass(commands,pass,&error));CHECK(device.set_viewport(commands,{0,0,64,64,0,1},&error));CHECK(device.set_scissor(commands,{0,0,64,64},&error));
    const dve::render::Text3DRenderPipelines pipelines{facePipeline,sidePipeline,facePipeline,{}};
    CHECK(dve::render::record_text3d_frame(device,commands,pipelines,plan,{},&error));
    CHECK(device.end_render_pass(commands,&error));CHECK(device.submit(commands,&error));
    CHECK(device.statistics().indexedDrawsExecuted==5U);

    auto changed=cooked.asset;changed.style.faceColor.x=0.25F;changed.contentHash=dve::text3d_content_hash(changed);CHECK(cache.upload(changed,&error));CHECK(cache.stats().replacements==1U);
    CHECK(cache.remove(77,&error));CHECK(cache.find(77)==nullptr);
    CHECK(device.destroy_graphics_pipeline(facePipeline,&error));CHECK(device.destroy_graphics_pipeline(sidePipeline,&error));CHECK(device.destroy_bind_group_layout(constantsLayout,&error));CHECK(device.destroy_texture(depth,&error));CHECK(device.destroy_texture(color,&error));
    std::filesystem::remove(font);
}
}

int main(){try{run();std::cout<<"text3d render tests: PASS\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
