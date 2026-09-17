#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/render/camera_frame_graph.hpp"
#include "dve/rhi/vulkan_device.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::render;
using namespace dve::rhi;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
          std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1U) << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}
std::vector<std::uint32_t> encoded_string(const char* text) {
    const std::size_t bytes = std::strlen(text) + 1U;
    std::vector<std::uint32_t> words((bytes + 3U) / 4U, 0U);
    std::memcpy(words.data(), text, bytes);
    return words;
}
void emit_entry_point(std::vector<std::uint32_t>& words, std::uint32_t model,
                      std::uint32_t function, const char* name,
                      std::initializer_list<std::uint32_t> interfaces) {
    auto stringWords = encoded_string(name);
    const std::uint32_t count = 3U + static_cast<std::uint32_t>(stringWords.size()) +
                                static_cast<std::uint32_t>(interfaces.size());
    words.push_back((count << 16U) | 15U);
    words.push_back(model);
    words.push_back(function);
    words.insert(words.end(), stringWords.begin(), stringWords.end());
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}
std::vector<std::byte> as_bytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::byte> result(words.size() * sizeof(std::uint32_t));
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}
std::vector<std::byte> vertex_shader() {
    std::vector<std::uint32_t> w{0x07230203U, 0x00010000U, 0U, 37U, 0U};
    emit(w,17,{1});emit(w,14,{0,1});emit_entry_point(w,0,26,"main",{24,25});
    emit(w,71,{13,2});emit(w,72,{13,0,11,0});emit(w,71,{24,11,42});emit(w,19,{1});emit(w,33,{2,1});
    emit(w,22,{3,32});emit(w,23,{4,3,2});emit(w,23,{5,3,4});emit(w,21,{6,32,0});emit(w,21,{7,32,1});
    emit(w,43,{6,8,3});emit(w,28,{9,4,8});emit(w,32,{10,6,9});emit(w,32,{11,6,4});emit(w,30,{13,5});
    emit(w,32,{12,3,13});emit(w,32,{14,3,5});emit(w,32,{36,1,7});
    emit(w,43,{3,15,std::bit_cast<std::uint32_t>(-0.8F)});emit(w,43,{3,16,std::bit_cast<std::uint32_t>(0.8F)});
    emit(w,43,{3,17,std::bit_cast<std::uint32_t>(0.0F)});emit(w,43,{3,18,std::bit_cast<std::uint32_t>(1.0F)});
    emit(w,44,{4,19,15,15});emit(w,44,{4,20,16,15});emit(w,44,{4,21,17,16});emit(w,44,{9,22,19,20,21});
    emit(w,43,{7,35,0});emit(w,59,{10,23,6,22});emit(w,59,{36,24,1});emit(w,59,{12,25,3});
    emit(w,54,{1,26,0,2});emit(w,248,{27});emit(w,61,{7,28,24});emit(w,65,{11,29,23,28});emit(w,61,{4,30,29});
    emit(w,81,{3,31,30,0});emit(w,81,{3,32,30,1});emit(w,80,{5,33,31,32,17,18});emit(w,65,{14,34,25,35});
    emit(w,62,{34,33});emit(w,253,{});emit(w,56,{});return as_bytes(w);
}
std::vector<std::byte> fragment_shader(float red, float green, float blue) {
    std::vector<std::uint32_t> w{0x07230203U,0x00010000U,0U,14U,0U};
    emit(w,17,{1});emit(w,14,{0,1});emit_entry_point(w,4,11,"main",{6});emit(w,16,{11,7});emit(w,71,{6,30,0});
    emit(w,19,{1});emit(w,33,{2,1});emit(w,22,{3,32});emit(w,23,{4,3,4});emit(w,32,{5,3,4});emit(w,59,{5,6,3});
    emit(w,43,{3,7,std::bit_cast<std::uint32_t>(red)});emit(w,43,{3,8,std::bit_cast<std::uint32_t>(green)});
    emit(w,43,{3,9,std::bit_cast<std::uint32_t>(blue)});emit(w,43,{3,10,std::bit_cast<std::uint32_t>(1.0F)});
    emit(w,44,{4,12,7,8,9,10});emit(w,54,{1,11,0,2});emit(w,248,{13});emit(w,62,{6,12});emit(w,253,{});emit(w,56,{});
    return as_bytes(w);
}
CameraViewportFrame frame(CameraViewportId id, std::string name, std::string target,
                          float x, float width, Float3 position) {
    CameraViewportFrame result;
    result.viewport = {id, std::move(name), 1U, x, 0.0F, width, 1.0F, std::move(target), true};
    result.pose.position = position;
    result.pose.target = {0,0,0};
    result.gpu = pack_camera_gpu_packet(result.viewport,result.pose,result.postProcess,0U,true);
    return result;
}
void write_ppm(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               const std::vector<std::byte>& pixels) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not open Vulkan camera image output");
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x) {
        const std::size_t offset=(static_cast<std::size_t>(y)*width+x)*4U;
        const std::array<char,3> rgb{static_cast<char>(std::to_integer<std::uint8_t>(pixels[offset])),
                                     static_cast<char>(std::to_integer<std::uint8_t>(pixels[offset+1U])),
                                     static_cast<char>(std::to_integer<std::uint8_t>(pixels[offset+2U]))};
        output.write(rgb.data(),3);
    }
}
}

int main() {
    try {
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN"))
                throw std::runtime_error(std::string("required Vulkan camera device unavailable: ") +
                                         std::string(device.device_loss_reason()));
            std::cout << "Vulkan camera outputs test skipped: " << device.device_loss_reason() << '\n';
            return 0;
        }
        std::string error;
        CameraFrameGraph graph;
        graph.begin_frame(128U,64U);
        require(graph.submit(frame(1,"Left","main",0.0F,0.5F,{-2,1,5}),128U,64U,&error),error.c_str());
        require(graph.submit(frame(2,"Right","main",0.5F,0.5F,{2,1,5}),128U,64U,&error),error.c_str());
        require(graph.submit(frame(3,"Security","security",0.0F,1.0F,{0,8,0}),64U,64U,&error),error.c_str());
        const CameraFramePlan plan=graph.finalize();
        require(plan.viewports.size()==3U&&plan.targets.size()==2U,"camera frame graph lost a target or viewport");
        const BufferHandle cameraPackets=device.create_buffer({plan.packetUpload.size(),BufferUsage::Constant|BufferUsage::CopyDestination,MemoryDomain::Upload,"camera packet array",ResourceState::ShaderRead},&error);
        require(static_cast<bool>(cameraPackets), error.c_str());require(graph.upload_packets(device,cameraPackets,plan,&error),error.c_str());
        std::vector<std::byte> packetReadback(plan.packetUpload.size());require(device.read_buffer(cameraPackets,0U,packetReadback,&error),error.c_str());require(packetReadback==plan.packetUpload,"Vulkan camera packet upload/readback mismatch");

        const auto make_target=[&](std::uint32_t w,std::uint32_t h,const char* name){return device.create_texture({TextureDimension::Texture2D,TextureFormat::RGBA8Unorm,w,h,1U,1U,1U,TextureUsage::RenderTarget|TextureUsage::CopySource,ResourceState::RenderTarget,name},&error);};
        const auto make_depth=[&](std::uint32_t w,std::uint32_t h,const char* name){return device.create_texture({TextureDimension::Texture2D,TextureFormat::D32Float,w,h,1U,1U,1U,TextureUsage::DepthStencil,ResourceState::DepthWrite,name},&error);};
        const TextureHandle mainColor=make_target(128,64,"camera split-screen");const TextureHandle mainDepth=make_depth(128,64,"camera split depth");const TextureHandle securityColor=make_target(64,64,"security camera");const TextureHandle securityDepth=make_depth(64,64,"security depth");require(mainColor&&mainDepth&&securityColor&&securityDepth,error.c_str());
        const auto make_pipeline=[&](float r,float g,float b,const char* name){GraphicsPipelineDesc desc;desc.debugName=name;desc.vertexBytecode=vertex_shader();desc.fragmentBytecode=fragment_shader(r,g,b);desc.colorFormat=TextureFormat::RGBA8Unorm;desc.depthFormat=TextureFormat::D32Float;desc.cullMode=CullMode::Disabled;return device.create_graphics_pipeline(desc,&error);};
        const auto leftPipeline=make_pipeline(0.10F,0.35F,0.95F,"left camera");const auto rightPipeline=make_pipeline(0.95F,0.30F,0.08F,"right camera");const auto securityPipeline=make_pipeline(0.10F,0.85F,0.25F,"security camera");require(leftPipeline&&rightPipeline&&securityPipeline,error.c_str());
        const BufferHandle vertices=device.create_buffer({16U,BufferUsage::Vertex,MemoryDomain::DeviceLocal,"dummy camera vertices",ResourceState::ShaderRead},&error);const BufferHandle indices=device.create_buffer({3U*sizeof(std::uint32_t),BufferUsage::Index,MemoryDomain::Upload,"camera indices",ResourceState::ShaderRead},&error);require(vertices&&indices,error.c_str());const std::array<std::uint32_t,3> indexData{0,1,2};require(device.write_buffer(indices,0U,std::as_bytes(std::span<const std::uint32_t>(indexData)),&error),error.c_str());

        auto draw=[&](TextureHandle color,TextureHandle depth,std::uint32_t w,std::uint32_t h,const std::vector<std::tuple<GraphicsPipelineHandle,Viewport,ScissorRect>>& views,const char* name){const auto commands=device.begin_commands(QueueKind::Graphics,name,&error);require(static_cast<bool>(commands),error.c_str());RenderPassDesc pass;pass.colors.push_back({color,true,0.01F,0.015F,0.025F,1.0F});pass.depth=RenderPassDepthAttachment{depth,true,1.0F};require(device.begin_render_pass(commands,pass,&error),error.c_str());for(const auto& [pipeline,viewport,scissor]:views){require(device.bind_graphics_pipeline(commands,pipeline,&error),error.c_str());require(device.set_viewport(commands,viewport,&error),error.c_str());require(device.set_scissor(commands,scissor,&error),error.c_str());require(device.bind_vertex_buffer(commands,0U,vertices,0U,4U,&error),error.c_str());require(device.bind_index_buffer(commands,indices,0U,IndexFormat::Uint32,&error),error.c_str());require(device.draw_indexed(commands,3U,1U,0U,0,0U,&error),error.c_str());}require(device.end_render_pass(commands,&error),error.c_str());const auto fence=device.submit(commands,&error);require(fence&&device.fence_complete(fence),error.c_str());(void)w;(void)h;};
        draw(mainColor,mainDepth,128,64,{{leftPipeline,{0,0,64,64,0,1},{0,0,64,64}},{rightPipeline,{64,0,64,64,0,1},{64,0,64,64}}},"split-screen cameras");
        draw(securityColor,securityDepth,64,64,{{securityPipeline,{0,0,64,64,0,1},{0,0,64,64}}},"named security target");
        std::vector<std::byte> mainPixels(128U*64U*4U),securityPixels(64U*64U*4U);require(device.read_texture(mainColor,0,0,mainPixels,128U*4U,&error),error.c_str());require(device.read_texture(securityColor,0,0,securityPixels,64U*4U,&error),error.c_str());
        const auto channel=[](const auto& pixels,std::uint32_t width,std::uint32_t x,std::uint32_t y,std::uint32_t c){return std::to_integer<std::uint8_t>(pixels[(static_cast<std::size_t>(y)*width+x)*4U+c]);};
        require(channel(mainPixels,128,32,32,2)>160U,"left split-screen camera did not render blue");require(channel(mainPixels,128,96,32,0)>160U,"right split-screen camera did not render orange");require(channel(securityPixels,64,32,32,1)>160U,"named security render target did not render green");
        if(const char* directory=std::getenv("DVE_VULKAN_CAMERA_OUTPUT_DIR")){std::filesystem::create_directories(directory);write_ppm(std::filesystem::path(directory)/"vulkan_split_screen.ppm",128,64,mainPixels);write_ppm(std::filesystem::path(directory)/"vulkan_security_camera.ppm",64,64,securityPixels);}
        std::cout<<"dve_vulkan_camera_outputs_tests: PASS\n";return 0;
    } catch(const std::exception& exception){std::cerr<<"dve_vulkan_camera_outputs_tests: FAIL: "<<exception.what()<<'\n';return 1;}
}
