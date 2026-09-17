#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/editor_camera_sequencer.hpp"

namespace {using namespace dve;using namespace dve::camera;using namespace dve::editor;void require(bool c,const char*m){if(!c)throw std::runtime_error(m);}CameraSequence make(){CameraSequence q;q.name="Edit";q.durationSeconds=10;CameraDollySpline d;d.id=1;d.name="Path";CameraDollyKey a;a.timeSeconds=0;a.pose.position={0,0,5};a.pose.target={0,0,0};CameraDollyKey b=a;b.timeSeconds=5;b.pose.position={5,2,0};d.keys={a,b};q.dollies.push_back(d);CameraShot s;s.id=1;s.name="A";s.startSeconds=0;s.durationSeconds=5;s.dollyId=1;s.blendIn={CameraBlendCurve::Cut,0};CameraShot t;t.id=2;t.name="B";t.startSeconds=5;t.durationSeconds=5;t.rigId=7;t.blendIn={CameraBlendCurve::EaseInOut,0.5F};q.shots={s,t};return q;}
void test(){EditorCameraSequencer e;std::string error;require(e.set_sequence(make(),&error),error.c_str());e.set_snap_seconds(0.5F);e.set_playhead(2.26F);require(e.playhead_seconds()==2.5F,"playhead snapping failed");require(e.select_shot(2),"shot selection failed");require(!e.move_selected_shot(4.0F,&error),"overlapping shot move was accepted");require(e.trim_selected_shot(4.0F,&error),error.c_str());require(e.move_selected_shot(5.5F,&error),error.c_str());const auto layout=e.layout(1000);require(layout.shots.size()==2&&layout.playheadX==250.0F,"sequencer layout incorrect");const std::string otio=e.export_otio_json();require(otio.find("Timeline.1")!=std::string::npos&&otio.find("dve_dolly_id")!=std::string::npos,"OTIO metadata export missing");}
void test_transactional_multi_edit(){EditorCameraSequencer e;auto q=make();q.durationSeconds=12;q.shots[0].durationSeconds=4;q.shots[1].startSeconds=4;q.shots[1].durationSeconds=3;CameraShot c;c.id=3;c.name="C";c.startSeconds=7;c.durationSeconds=3;c.rigId=8;c.blendIn={CameraBlendCurve::Cut,0};q.shots.push_back(c);q.metadata["unknown.otio"]="kept";std::string error;require(e.set_sequence(q,&error),error.c_str());e.set_snap_seconds(0.5F);require(e.select_shot(2),"select B failed");require(e.select_shot(3,true),"additive select C failed");require(e.selected_shots().size()==2,"multi-selection size wrong");require(e.move_selected_shots(1.0F,false,&error),error.c_str());require(e.undo(),"undo failed");require(e.redo(),"redo failed");require(e.roll_boundary(2,3,7.5F,&error),error.c_str());require(e.select_shot(3),"select C failed");require(e.ripple_delete_selected(&error),error.c_str());require(e.sequence().shots.size()==2&&e.sequence().durationSeconds<12.0F,"ripple delete did not close sequence");require(e.export_otio_json().find("unknown.otio")!=std::string::npos,"unknown OTIO metadata was not preserved");}
void test_cinematic_inspector_edits(){
    EditorCameraSequencer editor;std::string error;require(editor.set_sequence(make(),&error),error.c_str());
    require(editor.apply_cinematic_preset_to_dolly_key(1U,0U,CameraCinematicPreset::VintageAnamorphic,&error),error.c_str());
    require(editor.sequence().dollies[0].keys[0].postProcess.cinematic.film.framing==CameraFramingPreset::Scope239,
            "cinematic preset edit was not applied");
    require(editor.apply_filmback_preset_to_dolly_key(1U,0U,CameraFilmbackPreset::Imax15Perf,65.0F,&error),error.c_str());
    const auto& physical=editor.sequence().dollies[0].keys[0].pose.lens.physical;
    require(physical.sensorWidthMillimeters>70.0F&&physical.focalLengthMillimeters==65.0F,
            "filmback preset edit was not applied");
    CameraPostProcessProfile invalid;invalid.cinematic.bokeh.bladeCount=2U;
    require(!editor.set_dolly_key_post_process(1U,0U,invalid,&error),
            "invalid cinematic inspector edit was accepted");
    require(editor.undo(),"cinematic inspector undo failed");
}

}
int main(){try{test();test_transactional_multi_edit();test_cinematic_inspector_edits();std::cout<<"dve_editor_camera_sequencer_tests: PASS\n";return 0;}catch(const std::exception&e){std::cerr<<"dve_editor_camera_sequencer_tests: FAIL: "<<e.what()<<'\n';return 1;}}
