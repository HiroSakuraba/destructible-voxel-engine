#include "dve/rhi/null_device.hpp"
#ifndef DVE_DEPTH_ONLY_NULL_ONLY
#include "dve/rhi/vulkan_device.hpp"
#endif

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve::rhi;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void emit(std::vector<std::uint32_t>& words, std::uint16_t op,
          std::initializer_list<std::uint32_t> args) {
    words.push_back((static_cast<std::uint32_t>(args.size() + 1U) << 16U) | op);
    words.insert(words.end(), args.begin(), args.end());
}
std::vector<std::uint32_t> encode(const char* text) {
    const std::size_t bytes = std::strlen(text) + 1U;
    std::vector<std::uint32_t> words((bytes + 3U) / 4U);
    std::memcpy(words.data(), text, bytes);
    return words;
}
void entry(std::vector<std::uint32_t>& words, std::uint32_t model, std::uint32_t function,
           const char* name, std::initializer_list<std::uint32_t> interfaces) {
    const auto encoded = encode(name);
    words.push_back(((3U + static_cast<std::uint32_t>(encoded.size()) +
                      static_cast<std::uint32_t>(interfaces.size())) << 16U) | 15U);
    words.push_back(model); words.push_back(function);
    words.insert(words.end(), encoded.begin(), encoded.end());
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}
std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> output(words.size() * sizeof(std::uint32_t));
    std::memcpy(output.data(), words.data(), output.size());
    return output;
}
std::vector<std::byte> vertex_shader() {
    std::vector<std::uint32_t> w{0x07230203U,0x00010000U,0U,37U,0U};
    emit(w,17,{1}); emit(w,14,{0,1}); entry(w,0,26,"main",{24,25});
    emit(w,71,{13,2}); emit(w,72,{13,0,11,0}); emit(w,71,{24,11,42});
    emit(w,19,{1}); emit(w,33,{2,1}); emit(w,22,{3,32}); emit(w,23,{4,3,2});
    emit(w,23,{5,3,4}); emit(w,21,{6,32,0}); emit(w,21,{7,32,1});
    emit(w,43,{6,8,3}); emit(w,28,{9,4,8}); emit(w,32,{10,6,9});
    emit(w,32,{11,6,4}); emit(w,30,{13,5}); emit(w,32,{12,3,13});
    emit(w,32,{14,3,5}); emit(w,32,{36,1,7});
    emit(w,43,{3,15,std::bit_cast<std::uint32_t>(-0.8F)});
    emit(w,43,{3,16,std::bit_cast<std::uint32_t>(0.8F)});
    emit(w,43,{3,17,std::bit_cast<std::uint32_t>(0.25F)});
    emit(w,43,{3,18,std::bit_cast<std::uint32_t>(1.0F)});
    emit(w,44,{4,19,15,15}); emit(w,44,{4,20,16,15}); emit(w,44,{4,21,17,16});
    emit(w,44,{9,22,19,20,21}); emit(w,43,{7,35,0}); emit(w,59,{10,23,6,22});
    emit(w,59,{36,24,1}); emit(w,59,{12,25,3}); emit(w,54,{1,26,0,2});
    emit(w,248,{27}); emit(w,61,{7,28,24}); emit(w,65,{11,29,23,28});
    emit(w,61,{4,30,29}); emit(w,81,{3,31,30,0}); emit(w,81,{3,32,30,1});
    emit(w,80,{5,33,31,32,17,18}); emit(w,65,{14,34,25,35}); emit(w,62,{34,33});
    emit(w,253,{}); emit(w,56,{}); return as_bytes(w);
}


template <typename Device>
void verify_region_clear(Device& device) {
    std::string error;
    TextureDesc desc; desc.format=TextureFormat::D32Float; desc.width=8U; desc.height=4U;
    desc.usage=TextureUsage::DepthStencil|TextureUsage::CopySource;
    desc.initialState=ResourceState::DepthWrite; desc.debugName="depth-region clear fixture";
    const auto texture=device.create_texture(desc,&error); require(static_cast<bool>(texture),error.c_str());
    auto commands=device.begin_commands(QueueKind::Graphics,"initialize depth region",&error);
    require(static_cast<bool>(commands),error.c_str());
    RenderPassDesc initialize; initialize.depth=RenderPassDepthAttachment{texture,true,0.25F};
    require(device.begin_render_pass(commands,initialize,&error),error.c_str());
    require(device.end_render_pass(commands,&error),error.c_str());
    auto fence=device.submit(commands,&error); require(fence&&device.fence_complete(fence),error.c_str());
    commands=device.begin_commands(QueueKind::Graphics,"clear depth region",&error);
    require(static_cast<bool>(commands),error.c_str());
    RenderPassDesc partial; partial.depth=RenderPassDepthAttachment{texture,false,1.0F};
    require(device.begin_render_pass(commands,partial,&error),error.c_str());
    require(device.clear_depth_region(commands,1.0F,{2,1,3,2},&error),error.c_str());
    require(device.end_render_pass(commands,&error),error.c_str());
    fence=device.submit(commands,&error); require(fence&&device.fence_complete(fence),error.c_str());
    std::vector<std::byte> bytes(8U*4U*sizeof(float));
    require(device.read_texture(texture,0U,0U,bytes,8U*sizeof(float),&error),error.c_str());
    for(std::uint32_t y=0;y<4U;++y)for(std::uint32_t x=0;x<8U;++x){
        float value=0.0F; std::memcpy(&value,bytes.data()+
            static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y)*8U+x)*sizeof(float)),sizeof(float));
        const float expected=(x>=2U&&x<5U&&y>=1U&&y<3U)?1.0F:0.25F;
        require(std::abs(value-expected)<1.0e-6F,"depth-region clear modified the wrong pixels");
    }
    require(device.destroy_texture(texture,&error),error.c_str());
}

template <typename Device>
void run(Device& device, bool readback) {
    std::string error;
    TextureDesc depth; depth.format=TextureFormat::D32Float; depth.width=32U; depth.height=32U;
    depth.usage=TextureUsage::DepthStencil|TextureUsage::CopySource;
    depth.initialState=ResourceState::DepthWrite; depth.debugName="depth-only fixture";
    const auto depthTexture=device.create_texture(depth,&error); require(static_cast<bool>(depthTexture),error.c_str());
    const std::array<std::uint32_t,3> indices{0U,1U,2U};
    BufferDesc vb; vb.bytes=16U; vb.usage=BufferUsage::Vertex; vb.memory=MemoryDomain::Upload;
    vb.initialState=ResourceState::ShaderRead; vb.debugName="depth-only vertex placeholder";
    BufferDesc ib; ib.bytes=sizeof(indices); ib.usage=BufferUsage::Index; ib.memory=MemoryDomain::Upload;
    ib.initialState=ResourceState::ShaderRead; ib.debugName="depth-only indices";
    const auto vertex=device.create_buffer(vb,&error), index=device.create_buffer(ib,&error);
    require(vertex&&index,error.c_str());
    require(device.write_buffer(index,0U,std::as_bytes(std::span(indices)),&error),error.c_str());
    GraphicsPipelineDesc pipelineDesc; pipelineDesc.debugName="depth-only pipeline";
    pipelineDesc.vertexBytecode=vertex_shader(); pipelineDesc.fragmentBytecode.clear();
    pipelineDesc.fragmentEntryPoint.clear(); pipelineDesc.colorFormat.reset();
    pipelineDesc.depthFormat=TextureFormat::D32Float; pipelineDesc.cullMode=CullMode::Disabled;
    const auto pipeline=device.create_graphics_pipeline(pipelineDesc,&error); require(static_cast<bool>(pipeline),error.c_str());
    auto commands=device.begin_commands(QueueKind::Graphics,"depth-only commands",&error); require(static_cast<bool>(commands),error.c_str());
    RenderPassDesc pass; pass.debugName="depth-only render pass";
    pass.depth=RenderPassDepthAttachment{depthTexture,true,1.0F};
    require(device.begin_render_pass(commands,pass,&error),error.c_str());
    require(device.bind_graphics_pipeline(commands,pipeline,&error),error.c_str());
    require(device.set_viewport(commands,{0,0,32,32,0,1},&error),error.c_str());
    require(device.set_scissor(commands,{0,0,32,32},&error),error.c_str());
    require(device.bind_vertex_buffer(commands,0U,vertex,0U,4U,&error),error.c_str());
    require(device.bind_index_buffer(commands,index,0U,IndexFormat::Uint32,&error),error.c_str());
    require(device.draw_indexed(commands,3U,1U,0U,0,0U,&error),error.c_str());
    require(device.end_render_pass(commands,&error),error.c_str());
    const auto fence=device.submit(commands,&error); require(fence&&device.fence_complete(fence),error.c_str());
    if(readback){
        std::vector<std::byte> bytes(32U*32U*sizeof(float));
        require(device.read_texture(depthTexture,0U,0U,bytes,32U*sizeof(float),&error),error.c_str());
        bool changed=false; for(std::size_t offset=0;offset<bytes.size();offset+=sizeof(float)){
            float value=1.0F; std::memcpy(&value,bytes.data()+offset,sizeof(float));
            if(value<0.99F){changed=true;break;}
        }
        require(changed,"depth-only rasterization did not update D32");
    }
    require(device.destroy_graphics_pipeline(pipeline,&error),error.c_str());
    require(device.destroy_buffer(vertex,&error)&&device.destroy_buffer(index,&error),error.c_str());
    require(device.destroy_texture(depthTexture,&error),error.c_str());
}
}

int main(){
    try{
        NullDevice nullDevice; verify_region_clear(nullDevice); run(nullDevice,false);
#ifndef DVE_DEPTH_ONLY_NULL_ONLY
        if (std::getenv("DVE_SKIP_VULKAN") == nullptr) {
            VulkanDevice vulkan;
            if(vulkan.status()==DeviceStatus::Ready) { verify_region_clear(vulkan); run(vulkan,true); }
            else if(std::getenv("DVE_REQUIRE_VULKAN")) throw std::runtime_error(std::string(vulkan.device_loss_reason()));
        }
#endif
        std::cout<<"dve_depth_only_rhi_tests: PASS\n"; return 0;
    }catch(const std::exception& e){std::cerr<<"dve_depth_only_rhi_tests: FAIL: "<<e.what()<<'\n';return 1;}
}
