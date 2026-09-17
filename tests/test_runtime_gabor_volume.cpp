#include "dve/render/gabor_volume_renderer.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/runtime_gabor_volume_world.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while(false)

namespace {
dve::GaborVolumeAsset make_asset(float offset=0.0F){
    dve::GaborVolumeAsset asset; asset.name="Runtime Gabor";
    asset.primitives={
        {{-0.4F+offset,0,0},{0.35F,0.25F,0.3F},{},{0.7F,0.8F,1.0F},1.2F,0.0F,3.0F,0,0},
        {{ 0.4F+offset,0,0},{0.2F,0.3F,0.25F},{},{1.0F,0.6F,0.4F},0.9F,3.0F,3.0F,1,2}
    };
    asset.recompute_bounds_and_hash(); return asset;
}
void run(){
    auto asset=make_asset(); std::string error;
    dve::RuntimeGaborVolumeWorld world;
    const auto object=world.create(asset,{},77,asset.contentHash,&error);CHECK(object==77);CHECK(world.create(asset,{},77,asset.contentHash,&error)==0);
    auto snapshot=world.snapshot();CHECK(snapshot.validate(&error));CHECK(snapshot.assets.size()==1);CHECK(snapshot.instances.size()==1);

    dve::rhi::NullDevice device; dve::render::GaborGpuCache cache(device);
    CHECK(cache.upload(asset.contentHash,asset,&error));CHECK(cache.upload(asset.contentHash,asset,&error));CHECK(cache.stats().unchanged==1);
    dve::render::GaborVolumeHistoryState history;
    dve::GaborVolumeRenderSettings settings; settings.maximumPrimitivesPerTile=8;
    auto plan=dve::render::make_gabor_volume_render_plan(snapshot,cache,settings,128,96,256.0F,1,&history);
    CHECK(plan.validate(&error));CHECK(plan.enabled);CHECK(plan.packets.size()==1);CHECK(plan.temporalHistoryReset);CHECK(plan.dispatches.size()==5);
    auto second=dve::render::make_gabor_volume_render_plan(snapshot,cache,settings,128,96,256.0F,1,&history);CHECK(second.temporalHistoryRead);CHECK(!second.temporalHistoryReset);
    auto cut=dve::render::make_gabor_volume_render_plan(snapshot,cache,settings,128,96,256.0F,2,&history);CHECK(cut.temporalHistoryReset);

    dve::render::GaborVolumePipelines pipelines;
    dve::rhi::ComputePipelineDesc desc;desc.threadsX=8;desc.threadsY=8;
    pipelines.buildTileLists=device.create_compute_pipeline(desc,&error);pipelines.integrate=device.create_compute_pipeline(desc,&error);
    pipelines.resolveTemporal=device.create_compute_pipeline(desc,&error);pipelines.castVolumeShadows=device.create_compute_pipeline(desc,&error);pipelines.composite=device.create_compute_pipeline(desc,&error);
    CHECK(pipelines.buildTileLists&&pipelines.integrate&&pipelines.resolveTemporal&&pipelines.castVolumeShadows&&pipelines.composite);
    const auto commands=device.begin_commands(dve::rhi::QueueKind::Compute,"gabor frame",&error);CHECK(commands);
    CHECK(dve::render::record_gabor_volume_frame(device,commands,pipelines,plan,&error));CHECK(device.submit(commands,&error));CHECK(device.statistics().dispatchesExecuted==plan.dispatches.size());

    auto changed=make_asset(0.1F);CHECK(world.update_asset(object,changed,&error));snapshot=world.snapshot();CHECK(snapshot.validate(&error));CHECK(snapshot.assets.front().contentHash==changed.contentHash);
    CHECK(cache.upload(changed.contentHash,changed,&error));
    auto instance=*world.find_instance(object);instance.visible=false;CHECK(world.update_instance(object,instance,&error));
    snapshot=world.snapshot();auto hidden=dve::render::make_gabor_volume_render_plan(snapshot,cache,settings,64,64,100,3,nullptr);CHECK(!hidden.enabled);
    CHECK(world.destroy(object));CHECK(world.snapshot().assets.empty());
}
}
int main(){try{run();std::cout<<"runtime Gabor volume tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
