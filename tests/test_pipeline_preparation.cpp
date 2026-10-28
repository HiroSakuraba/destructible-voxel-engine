#include "dve/rhi/pipeline_preparation.hpp"
#include "dve/rhi/null_device.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace dve::rhi;
namespace {
void expect(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
GraphicsPipelineDesc graphics(std::string name) {
    GraphicsPipelineDesc out;
    out.debugName=std::move(name);
    out.vertexBytecode={std::byte{1},std::byte{2},std::byte{3},std::byte{4}};
    out.fragmentBytecode={std::byte{4},std::byte{3},std::byte{2},std::byte{1}};
    out.colorFormat=TextureFormat::RGBA8Unorm;
    return out;
}
ComputePipelineDesc compute(std::string name) {
    ComputePipelineDesc out;
    out.debugName=std::move(name);
    out.bytecode={std::byte{1},std::byte{2},std::byte{3},std::byte{4}};
    return out;
}
void run() {
    NullDevice device;
    std::string error;
    PipelinePreparationQueue staging(device);
    expect(!staging.ready_to_publish(1U),"empty generation must fail closed");
    expect(!staging.stage_graphics(0U,"bad",graphics("bad"),&error),"zero epoch admitted");
    expect(!staging.stage_graphics(1U,"",graphics("bad"),&error),"empty key admitted");
    expect(staging.stage_graphics(1U,"material",graphics("material"),&error),"material staging failed");
    expect(staging.stage_graphics(1U,"shadow",graphics("shadow"),&error),"shadow staging failed");
    expect(staging.stage_compute(1U,"gpu-gi",compute("gi"),&error),"GI staging failed");
    expect(!staging.stage_graphics(1U,"material",graphics("duplicate"),&error),
           "duplicate variant key admitted");
    expect(staging.stage_graphics(2U,"material",graphics("new scene material"),&error),
           "separate scene generation unexpectedly rejected");
    expect(staging.state(1U,"material")==PipelinePreparationState::Pending,
           "new material not pending");
    expect(!staging.ready_to_publish(1U),"pending pipelines prematurely published");
    auto work=staging.pump(0U,std::chrono::microseconds{0});
    expect(work.attempted==0U && work.pending==4U,"zero-budget pump compiled a pipeline");
    work=staging.pump(1U,std::chrono::microseconds{5000});
    expect(work.attempted==1U && work.compiled==1U && work.pending==3U,
           "bounded preparation did not compile exactly one variant");
    expect(!staging.ready_to_publish(1U),"incomplete scene became publishable");
    expect(!staging.take_compute(1U,"material"),"wrong pipeline type leaked");
    const auto invalid=staging.take_graphics(99U,"material");
    expect(!invalid,"stale epoch acquired a handle");
    work=staging.pump(2U,std::chrono::microseconds{5000});
    expect(work.attempted==2U && work.compiled==2U && work.pending==1U,
           "second budget must finish only two variants");
    const auto summary=staging.summary(1U);
    expect(summary.allReady && summary.ready==3U && summary.pending==0U,
           "scene not ready after all its requirements finished");
    const auto material=staging.take_graphics(1U,"material");
    const auto shadow=staging.take_graphics(1U,"shadow");
    const auto gi=staging.take_compute(1U,"gpu-gi");
    expect(material && shadow && gi,"prepared handles not transferred");
    expect(!staging.take_graphics(1U,"material"),"published pipeline transferred twice");
    expect(staging.state(1U,"material")==PipelinePreparationState::Published,
           "publish did not advance state");
    staging.discard_epoch(1U);
    expect(!staging.ready_to_publish(1U),"retired scene falsely ready");
    // Ownership transferred to caller; discarding the scene cannot destroy
    // the live pipeline. Explicit device destroys must still succeed.
    expect(device.destroy_graphics_pipeline(material,&error),
           "discarded scene stole the live material pipeline");
    expect(device.destroy_graphics_pipeline(shadow,&error),
           "discarded scene stole the live shadow pipeline");
    expect(device.destroy_compute_pipeline(gi,&error),
           "discarded scene stole the live compute pipeline");
    expect(staging.state(2U,"material")==PipelinePreparationState::Pending,
           "retiring old scene removed new scene's work");
    staging.discard_epoch(2U);
    expect(staging.size()==0U,"discarding new scene did not clear pending work");

    // A driver rejection is a hard publication barrier, not a silently
    // missing variant or partially-published scene.
    expect(staging.stage_graphics(3U,"valid",graphics("good"),&error),"valid staging refused");
    expect(staging.stage_graphics(3U,"invalid",GraphicsPipelineDesc{},&error),
           "failure-case staging refused too early");
    work=staging.pump(10U,std::chrono::microseconds{5000});
    expect(work.attempted==2U && work.compiled==1U && work.failed==1U,
           "driver failure must be recorded");
    expect(!staging.ready_to_publish(3U) && staging.summary(3U).failed==1U,
           "partially failed scene was publishable");
    expect(!staging.failure_reason(3U,"invalid").empty(),
           "failed pipeline lacks reason");
    staging.discard_epoch(3U); // cancels/destroys the unclaimed valid handle
    expect(staging.size()==0U,"stale failed scene not retired");

    expect(staging.stage_graphics(4U,"not-published",graphics("temporary"),&error),
           "could not prepare cancellation test");
    work=staging.pump(1U);
    expect(work.compiled==1U,"temporary pipeline compile failed");
    staging.discard_epoch(4U);
    expect(staging.size()==0U,"cancellation leaked completed work");
    expect(!staging.take_graphics(4U,"not-published"),"cancelled pipeline transferable");
    std::cout<<"PSO-2: queued/staged/published/stale/failure/budget contracts PASS\n";
}
}
int main(){try{run();return 0;}
catch(const std::exception& e){std::cerr<<"PSO-2: "<<e.what()<<"\n";return 1;}}
