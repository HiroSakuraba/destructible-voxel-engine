#include "dve/audio/audio_clock.hpp"
#include "dve/audio/mixer.hpp"
#include "dve/camera_runtime.hpp"
#include "dve/camera_sequence.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/job_system.hpp"
#include "dve/player/player_input.hpp"
#include "dve/resumable_destruction.hpp"
#include "dve/runtime_replay.hpp"
#include "dve/simulation_clock.hpp"
#include "dve/world_tick_schedule.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace dve;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
GameObjectId box(GameWorld& world, int x, int y, int z, Float3 position = {}, float size = 0.1F) {
    GameObjectDesc desc;
    desc.name = "box";
    desc.dynamic = false;
    desc.transform.position = position;
    desc.voxelSizeMeters = size;
    desc.voxels = std::make_unique<VoxelObject>(71);
    for (int k = 0; k < z; ++k)
        for (int j = 0; j < y; ++j)
            for (int i = 0; i < x; ++i)
                desc.voxels->set_voxel({i, j, k}, 1);
    std::string error;
    const auto id = world.create_object(std::move(desc), &error);
    if (!id)
        throw std::runtime_error(error);
    return id;
}
std::unique_ptr<GameWorld> world() {
    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    physics->set_gravity({});
    return std::make_unique<GameWorld>(std::move(physics));
}
const VoxelObject* voxels(const GameWorld& world, GameObjectId id) {
    for (const auto& object : world.render_objects())
        if (object.id == id)
            return object.voxels;
    return nullptr;
}
void clocks() {
    for (const unsigned display : {60U, 120U, 144U, 165U, 240U}) {
        SimulationClock clock;
        std::uint64_t total{};
        std::uint64_t previous{};
        for (std::uint64_t i = 1; i <= display * 20; ++i) {
            const auto now = tick_deadline_nanoseconds(i, {display, 1});
            total += clock.advance_nanoseconds(now - previous);
            previous = now;
        }
        require(total == 1200, "display rate changed simulation count");
        require(clock.accumulator == 0, "integer clock retained rounding drift");
    }
    SimulationClock drop;
    require(drop.advance(0.5) == 8, "catch-up cap failed");
    require(drop.accumulator < drop.period_seconds(), "drop policy retained a full step");
    SimulationClock retain;
    retain.backlogPolicy = BacklogPolicy::Retain;
    require(retain.advance(0.25) == 8 && retain.advance(0) == 7, "retain policy lost debt");
    require(retain.alpha() <= 1.0F, "retained debt extrapolates rendering");
    RationalTimeScale half{1, 2};
    std::uint64_t scaled{};
    for (unsigned i = 0; i < 165; ++i)
        scaled += half.apply(1);
    require(scaled == 82 && half.remainder == 1, "rational speed lost remainder");
    require(valid_world_tick_schedule(), "world phase dependencies invalid");
}
void timers_and_presentation() {
    auto w = world();
    unsigned repeated{}, nested{};
    const auto timer = w->schedule_repeating(0.025F, [&] { ++repeated; });
    (void)w->schedule_once(0, [&] { (void)w->schedule_once(0, [&] { ++nested; }); });
    for (unsigned i = 0; i < 60; ++i)
        w->tick(1.0F / 60);
    require(repeated == 40 && nested == 1, "timer phase or callback snapshot changed");
    require(w->cancel_timer(timer), "timer cancellation failed");
    GameObjectDesc marker;
    const auto id = w->create_object(std::move(marker));
    w->on_tick([&](float) { (void)w->set_position(id, {10, 0, 0}); });
    w->tick(1.0F / 60);
    require(std::abs(w->presentation_transform(id, 0.5F)->position.x - 5) < 0.0001F,
            "script motion did not interpolate");
    const auto before = w->runtime_state_hash();
    w->set_frame_presentation(true);
    w->update_presentation(0.01F, 0.3F);
    require(w->runtime_state_hash() == before, "presentation mutated simulation");
    (void)w->set_position(id, {20, 0, 0});
    require(w->presentation_transform(id, 0)->position.x == 20,
            "external teleport retained history");
}
void camera_ticks() {
    auto w = world();
    w->set_frame_presentation(true);
    camera::CameraViewportDesc viewport;
    viewport.id = 1;
    viewport.name = "main";
    std::string error;
    require(w->cameras().add_viewport(viewport, &error), "camera viewport failed");
    camera::CameraSequence sequence;
    sequence.name = "Tick events";
    sequence.durationSeconds = 1;
    sequence.events = {{1, 0.025F, "impact", ""}};
    require(w->cameras().set_sequence(1, sequence, &error), "camera sequence failed");
    require(w->cameras().play_sequence(1), "sequence play failed");
    unsigned events{};
    w->on_tick([&](float) { events += unsigned(w->cameras().frame(1)->triggeredEvents.size()); });
    w->tick(1.0F / 60);
    const auto hash = w->cameras().sequence_state_hash();
    for (unsigned i = 0; i < 20; ++i)
        w->update_presentation(0.001F, float(i) / 20);
    require(hash == w->cameras().sequence_state_hash() && !events,
            "rendering advanced cinematic events");
    w->tick(1.0F / 60);
    require(events == 1, "cinematic event was not visible to tick listeners");
    w->cameras().clear_sequence(1);
    w->update_presentation(0);
    require(w->cameras().frame(1)->triggeredEvents.empty(), "cleared sequence retained old events");
}

void forces() {
    for (const float dt : {1.0F / 30, 1.0F / 60, 1.0F / 120}) {
        ReferenceRigidBodyWorld physics;
        physics.set_gravity({});
        RigidBodyCreateDesc desc;
        desc.massKilograms = 2;
        desc.inertiaKilogramMetersSquared = {2, 2, 2, 0, 0, 0};
        desc.boxes.push_back({{}, {0.5F, 0.5F, 0.5F}});
        const auto body = physics.create_body(desc);
        require(body != kInvalidRigidBodyHandle, "force fixture failed");
        require(physics.apply_force(body, {6, 0, 0}), "force rejected");
        require(physics.state(body)->linearVelocity.x == 0, "force was applied before the step");
        require(physics.apply_torque(body, {0, 0, 4}), "torque rejected");
        const auto hidden = physics.solver_state_hash();
        physics.step(dt);
        require(hidden != physics.solver_state_hash(),
                "pending loads missing from solver fingerprint");
        require(std::abs(physics.state(body)->linearVelocity.x - 3 * dt) < 1e-6F,
                "force retained hard-coded 60 Hz");
        require(std::abs(physics.state(body)->angularVelocity.z - 2 * dt) < 1e-6F,
                "torque retained hard-coded 60 Hz");
        physics.step(dt);
        require(std::abs(physics.state(body)->linearVelocity.x - 3 * dt) < 1e-6F,
                "force persisted another step");
    }
}
void pending_force_save() {
    auto original = world();
    GameObjectDesc desc;
    desc.name = "loaded body";
    desc.voxels = std::make_unique<VoxelObject>(71);
    desc.voxels->set_voxel({0, 0, 0}, 1);
    std::string error;
    const auto id = original->create_object(std::move(desc), &error);
    require(id != 0 && original->apply_force(id, {6, 0, 0}), "pending load fixture failed");
    GameSaveData save;
    save.world = original->capture_save_state();
    require(save.world.session && save.world.session->pendingLoads.contains(id),
            "pending load absent from world snapshot");
    GameSaveCodec codec;
    const auto encoded = codec.encode(save, nullptr, &error);
    require(bool(encoded), "pending load encode failed");
    const auto decoded = codec.decode(*encoded, nullptr, &error);
    require(decoded && decoded->world.session->pendingLoads.contains(id),
            "pending load decode failed");
    auto restored = world();
    require(restored->restore_save_state(decoded->world, nullptr, &error),
            "pending load restore failed");
    original->tick(1.0F / 60);
    restored->tick(1.0F / 60);
    require(original->runtime_state_hash() == restored->runtime_state_hash(),
            "pending force diverged after restore and physics step");
}
void input() {
    std::string error;
    auto bindings = player::InputBindingTable::parse({{"jump", "key:space"}}, &error);
    require(bool(bindings), "input binding failed");
    player::PlayerInput input(std::move(*bindings));
    platform::PlatformEvent event;
    event.key = "space";
    event.type = platform::EventType::KeyDown;
    event.timestampNanoseconds = 100;
    input.queue_event(event, 3);
    event.type = platform::EventType::KeyUp;
    input.queue_event(event, 3);
    event.type = platform::EventType::KeyDown;
    input.queue_event(event, 3);
    event.type = platform::EventType::KeyUp;
    input.queue_event(event, 3);
    input.begin_tick(2);
    require(!input.action("jump"), "future input ran early");
    input.begin_tick(3);
    require(input.action("jump") && input.press_count("jump") == 2, "short presses collapsed");
    const auto state = input.replay_state();
    require(state.find("delivered 4") != std::string::npos,
            "input checkpoint omitted delivered events");
    require(input.tick_events().size() == 4 &&
                input.tick_events()[0].sequence < input.tick_events()[1].sequence,
            "input ordering lost");
    input.end_tick();
    input.begin_tick(4);
    require(!input.action("jump"), "input edge replayed");
}
void root_motion() {
    auto w = world();
    box(*w, 40, 40, 1, {-5, -5, 0}, 0.25F);
    box(*w, 1, 12, 12, {2, -1.5F, 0.25F}, 0.25F);
    GameObjectDesc desc;
    desc.transform.position = {0, 0, 0.25F};
    const auto pawn = w->create_object(std::move(desc));
    require(w->gameplay().add_character(pawn), "character creation failed");
    require(w->gameplay().queue_root_motion(pawn, {5, 0, 0}), "root intent rejected");
    GameSaveData save;
    save.world = w->capture_save_state();
    save.runtimes = capture_game_runtime_state(*w);
    GameSaveCodec codec;
    std::string error;
    const auto bytes = codec.encode(save, nullptr, &error);
    require(bool(bytes), "root intent did not encode");
    const auto decoded = codec.decode(*bytes, nullptr, &error);
    require(decoded && decoded->runtimes.gameplay->pendingRootMotion.at(pawn).x == 5,
            "root intent did not survive save codec");
    require(w->gameplay().restore_save_state(*decoded->runtimes.gameplay, &error),
            "root intent restore failed");
    w->tick(1.0F / 60);
    require(w->position(pawn)->x < 1.7F && w->position(pawn)->x > 1.0F,
            "root motion tunneled through wall or did not move");
    require(w->gameplay().telemetry(pawn)->contacts > 0, "root motion bypassed controller sweep");
}
void destruction() {
    auto w = world();
    const auto id = box(*w, 24, 8, 8);
    const auto original = voxels(*w, id)->state_hash();
    unsigned commits{};
    std::uint64_t removed{};
    w->on_damage([&](const auto& event) {
        ++commits;
        removed = event.removedVoxelCount;
    });
    w->set_destruction_units_per_tick(64);
    require(bool(w->queue_damage_sphere(id, {1.2F, 0.4F, 0.4F}, 0.6F)),
            "damage queue rejected fixture");
    w->tick(1.0F / 60);
    require(voxels(*w, id)->state_hash() == original && !commits,
            "preparation changed live geometry");
    GameSaveData save;
    save.world = w->capture_save_state();
    GameSaveCodec codec;
    std::string error;
    const auto bytes = codec.encode(save, nullptr, &error);
    require(bool(bytes), "pending damage did not encode");
    const auto decoded = codec.decode(*bytes, nullptr, &error);
    require(bool(decoded), "pending damage did not decode");
    auto restored = world();
    require(restored->restore_save_state(decoded->world, nullptr, &error),
            "pending damage did not restore");
    unsigned restoredCommits{};
    restored->on_damage([&](const auto&) { ++restoredCommits; });
    for (unsigned i = 0; i < 200 && w->pending_destruction_count(); ++i) {
        w->tick(1.0F / 60);
        restored->tick(1.0F / 60);
    }
    require(!w->pending_destruction_count() && commits == 1 && removed > 0,
            "damage did not commit once");
    require(restoredCommits == 1 && restored->runtime_state_hash() == w->runtime_state_hash(),
            "resumed damage diverged after save");
    require(w->object_count() == 2, "beam fracture did not publish both pieces");
    const auto rejected = w->rejected_destruction_requests();
    require(bool(w->queue_damage_sphere(id, {0.2F, 0.2F, 0.2F}, 0.05F)), "stale fixture rejected");
    w->tick(1.0F / 60);
    (void)w->damage_sphere(id, {0.2F, 0.2F, 0.2F}, 0.1F);
    w->tick(1.0F / 60);
    require(w->rejected_destruction_requests() == rejected + 1, "stale geometry committed");
}
void replay() {
    auto a = world(), b = world();
    GameSaveCodec codec;
    const auto one = capture_runtime_replay_checkpoint(*a, codec),
               two = capture_runtime_replay_checkpoint(*b, codec);
    require(one && two && compare_runtime_replay_checkpoints(*one, *two).empty(),
            "equal checkpoints diverged");
    b->set_axis("test", 0.25F);
    const auto changed = capture_runtime_replay_checkpoint(*b, codec);
    require(changed && !compare_runtime_replay_checkpoints(*one, *changed).empty(),
            "world input missing from replay fingerprint");
}
void audio_clock() {
    audio::AudioClock clock;
    clock.publish({1000000000ULL, 48000, 7, 48000, 480});
    const auto target = clock.target(1020000000ULL, 48000, 7);
    require(target.frame == 48480 && !target.late && !target.stale, "audible target mapping wrong");
    require(clock.target(1, 48000, 7).late, "old timestamp underflowed");
    require(clock.target(1020000000ULL, 48000, 6).stale, "device restart accepted old generation");
    audio::AudioMixer mixer;
    mixer.publish_audio_clock({1000000000ULL, 0, 8, 48000, 0});
    audio::ResidentSampleDesc sample;
    sample.channels = 1;
    sample.samples.assign(64, 0.5F);
    const auto id = mixer.register_resident_sample(sample);
    audio::PlaySampleDesc play;
    play.sample = id;
    play.spatialized = false;
    play.audibleHostNanoseconds = 1005000000ULL;
    play.deviceGeneration = 8;
    require(bool(mixer.play_sample(play)), "timestamped sample rejected");
    std::vector<float> output(1024 * 2);
    mixer.render(output);
    for (unsigned i = 0; i < 240; ++i)
        require(output[i * 2] == 0, "audio started before timestamp");
    require(std::abs(output[240 * 2]) > 0.01F, "timestamped click was silent");
}
void music_clock() {
    audio::Synthesizer synth;
    audio::SynthPreset preset;
    preset.arpeggiator.enabled = true;
    preset.arpeggiator.clockSource = audio::ArpeggiatorClockSource::GameClock;
    preset.arpeggiator.division = audio::ArpeggiatorDivision::Sixteenth;
    synth.set_preset(preset);
    require(synth.set_game_music_clock({0, 0, 123, true, 1}), "music anchor rejected");
    require(synth.note_on(60, 0.8F), "music note rejected");
    const auto deadline = static_cast<std::size_t>(std::ceil(37 * 0.25 * 60 * 48000 / 123.0));
    std::vector<float> output((deadline + 1) * 2);
    synth.render(output.data(), deadline + 1);
    require(synth.profiler().arpSteps == 38, "musical deadlines accumulated rounding drift");
    const auto steps = synth.profiler().arpSteps;
    require(synth.set_game_music_clock({synth.current_frame(), 9.25, 123, false, 1}),
            "pause anchor rejected");
    synth.render(output.data(), 5000);
    require(synth.profiler().arpSteps == steps, "paused music kept advancing");
    require(synth.set_game_music_clock({synth.current_frame(), 9.25, 123, true, 2}),
            "resume anchor rejected");
    synth.render(output.data(), 1);
    require(synth.profiler().arpSteps == steps + 1, "music resume lost beat phase");
}

void jobs() {
    JobSystem pool(2);
    std::atomic<unsigned> count{};
    pool.parallel_for(5, [&](std::size_t) { pool.parallel_for(3, [&](std::size_t) { ++count; }); });
    require(count == 15, "nested jobs deadlocked or lost work");
    std::thread other([&] { pool.parallel_for(20, [&](std::size_t) { ++count; }); });
    pool.parallel_for(20, [&](std::size_t) { ++count; });
    other.join();
    require(count == 55, "concurrent coordinators corrupted a batch");
    bool caught{};
    try {
        pool.parallel_for(5, [](std::size_t i) {
            if (i == 2)
                throw std::runtime_error("expected");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    require(caught, "worker exception was not propagated");
}
} // namespace
int main() {
    try {
        clocks();
        timers_and_presentation();
        camera_ticks();
        forces();
        pending_force_save();
        input();
        root_motion();
        destruction();
        replay();
        audio_clock();
        music_clock();
        jobs();
        std::cout << "Clockwork integration: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
