#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dve/gameplay_runtime.hpp"
#include "dve/replication_contract.hpp"

namespace {
std::unique_ptr<dve::VoxelObject> box(int sx,int sy,int sz,dve::MaterialId material=1){auto v=std::make_unique<dve::VoxelObject>(material);for(int z=0;z<sz;++z)for(int y=0;y<sy;++y)for(int x=0;x<sx;++x)v->set_voxel({x,y,z},material);return v;}
dve::GameObjectId add_box(dve::GameWorld& world,std::string name,int sx,int sy,int sz,float scale,dve::Float3 position,bool dynamic=false){dve::GameObjectDesc d;d.name=std::move(name);d.voxelSizeMeters=scale;d.transform=dve::make_rigid_transform(position,{});d.voxels=box(sx,sy,sz);d.dynamic=dynamic;std::string error;const auto id=world.create_object(std::move(d),&error);if(id==dve::kInvalidGameObjectId)throw std::runtime_error(error);return id;}
}

int main(int argc,char** argv){
    try{
        const std::filesystem::path output=argc>1?argv[1]:"playable_runtime_v1_45_evidence.json";
        auto physics=std::make_unique<dve::ReferenceRigidBodyWorld>();physics->set_gravity({0,0,0});
        dve::GameWorld world(std::move(physics));
        const auto floor=add_box(world,"Destructible Floor",40,40,1,0.25F,{-5,-5,0},false);
        add_box(world,"Step",4,8,2,0.25F,{1.5F,-1,0.25F},false);
        dve::GameObjectDesc pawnDesc;pawnDesc.name="Player Pawn";pawnDesc.tags={"player"};pawnDesc.transform.position={0,0,0.25F};
        const auto pawn=world.create_object(std::move(pawnDesc));
        std::string error;if(!world.gameplay().add_character(pawn,{},&error))throw std::runtime_error(error);
        const auto player=world.gameplay().create_player("Local Player",true);if(!world.gameplay().possess(player,pawn,&error))throw std::runtime_error(error);
        dve::TriggerVolumeDesc checkpoint;checkpoint.name="Checkpoint";checkpoint.transform.position={2.25F,0,1};checkpoint.halfExtents={0.75F,1.5F,1.5F};checkpoint.requiredTag="player";
        const auto trigger=world.gameplay().create_trigger(checkpoint,&error);if(trigger==dve::kInvalidGameTriggerId)throw std::runtime_error(error);
        std::uint32_t enter{},stay{},exit{};world.gameplay().on_trigger([&](const dve::TriggerEvent& event){if(event.trigger!=trigger)return;if(event.kind==dve::TriggerEventKind::Enter)++enter;else if(event.kind==dve::TriggerEventKind::Stay)++stay;else ++exit;});
        world.gameplay().begin_recording(player);
        for(int tick=0;tick<150;++tick){dve::CharacterInput input;input.move={tick<100?1.0F:-0.5F,0,0};input.jumpPressed=tick==55;input.crouchHeld=tick>120;world.gameplay().set_player_input(player,input);world.tick(1.0F/60.0F);}
        const auto replay=world.gameplay().end_recording(player).value();
        const auto beforeRemoval=*world.gameplay().character(pawn);world.destroy_object(floor);world.tick(1.0F/60.0F);const auto afterRemoval=*world.gameplay().character(pawn);
        dve::GameplayReplicationFrame frame;frame.serverTick=world.gameplay().fixed_tick();frame.baselineTick=frame.serverTick-1;frame.characters.push_back({1,dve::quantize_position_millimeters(*world.position(pawn)),dve::quantize_velocity_centimeters(afterRemoval.velocity),static_cast<std::uint8_t>((afterRemoval.grounded?1U:0U)|(afterRemoval.stance==dve::CharacterStance::Crouched?2U:0U)),0});frame.triggers.push_back({2,world.gameplay().trigger(trigger)->enabled,world.gameplay().trigger_fired(trigger)});frame.destructionEdits.push_back({3,1,dve::quantize_position_millimeters({0,0,0}),1500,1,0x145ULL});const auto packet=dve::encode_gameplay_replication_frame(frame,&error);if(packet.empty())throw std::runtime_error(error);
        std::ofstream file(output);file<<"{\n  \"version\": \"1.45.0\",\n  \"fixed_ticks\": "<<world.gameplay().fixed_tick()<<",\n  \"pawn_position\": ["<<world.position(pawn)->x<<","<<world.position(pawn)->y<<","<<world.position(pawn)->z<<"],\n  \"trigger_enter\": "<<enter<<",\n  \"trigger_stay\": "<<stay<<",\n  \"trigger_exit\": "<<exit<<",\n  \"replay_frames\": "<<replay.frames.size()<<",\n  \"replay_hash\": "<<replay.stable_hash()<<",\n  \"floor_removed\": "<<(world.gameplay().telemetry(pawn)->floorRemoved?"true":"false")<<",\n  \"was_grounded_before_floor_removal\": "<<(beforeRemoval.grounded?"true":"false")<<",\n  \"grounded_after_floor_removal\": "<<(afterRemoval.grounded?"true":"false")<<",\n  \"replication_packet_bytes\": "<<packet.size()<<",\n  \"replication_hash\": "<<frame.stable_hash()<<"\n}\n";if(!file)throw std::runtime_error("could not write evidence");std::cout<<output<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"DVE v1.45 playable runtime demo failed: "<<e.what()<<'\n';return 1;}
}
