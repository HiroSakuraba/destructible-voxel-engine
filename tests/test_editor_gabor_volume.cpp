#include "dve/editor_command.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_workspace.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while(false)

namespace {
dve::GaborVolumeAsset make_asset(){
    dve::GaborVolumeAsset asset;asset.name="Editor Smoke";asset.material.densityMultiplier=2.0F;
    asset.primitives={{{0,0,0},{0.6F,0.4F,0.5F},{},{0.7F,0.8F,1.0F},1.0F,0.0F,3.0F,0,0},
                      {{0.5F,0.1F,0},{0.25F,0.2F,0.3F},{},{1.0F,0.5F,0.3F},0.8F,4.0F,3.0F,1,3}};
    asset.recompute_bounds_and_hash();return asset;
}
void run(){
    using namespace dve;using namespace dve::editor;
    EditorWorkspace workspace(EditorDocument("Gabor Project"));
    CHECK(workspace.menus().find("create.gabor_empty")!=nullptr);
    CHECK(workspace.menus().find("create.gabor_import")!=nullptr);
    CHECK(workspace.menus().find("window.gabor_inspector")!=nullptr);
    CHECK(workspace.settings().find("render.gabor.enabled")!=nullptr);
    CHECK(workspace.settings().find("render.gabor.mode")!=nullptr);

    const auto id=workspace.document().allocate_object_id();EditorObject object(id,"Cloud");
    object.gaborVolume=make_asset();object.sourceAsset="assets/volumes/cloud.dgabor";
    object.flags.structural=false;object.flags.collisionEnabled=false;object.flags.decorative=true;
    CHECK(workspace.commands().execute(workspace.document(),std::make_unique<AddObjectCommand>(std::move(object),"Add Gabor")).success);
    workspace.select_object(id);
    const auto* added=workspace.document().find_object(id);CHECK(added&&added->is_gabor_volume());CHECK(workspace.document().validate());
    const auto bounds=object_world_bounds(*added);CHECK(bounds.valid);CHECK(bounds.maximum.x>bounds.minimum.x);
    EditorCamera camera;frame_camera_on_bounds(camera,bounds);const UiRect viewport{0,0,800,600};
    const auto draw=build_gabor_volume_draw_list(workspace.document(),camera,viewport,id);CHECK(draw.size()==1);CHECK(draw.front().selected);CHECK(draw.front().primitiveCount==2);
    const auto ray=make_viewport_ray(camera,viewport,400.0F,300.0F);const auto pick=pick_editor_document(workspace.document(),ray);CHECK(pick&&pick->objectId==id);

    auto before=GaborVolumeObjectState{added->gaborVolume,added->sourceAsset,added->importRecipe};
    auto changed=*added->gaborVolume;changed.material.densityMultiplier=4.0F;changed.recompute_bounds_and_hash();
    auto after=GaborVolumeObjectState{changed,added->sourceAsset,added->importRecipe};
    CHECK(workspace.commands().execute(workspace.document(),std::make_unique<ReplaceGaborVolumeObjectCommand>(id,before,after)).success);
    CHECK(workspace.document().find_object(id)->gaborVolume->material.densityMultiplier==4.0F);
    CHECK(workspace.commands().undo(workspace.document()).success);CHECK(workspace.document().find_object(id)->gaborVolume->material.densityMultiplier==2.0F);
    CHECK(workspace.commands().redo(workspace.document()).success);

    const auto root=std::filesystem::temp_directory_path()/"dve_editor_gabor_volume";std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    const auto saved=workspace.document().save_transactional(root/"cloud.dvescene");CHECK(saved.success);CHECK(std::filesystem::is_regular_file(saved.revisionDirectory/(std::to_string(id)+".dgabor")));
    std::string error;const auto loaded=EditorDocument::load(saved.manifestPath,&error);CHECK(loaded);const auto* loadedObject=loaded->find_object(id);CHECK(loadedObject&&loadedObject->gaborVolume);CHECK(loadedObject->gaborVolume->contentHash==workspace.document().find_object(id)->gaborVolume->contentHash);
    NativeEditorController controller(EditorWorkspace(EditorDocument("Gabor Settings")));
    CHECK(controller.dispatch_action("render.gabor.quality_high"));
    CHECK(controller.dispatch_action("render.gabor.mode_absorption"));
    CHECK(controller.dispatch_action("render.gabor.temporal"));
    const auto renderSettings=controller.gabor_render_settings();
    CHECK(renderSettings.quality==GaborVolumeQuality::High);
    CHECK(renderSettings.mode==GaborVolumeMode::AbsorptionPreview);
    CHECK(!renderSettings.temporalAccumulation);
    EditorObject planned(controller.workspace().document().allocate_object_id(),"Planned Cloud");planned.gaborVolume=make_asset();planned.flags.structural=false;planned.flags.collisionEnabled=false;
    controller.workspace().document().add_object(std::move(planned));
    const auto plans=controller.gabor_frame_plans(512.0F);CHECK(plans.size()==1);CHECK(plans.front().raySteps==160);CHECK(!plans.front().temporalHistory);
    std::filesystem::remove_all(root);
}
}
int main(){try{run();std::cout<<"editor Gabor volume tests: PASS\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
