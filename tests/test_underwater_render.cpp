#include "dve/editor_prototype.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/underwater_optics.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dve;using namespace dve::player;using namespace dve::editor;
namespace {
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
}
int main(){
 try{
  auto root=std::filesystem::current_path()/"submarine-s2-render-output";
  std::filesystem::remove_all(root);std::filesystem::create_directories(root);
  std::string error;
  check(export_prototype_project(make_submarine_starter(),
      EditorMaterialLibrary::make_default(),root/"project",&error),
      "export Deep Explorer: "+error);
  PlayerBootOptions opts;opts.enableScripts=false;
  opts.physicsBackend=Physics3DBackend::Reference;opts.saveDirectory=root/"saves";
  auto content=open_content_source(root/"project");
  check(bool(content),"open Deep Explorer content");
  auto app=PlayerApp::boot(std::move(content),opts,&error);
  check(bool(app),"boot player: "+error);
  std::vector<GameRenderObject> objects;
  auto view=app->render_view(objects,4.F/3.F);
  check(view.ocean!=nullptr&&view.waterVolume!=nullptr,"S1 ocean/volume exposed");
  check(view.underwaterLamps.size()==2,"moving sub emits two underwater spotlights");
  check(view.underwaterLamps[0].intensity>0,"both lamps powered by battery");
  auto renderer=make_cpu_player_renderer(nullptr);
  check(renderer->resize(160,120,&error),"renderer resize");
  // Force an entirely submerged camera near sub so the underlying polygon
  // geometry supplies bright reference pixels for lamp and medium tests.
  view.camera.position={0,-28,-2.F};view.camera.target={0,-16,-2.F};
  check(renderer->render(view,&error),"underwater HDR render: "+error);
  const auto underwater=renderer->readback()->pixels;
  check(renderer->render(view,&error),"repeat underwater render: "+error);
  check(renderer->readback()->pixels==underwater,
        "suspended particles and physical ocean rendering are frame deterministic");
  auto withoutOptics=view;withoutOptics.waterVolume=nullptr;
  check(renderer->render(withoutOptics,&error),"reference no-optics render: "+error);
  check(renderer->readback()->pixels!=underwater,
        "water volume changes physical rendered image before tonemapping");
  // Lamp contribution must be distinct from environment-only daylight.
  auto noLamps=view;noLamps.underwaterLamps={};
  check(renderer->render(noLamps,&error),"headlights off render: "+error);
  check(renderer->readback()->pixels!=underwater,
        "white headlamps illuminate near objects and scatter within water");
  WaterVolumeSettings muddy=*view.waterVolume;
  muddy.clarity=WaterClarity::Turbid;
  auto turbid=view;turbid.waterVolume=&muddy;
  check(renderer->render(turbid,&error),"turbid ocean HDR render: "+error);
  check(renderer->readback()->pixels!=underwater,
        "physical turbidity changes visible pixel radiance");
  // Air-camera view should see geometry/sky in air without full-distance blue tint.
  auto air=view;air.camera.position={0,-28,8};air.camera.target={0,-16,-1};
  check(renderer->render(air,&error),"air/water transition render: "+error);
  const auto airFrame=renderer->readback()->pixels;
  check(airFrame!=underwater,"crossing the wavy air/water boundary changes shading");
  std::filesystem::remove_all(root);
  std::cout<<"S2 Deep Explorer HDR water/air/lamp framebuffer tests passed\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<"S2 renderer: "<<e.what()<<'\n';return 1;}
}
