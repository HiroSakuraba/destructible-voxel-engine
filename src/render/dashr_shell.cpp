#include "dve/render/dashr_shell.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace dve::render {
namespace {

constexpr float kShellEpsilon = 1.0e-8F;

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

Float3 sub(Float3 a, Float3 b) noexcept {
    return {a.x-b.x,a.y-b.y,a.z-b.z};
}
Float3 add(Float3 a, Float3 b) noexcept {
    return {a.x+b.x,a.y+b.y,a.z+b.z};
}
Float3 mul(Float3 a, float s) noexcept {
    return {a.x*s,a.y*s,a.z*s};
}
float dot(Float3 a, Float3 b) noexcept {
    return a.x*b.x+a.y*b.y+a.z*b.z;
}
Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
Float3 normalize(Float3 value) noexcept {
    const float squared=dot(value,value);
    if (!(squared>kShellEpsilon*kShellEpsilon) || !std::isfinite(squared)) return {};
    return mul(value,1.0F/std::sqrt(squared));
}

std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t result=std::max<std::size_t>(current,256U);
    while(result<required) result += result/2U;
    return result;
}

} // namespace

std::optional<DashrShellMesh> build_dashr_shell_mesh(
    const CookedPolygonAsset& asset,
    const DashrSurfaceSettings& settings,
    std::span<const Float3> deformedPositions,
    std::string* error) {
    const auto valid=validate_polygon_asset(asset);
    if(!valid) { set_error(error,valid.message); return std::nullopt; }
    std::string settingsError;
    if(!validate_dashr_surface_settings(settings,&settingsError)) {
        set_error(error,settingsError); return std::nullopt;
    }
    if(!deformedPositions.empty() && deformedPositions.size()!=asset.vertices.size()) {
        set_error(error,"DASHR shell deformed position count does not match asset");
        return std::nullopt;
    }

    const float mapped0=dashr_map_height(0.0F,settings);
    const float mapped1=dashr_map_height(1.0F,settings);
    const float minimumSurface=std::min(mapped0,mapped1)-settings.envelopePadding;
    const float maximumSurface=std::max(mapped0,mapped1)+settings.envelopePadding;
    const float minimumExtrusion=minimumSurface-0.5F;
    const float maximumExtrusion=maximumSurface-0.5F;

    DashrShellMesh result;
    result.stats.minimumExtrusion=minimumExtrusion;
    result.stats.maximumExtrusion=maximumExtrusion;
    result.stats.sourceTriangles=asset.indices.size()/3U;
    result.vertices.reserve(result.stats.sourceTriangles*6U);
    result.indices.reserve(result.stats.sourceTriangles*24U);
    result.submeshes.reserve(asset.submeshes.size());

    const auto position=[&](std::uint32_t index)->Float3 {
        return deformedPositions.empty()?asset.vertices[index].position:deformedPositions[index];
    };

    for(const PolygonSubmesh& submesh:asset.submeshes) {
        DashrShellSubmeshRange range;
        range.materialIndex=submesh.materialIndex;
        range.firstIndex=static_cast<std::uint32_t>(result.indices.size());
        const std::uint32_t end=submesh.firstIndex+submesh.indexCount;
        for(std::uint32_t index=submesh.firstIndex;index<end;index+=3U) {
            const std::uint32_t i0=asset.indices[index];
            const std::uint32_t i1=asset.indices[index+1U];
            const std::uint32_t i2=asset.indices[index+2U];
            const Float3 p0=position(i0),p1=position(i1),p2=position(i2);
            const Float3 n=normalize(cross(sub(p1,p0),sub(p2,p0)));
            if(dot(n,n)<=kShellEpsilon) {
                ++result.stats.degenerateTrianglesSkipped;
                continue;
            }
            const Float2 uv0=asset.vertices[i0].texcoord;
            const Float2 uv1=asset.vertices[i1].texcoord;
            const Float2 uv2=asset.vertices[i2].texcoord;

            const std::uint32_t base=static_cast<std::uint32_t>(result.vertices.size());
            const auto emit=[&](Float3 p,Float2 uv) {
                result.vertices.push_back({p.x,p.y,p.z,uv.x,uv.y});
            };
            emit(add(p0,mul(n,maximumExtrusion)),uv0);
            emit(add(p1,mul(n,maximumExtrusion)),uv1);
            emit(add(p2,mul(n,maximumExtrusion)),uv2);
            emit(add(p0,mul(n,minimumExtrusion)),uv0);
            emit(add(p1,mul(n,minimumExtrusion)),uv1);
            emit(add(p2,mul(n,minimumExtrusion)),uv2);

            const std::uint32_t local[]{
                0,1,2, 5,4,3,
                0,3,4, 0,4,1,
                1,4,5, 1,5,2,
                2,5,3, 2,3,0
            };
            for(const std::uint32_t value:local) result.indices.push_back(base+value);
            result.stats.emittedTriangles += 8U;
        }
        range.indexCount=static_cast<std::uint32_t>(result.indices.size())-range.firstIndex;
        result.submeshes.push_back(range);
    }
    if(result.vertices.empty()||result.indices.empty()) {
        set_error(error,"DASHR shell contains no non-degenerate triangles");
        return std::nullopt;
    }
    return result;
}

DashrShellMeshMirror::~DashrShellMeshMirror(){reset();}

bool DashrShellMeshMirror::ensure_buffer(
    rhi::BufferHandle& handle,std::size_t& capacity,std::size_t required,
    rhi::BufferUsage usage,std::string_view name,std::string* error) {
    const std::size_t bytes=std::max<std::size_t>(required,4U);
    if(handle&&capacity>=bytes) return true;
    if(handle&&!device_.destroy_buffer(handle,error)) return false;
    capacity=grown_capacity(0U,bytes);
    rhi::BufferDesc desc;
    desc.bytes=capacity;desc.usage=usage|rhi::BufferUsage::CopyDestination|rhi::BufferUsage::CopySource;
    desc.memory=rhi::MemoryDomain::DeviceLocal;desc.initialState=rhi::ResourceState::ShaderRead;
    desc.debugName.assign(name.begin(),name.end());
    handle=device_.create_buffer(desc,error);
    if(!handle){capacity=0U;return false;}
    return true;
}

bool DashrShellMeshMirror::upload(const DashrShellMesh& shell,std::string* error) {
    if(shell.vertices.empty()||shell.indices.empty()||
       shell.indices.size()>std::numeric_limits<std::uint32_t>::max()) {
        set_error(error,"DASHR shell upload is empty or exceeds 32-bit index count");
        return false;
    }
    const auto vertices=std::as_bytes(std::span(shell.vertices));
    const auto indices=std::as_bytes(std::span(shell.indices));
    if(!ensure_buffer(vertexBuffer_,vertexCapacity_,vertices.size(),rhi::BufferUsage::Vertex,
                      "DASHR conservative shell vertices",error)||
       !ensure_buffer(indexBuffer_,indexCapacity_,indices.size(),rhi::BufferUsage::Index,
                      "DASHR conservative shell indices",error)||
       !device_.write_buffer(vertexBuffer_,0U,vertices,error)||
       !device_.write_buffer(indexBuffer_,0U,indices,error)) return false;
    indexCount_=static_cast<std::uint32_t>(shell.indices.size());
    submeshes_=shell.submeshes;
    return true;
}

void DashrShellMeshMirror::reset() noexcept {
    if(vertexBuffer_)(void)device_.destroy_buffer(vertexBuffer_,nullptr);
    if(indexBuffer_)(void)device_.destroy_buffer(indexBuffer_,nullptr);
    vertexBuffer_={};indexBuffer_={};vertexCapacity_=indexCapacity_=0U;indexCount_=0U;
    submeshes_.clear();
}

bool create_dashr_shell_renderer(
    rhi::IDevice& device,const DashrShellShaderBytecode& bytecode,
    rhi::TextureFormat colorFormat,std::size_t maximumConstantBytes,
    DashrShellRendererResources& out,std::string* error) {
    if(!bytecode.valid()||colorFormat==rhi::TextureFormat::D32Float||
       maximumConstantBytes<sizeof(GpuDashrShellConstants)) {
        set_error(error,"DASHR shell renderer creation arguments are invalid");
        return false;
    }
    DashrShellRendererResources r;
    std::string local;
    auto fail=[&](std::string message) {
        std::string ignored;(void)destroy_dashr_shell_renderer(device,r,&ignored);
        set_error(error,std::move(message));return false;
    };
    const std::size_t alignment=std::max<std::size_t>(
        16U,device.capabilities().minUniformBufferOffsetAlignment);
    const auto align_up=[&](std::size_t value) {
        const std::size_t rem=value%alignment;
        return rem==0U?value:value+alignment-rem;
    };
    r.constantStride=align_up(sizeof(GpuDashrShellConstants));
    r.constantCapacity=align_up(maximumConstantBytes);

    rhi::BufferDesc constants;
    constants.bytes=r.constantCapacity;
    constants.usage=rhi::BufferUsage::Constant|rhi::BufferUsage::CopyDestination;
    constants.memory=rhi::MemoryDomain::Upload;
    constants.initialState=rhi::ResourceState::ShaderRead;
    constants.debugName="DASHR shell constants";
    r.constants=device.create_buffer(constants,&local);
    if(!r.constants)return fail(local);

    rhi::BindGroupLayoutDesc constantsLayout;
    constantsLayout.debugName="DASHR shell constants layout";
    constantsLayout.bindings={{0U,rhi::BindingType::UniformBuffer,
                               rhi::ShaderStage::Vertex|rhi::ShaderStage::Fragment}};
    r.constantsLayout=device.create_bind_group_layout(constantsLayout,&local);
    if(!r.constantsLayout)return fail(local);

    rhi::BindGroupLayoutDesc surfaceLayout;
    surfaceLayout.debugName="DASHR shell explicit surface resources";
    for(std::uint32_t binding=0U;binding<6U;++binding)
        surfaceLayout.bindings.push_back(
            {binding,rhi::BindingType::SampledImage,rhi::ShaderStage::Fragment});
    for(std::uint32_t binding=6U;binding<9U;++binding)
        surfaceLayout.bindings.push_back(
            {binding,rhi::BindingType::Sampler,rhi::ShaderStage::Fragment});
    r.surfaceLayout=device.create_bind_group_layout(surfaceLayout,&local);
    if(!r.surfaceLayout)return fail(local);

    rhi::GraphicsPipelineDesc pipeline;
    pipeline.debugName="DASHR live conservative shell";
    pipeline.vertexBytecode=bytecode.vertex;
    pipeline.fragmentBytecode=bytecode.fragment;
    pipeline.bindGroupLayouts={r.constantsLayout,r.surfaceLayout};
    pipeline.colorFormat=colorFormat;
    pipeline.depthFormat=rhi::TextureFormat::D32Float;
    pipeline.depthTest=true;pipeline.depthWrite=true;
    pipeline.depthCompare=rhi::CompareOp::LessEqual;
    pipeline.cullMode=rhi::CullMode::Disabled;
    pipeline.vertexBuffer=rhi::VertexBufferLayoutDesc{
        static_cast<std::uint32_t>(sizeof(GpuDashrShellVertex)),false};
    pipeline.vertexAttributes={
        {0U,rhi::VertexFormat::Float3,
         static_cast<std::uint32_t>(offsetof(GpuDashrShellVertex,positionX))},
        {1U,rhi::VertexFormat::Float2,
         static_cast<std::uint32_t>(offsetof(GpuDashrShellVertex,uvX))}
    };
    r.pipeline=device.create_graphics_pipeline(pipeline,&local);
    if(!r.pipeline)return fail(local);

    if(out.valid()) {
        std::string ignored;(void)destroy_dashr_shell_renderer(device,out,&ignored);
    }
    out=std::move(r);
    return true;
}

bool destroy_dashr_shell_renderer(
    rhi::IDevice& device,DashrShellRendererResources& r,std::string* error) {
    bool ok=true;std::string local;
    auto destroy=[&](bool result){if(!result){ok=false;if(error&&error->empty())*error=local;}local.clear();};
    if(r.pipeline)destroy(device.destroy_graphics_pipeline(r.pipeline,&local));
    if(r.surfaceLayout)destroy(device.destroy_bind_group_layout(r.surfaceLayout,&local));
    if(r.constantsLayout)destroy(device.destroy_bind_group_layout(r.constantsLayout,&local));
    if(r.constants)destroy(device.destroy_buffer(r.constants,&local));
    r={};return ok;
}

bool record_dashr_shell_frame(
    rhi::IDevice& device,DashrShellRendererResources& renderer,
    const DashrShellFrameDesc& frame,DashrShellFrameStats& stats,
    rhi::FenceHandle* fence,std::string* error) {
    stats={};
    if(!renderer.valid()||!frame.colorTarget||!frame.depthTarget||
       frame.width==0U||frame.height==0U) {
        set_error(error,"DASHR shell frame is invalid");return false;
    }
    const std::size_t required=frame.draws.size()*renderer.constantStride;
    if(required>renderer.constantCapacity) {
        set_error(error,"DASHR shell constant capacity exceeded");return false;
    }

    std::vector<rhi::BindGroupHandle> transient;
    transient.reserve(frame.draws.size()*2U);
    auto cleanup=[&](){
        for(auto it=transient.rbegin();it!=transient.rend();++it)
            if(*it)(void)device.destroy_bind_group(*it,nullptr);
        transient.clear();
    };
    auto fail=[&](){cleanup();return false;};

    struct Prepared {
        const DashrShellDraw* draw{};
        DashrShellSubmeshRange range{};
        rhi::BindGroupHandle constantsGroup{};
        rhi::BindGroupHandle surfaceGroup{};
    };
    std::vector<Prepared> prepared;
    prepared.reserve(frame.draws.size());

    std::size_t constantOffset=0U;
    for(const DashrShellDraw& draw:frame.draws) {
        if(!draw.shell||!draw.atlas||!draw.atlas->valid()||!draw.atlas->published||
           !draw.heightView||!draw.heightSampler||
           draw.shellSubmeshIndex>=draw.shell->submeshes().size()) {
            set_error(error,"DASHR shell draw is incomplete");return fail();
        }
        std::string settingsError;
        if(!validate_dashr_surface_settings(draw.settings,&settingsError)) {
            set_error(error,settingsError);return fail();
        }
        const auto range=draw.shell->submeshes()[draw.shellSubmeshIndex];
        if(range.indexCount==0U) continue;

        GpuDashrShellConstants constants;
        constants.objectToClip=draw.objectToClip;
        constants.cameraAndHeightScale={
            draw.cameraObjectPosition.x,draw.cameraObjectPosition.y,draw.cameraObjectPosition.z,
            draw.settings.heightScale};
        constants.heightAndStep={
            draw.settings.heightReferencePlane,draw.settings.heightOffset,
            draw.settings.envelopePadding,draw.settings.stepSize};
        constants.distortion={
            draw.settings.stepScale,draw.settings.compressionThreshold,
            draw.settings.stretchThreshold,draw.settings.stretchDamping};
        constants.minimumStepAndDebug={
            draw.settings.minimumStepFactor,static_cast<float>(draw.debugMode),0,0};
        constants.limits={
            draw.settings.maximumSteps,draw.settings.refinementSteps,
            draw.settings.maximumTeleports,0U};
        if(!device.write_buffer(renderer.constants,constantOffset,
                                std::as_bytes(std::span(&constants,1U)),error)) return fail();

        rhi::BindGroupDesc constantsGroup;
        constantsGroup.layout=renderer.constantsLayout;
        constantsGroup.debugName="DASHR shell constant range";
        constantsGroup.entries={{0U,renderer.constants,{},constantOffset,sizeof(constants),{}}};
        const auto cg=device.create_bind_group(constantsGroup,error);
        if(!cg)return fail();
        transient.push_back(cg);

        rhi::BindGroupDesc surfaceGroup;
        surfaceGroup.layout=renderer.surfaceLayout;
        surfaceGroup.debugName="DASHR live surface resources";
        for(std::uint32_t binding=0U;binding<4U;++binding)
            surfaceGroup.entries.push_back(
                {binding,{},draw.atlas->filledViews[binding],0U,0U,{}});
        surfaceGroup.entries.push_back({4U,{},draw.atlas->seamView,0U,0U,{}});
        surfaceGroup.entries.push_back({5U,{},draw.heightView,0U,0U,{}});
        surfaceGroup.entries.push_back({6U,{},{},0U,0U,draw.atlas->linearClampSampler});
        surfaceGroup.entries.push_back({7U,{},{},0U,0U,draw.atlas->pointClampSampler});
        surfaceGroup.entries.push_back({8U,{},{},0U,0U,draw.heightSampler});
        const auto sg=device.create_bind_group(surfaceGroup,error);
        if(!sg)return fail();
        transient.push_back(sg);

        prepared.push_back({&draw,range,cg,sg});
        constantOffset+=renderer.constantStride;
        ++stats.constantRanges;
        stats.transientBindGroups+=2U;
    }

    if(prepared.empty()) {cleanup();return true;}

    auto commands=device.begin_commands(rhi::QueueKind::Graphics,"DASHR live shell frame",error);
    if(!commands)return fail();
    rhi::RenderPassDesc pass;
    pass.debugName="DASHR live displaced depth and diagnostic shading";
    pass.colors={{frame.colorTarget,false,0,0,0,0}};
    pass.depth=rhi::RenderPassDepthAttachment{frame.depthTarget,false,1.0F};
    if(!device.begin_render_pass(commands,pass,error)||
       !device.bind_graphics_pipeline(commands,renderer.pipeline,error)||
       !device.set_viewport(commands,{0,0,static_cast<float>(frame.width),
                                     static_cast<float>(frame.height),0,1},error)||
       !device.set_scissor(commands,{0,0,frame.width,frame.height},error))
        return fail();

    for(const Prepared& item:prepared) {
        if(!device.bind_graphics_bind_group(commands,0U,item.constantsGroup,error)||
           !device.bind_graphics_bind_group(commands,1U,item.surfaceGroup,error)||
           !device.bind_vertex_buffer(commands,0U,item.draw->shell->vertex_buffer(),0U,
                                      static_cast<std::uint32_t>(sizeof(GpuDashrShellVertex)),error)||
           !device.bind_index_buffer(commands,item.draw->shell->index_buffer(),0U,
                                     rhi::IndexFormat::Uint32,error)||
           !device.draw_indexed(commands,item.range.indexCount,1U,item.range.firstIndex,0,0U,error))
            return fail();
        ++stats.draws;
        stats.shellTriangles+=item.range.indexCount/3U;
    }
    if(!device.end_render_pass(commands,error))return fail();
    const auto submitted=device.submit(commands,error);
    if(!submitted)return fail();
    cleanup();
    if(fence)*fence=submitted;
    return true;
}

} // namespace dve::render
