// Save games (dve/game_save.hpp): GameWorld capture/restore round trips, destruction deltas,
// the DVESAVE1 container's corruption handling, schema migrations and (with Lua) script state.
#include "dve/camera_runtime.hpp"
#include "dve/dvox.hpp"
#include "dve/game_save.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/version.hpp"
#if defined(DVE_HAVE_LUA)
#include "dve/game_script.hpp"
#endif

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(...)                                                                        \
    do {                                                                                  \
        if (!(__VA_ARGS__)) {                                                             \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

using namespace dve;

constexpr float kDt = 1.0F / 60.0F;

std::filesystem::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("dve_game_save_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

VoxelMaterialDefinition material(const char* name, float density, Float4 color) {
    VoxelMaterialDefinition m;
    m.name = name;
    m.densityKilogramsPerCubicMeter = density;
    m.baseColor = color;
    return m;
}

// A dumbbell: two 4x4x4 blocks joined by a 1x2x1 neck at (1, 4..5, 1). Blasting the neck
// splits it in two, so a static tower drops its top half as a dynamic fragment.
void write_tower(const std::filesystem::path& path) {
    CookedVoxelAsset asset(4001);
    asset.voxelSizeMeters = 0.25F;
    asset.materials.push_back(material("Air", 0.0F, {0, 0, 0, 0}));
    asset.materials.push_back(material("Stone", 2400.0F, {0.5F, 0.5F, 0.5F, 1}));
    asset.materials.push_back(material("Brick", 1900.0F, {0.7F, 0.3F, 0.2F, 1}));
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) {
            for (int y = 0; y < 4; ++y) asset.object.set_voxel({x, y, z}, 1);
            for (int y = 6; y < 10; ++y) asset.object.set_voxel({x, y, z}, 2);
        }
    asset.object.set_voxel({1, 4, 1}, 1);
    asset.object.set_voxel({1, 5, 1}, 1);
    std::string error;
    if (!write_dvox(path, asset, {}, &error)) { std::cerr << "write_dvox: " << error << '\n'; ++failures; }
}

void write_crate(const std::filesystem::path& path) {
    CookedVoxelAsset asset(4002);
    asset.voxelSizeMeters = 0.25F;
    asset.materials.push_back(material("Air", 0.0F, {0, 0, 0, 0}));
    asset.materials.push_back(material("Wood", 600.0F, {0.6F, 0.4F, 0.2F, 1}));
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z) asset.object.set_voxel({x, y, z}, 1);
    std::string error;
    if (!write_dvox(path, asset, {}, &error)) { std::cerr << "write_dvox: " << error << '\n'; ++failures; }
}

struct Assets {
    std::filesystem::path dir;
    std::filesystem::path tower;
    std::filesystem::path crate;
};

Assets make_assets() {
    Assets assets;
    assets.dir = make_temp_dir();
    assets.tower = assets.dir / "tower.dvox";
    assets.crate = assets.dir / "crate.dvox";
    write_tower(assets.tower);
    write_crate(assets.crate);
    return assets;
}

GameSaveSourceReader file_reader() {
    return [](std::string_view path, std::uint64_t maximumBytes, std::string* error) -> std::optional<std::vector<std::byte>> {
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (ec) { if (error) *error = "cannot open " + std::string(path); return std::nullopt; }
        if (size > maximumBytes) { if (error) *error = "too large"; return std::nullopt; }
        return read_bytes(std::filesystem::path(path));
    };
}

std::unique_ptr<IRigidBodyWorld> make_physics(Physics3DBackend backend) {
    std::string error;
    auto physics = create_physics3d_world(backend, nullptr, &error);
    if (!physics) { std::cerr << "physics: " << error << '\n'; ++failures; }
    return physics;
}

GameObjectId spawn_with_source(GameWorld& world, const std::filesystem::path& path, const char* name, Float3 at, bool dynamic) {
    std::string error;
    const GameObjectId id = world.spawn_asset(path, name, make_rigid_transform(at, {}), dynamic, true, &error);
    if (id == kInvalidGameObjectId) { std::cerr << "spawn " << name << ": " << error << '\n'; ++failures; return id; }
    CHECK(world.set_object_source(id, {path.string(), game_save_content_hash(read_bytes(path)), false}));
    return id;
}

// The "boot" every test uses, like PlayerApp loading a scene: identical in both processes.
struct Scene {
    std::unique_ptr<GameWorld> world;
    GameObjectId ground{}, tower{}, crate{}, marker{}, runtimeBox{};
    GameObjectPoolId pool{};
    int* timerFires{};
};

Scene boot(const Assets& assets, Physics3DBackend backend, int* timerFires) {
    Scene scene;
    scene.world = std::make_unique<GameWorld>(make_physics(backend));
    GameWorld& world = *scene.world;
    std::string error;
    {
        GameObjectDesc ground;
        ground.name = "Ground";
        ground.tags = {"floor"};
        ground.voxelSizeMeters = 0.5F;
        ground.dynamic = false;
        ground.transform = make_rigid_transform({-4.0F, -0.5F, -4.0F}, {});
        ground.voxels = std::make_unique<VoxelObject>(9001);
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) ground.voxels->set_voxel({x, 0, z}, 1);
        scene.ground = world.create_object(std::move(ground), &error);
        CHECK(scene.ground != kInvalidGameObjectId);
    }
    scene.tower = spawn_with_source(world, assets.tower, "Tower", {0.0F, 0.0F, 0.0F}, false);
    scene.crate = spawn_with_source(world, assets.crate, "Crate", {2.0F, 1.5F, 0.3F}, true);
    {
        GameObjectDesc marker;
        marker.name = "Spawn";
        marker.groups = {"points"};
        marker.layer = 3;
        marker.transform = make_rigid_transform({1.0F, 2.0F, 3.0F}, {});
        scene.marker = world.create_object(std::move(marker), &error);
    }
    {
        GameObjectDesc box;
        box.name = "RuntimeBox";
        box.voxelSizeMeters = 0.2F;
        box.transform = make_rigid_transform({-2.0F, 2.5F, 1.0F}, {});
        box.voxels = std::make_unique<VoxelObject>(9002);
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 3; ++z) box.voxels->set_voxel({x, y, z}, 3);
        scene.runtimeBox = world.create_object(std::move(box), &error);
        CHECK(scene.runtimeBox != kInvalidGameObjectId);
    }
    {
        GameObjectPoolDesc pool;
        pool.name = "Markers";
        pool.capacity = 3;
        pool.prototype.name = "Pooled";
        scene.pool = world.register_pool(std::move(pool), &error);
        CHECK(scene.pool != kInvalidGameObjectPoolId);
    }
    scene.timerFires = timerFires;
    (void)world.schedule_repeating(0.25F, [timerFires]() { ++*timerFires; });
    return scene;
}

void play(Scene& scene, int ticks) { for (int i = 0; i < ticks; ++i) scene.world->tick(kDt); }

GameSaveData capture(const Scene& scene) {
    GameSaveData data;
    data.metadata.gameName = "test";
    data.metadata.scenePath = "scenes/test.dscene";
    data.metadata.worldStateHash = scene.world->state_hash();
    data.world = scene.world->capture_save_state();
    return data;
}

bool restore(Scene& scene, const GameSaveData& data, GameWorldRestoreReport* report = nullptr) {
    std::string error;
    const bool ok = scene.world->restore_save_state(data.world, report, &error);
    if (!ok) std::cerr << "restore: " << error << '\n';
    return ok;
}

void test_round_trip(Physics3DBackend backend, const char* label) {
    const Assets assets = make_assets();
    GameSaveCodec codec(file_reader());
    int firesA = 0, firesB = 0;
    Scene a = boot(assets, backend, &firesA);
    play(a, 20);
    CHECK(a.world->acquire_from_pool(a.pool, make_rigid_transform({0, 3, 0}, {})) != kInvalidGameObjectId);
    CHECK(a.world->set_position(a.marker, {7.0F, 8.0F, 9.0F}));
    const GameSaveData saved = capture(a);
    GameSaveStats stats;
    std::string error;
    const auto bytes = codec.encode(saved, &stats, &error);
    CHECK(bytes.has_value());
    if (!bytes) { std::cerr << label << " encode: " << error << '\n'; return; }
    CHECK(stats.deltaObjects == 2U);   // tower + crate: untouched, so only a header
    CHECK(stats.fullObjects == 2U);    // ground and the runtime box: no source asset
    const auto decoded = codec.decode(*bytes, nullptr, &error);
    CHECK(decoded.has_value());
    if (!decoded) { std::cerr << label << " decode: " << error << '\n'; return; }
    CHECK(decoded->metadata.worldStateHash == a.world->state_hash());

    Scene b = boot(assets, backend, &firesB);   // a fresh "process": boot, then load
    GameWorldRestoreReport report;
    CHECK(restore(b, *decoded, &report));
    CHECK(b.world->state_hash() == a.world->state_hash());
    CHECK(report.timersRestored == 1U && report.timersDropped == 0U && report.poolsRestored == 1U);
    // Re-capturing gives byte-identical saves.
    GameSaveData again = capture(b);
    const auto bytesAgain = codec.encode(again, nullptr, &error);
    CHECK(bytesAgain && *bytesAgain == *bytes);
    CHECK(b.world->position(b.marker) && b.world->position(b.marker)->x == 7.0F);
    CHECK(b.world->pool_available(b.pool) == a.world->pool_available(a.pool));

    // Continuing both worlds keeps them in lock step (timers fire at the same ticks, bodies
    // resume with the saved velocities).
    const int firesBeforeA = firesA;
    play(a, 30);
    play(b, 30);
    CHECK(firesA - firesBeforeA == firesB);
    if (backend == Physics3DBackend::Reference) CHECK(b.world->state_hash() == a.world->state_hash());
    else if (b.world->state_hash() != a.world->state_hash())
        std::cerr << label << ": note: continuation diverged after load (solver caches are not saved)\n";
    std::filesystem::remove_all(assets.dir);
}

void test_destruction(Physics3DBackend backend, const char* label) {
    const Assets assets = make_assets();
    GameSaveCodec codec(file_reader());
    int firesA = 0, firesB = 0;
    Scene a = boot(assets, backend, &firesA);
    play(a, 10);
    const std::size_t before = a.world->object_count();
    // Blast the tower's neck (voxels (1,4..5,1), 0.25 m voxels) and chip the crate.
    const auto removed = a.world->damage_sphere(a.tower, {0.375F, 1.25F, 0.375F}, 0.3F);
    CHECK(removed && *removed >= 2U);
    CHECK(a.world->object_count() == before + 1U);   // the top half broke off
    const auto cratePos = a.world->position(a.crate);
    if (cratePos) (void)a.world->damage_sphere(a.crate, {cratePos->x, cratePos->y, cratePos->z}, 0.2F);
    play(a, 25);

    const GameSaveData saved = capture(a);
    GameSaveStats stats;
    std::string error;
    const auto bytes = codec.encode(saved, &stats, &error);
    CHECK(bytes.has_value());
    if (!bytes) { std::cerr << label << " encode: " << error << '\n'; return; }
    CHECK(stats.deltaObjects == 2U);   // tower and crate: a delta against their asset
    CHECK(stats.deltaBricks >= 1U);
    CHECK(stats.fullObjects >= 2U);    // runtime box + the fragment (derived: stored in full)
    std::cerr << label << ": destruction save " << stats.fileBytes << " bytes, " << stats.deltaObjects
              << " delta / " << stats.fullObjects << " full objects\n";

    Scene b = boot(assets, backend, &firesB);
    const auto decoded = codec.decode(*bytes, nullptr, &error);
    CHECK(decoded.has_value());
    if (!decoded) { std::cerr << label << " decode: " << error << '\n'; return; }
    CHECK(restore(b, *decoded));
    CHECK(b.world->object_count() == a.world->object_count());
    CHECK(b.world->state_hash() == a.world->state_hash());
    CHECK(b.world->voxel_count(b.tower) == a.world->voxel_count(a.tower));
    const auto bytesAgain = codec.encode(capture(b), nullptr, &error);
    CHECK(bytesAgain && *bytesAgain == *bytes);

    // Destruction keeps working after a load, identically in both worlds.
    const auto hitA = a.world->damage_sphere(a.tower, {0.375F, 0.5F, 0.375F}, 0.3F);
    const auto hitB = b.world->damage_sphere(b.tower, {0.375F, 0.5F, 0.375F}, 0.3F);
    CHECK(hitA == hitB);
    play(a, 5);
    play(b, 5);
    CHECK(a.world->object_count() == b.world->object_count());
    if (backend == Physics3DBackend::Reference) CHECK(b.world->state_hash() == a.world->state_hash());

    // A load also works into a world that has diverged (more fragments, fewer objects).
    Scene c = boot(assets, backend, &firesB);
    (void)c.world->damage_sphere(c.tower, {0.5F, 2.0F, 0.5F}, 0.6F);
    CHECK(c.world->destroy_object(c.marker));
    play(c, 3);
    CHECK(restore(c, *decoded));
    CHECK(c.world->state_hash() == decoded->metadata.worldStateHash);

    // Changing a source asset after saving is detected instead of loading garbage.
    CookedVoxelAsset edited(4001);
    edited.voxelSizeMeters = 0.25F;
    edited.materials.push_back(material("Air", 0.0F, {0, 0, 0, 0}));
    edited.materials.push_back(material("Stone", 2400.0F, {0.5F, 0.5F, 0.5F, 1}));
    edited.object.set_voxel({0, 0, 0}, 1);
    CHECK(write_dvox(assets.tower, edited, {}, &error));
    error.clear();
    CHECK(!codec.decode(*bytes, nullptr, &error).has_value());
    CHECK(error.find("changed") != std::string::npos);
    std::filesystem::remove_all(assets.dir);
}

void test_corruption() {
    const Assets assets = make_assets();
    GameSaveCodec codec(file_reader());
    int fires = 0;
    Scene a = boot(assets, Physics3DBackend::Reference, &fires);
    (void)a.world->damage_sphere(a.tower, {0.375F, 1.25F, 0.375F}, 0.3F);
    play(a, 5);
    std::string error;
    const auto bytes = codec.encode(capture(a), nullptr, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;

    // Every truncation fails with a message (never a crash or a huge allocation).
    std::size_t truncationFailures = 0, truncations = 0;
    const std::size_t stride = std::max<std::size_t>(1U, bytes->size() / 997U);
    for (std::size_t length = 0; length < bytes->size(); length += (length < 64U ? 1U : stride)) {
        ++truncations;
        std::string message;
        if (!codec.decode(std::span(bytes->data(), length), nullptr, &message) && !message.empty()) ++truncationFailures;
    }
    CHECK(truncationFailures == truncations);

    // Flipped bytes are caught by the magic, the version check or a section/document hash.
    // (Only the informational u64 sequence at offset 12..19 is outside the hashes.)
    for (std::size_t offset : {std::size_t{3}, std::size_t{8}, std::size_t{24}, std::size_t{40}, bytes->size() / 3U,
                               bytes->size() / 2U, bytes->size() - 1U}) {
        auto corrupt = *bytes;
        corrupt[offset] ^= std::byte{0x5A};
        std::string message;
        CHECK(!codec.decode(corrupt, nullptr, &message).has_value());
        CHECK(!message.empty());
    }
    // Trailing garbage is rejected too.
    {
        auto padded = *bytes;
        padded.push_back(std::byte{0});
        std::string message;
        CHECK(!codec.decode(padded, nullptr, &message).has_value());
        CHECK(message.find("trailing") != std::string::npos);
    }
    // A huge declared section size in a tiny file fails before allocating.
    {
        std::vector<std::byte> hostile(bytes->begin(), bytes->begin() + 32);
        SaveGameDocument doc;
        doc.schemaVersion = kGameSaveSchemaVersion;
        doc.sections["x"] = std::vector<std::byte>(4);
        hostile = encode_save_game_document(doc);
        // Section header: after the 32-byte file header, u32 nameBytes then u64 payloadBytes.
        for (int i = 0; i < 8; ++i) hostile[36 + static_cast<std::size_t>(i)] = std::byte{0xFF};
        std::string message;
        CHECK(!decode_save_game_document(hostile, {}, &message).has_value());
        CHECK(!message.empty());
    }

    // On disk: a corrupted slot falls back to the previous publish (.bak) and says so.
    const auto slot = assets.dir / "saves" / "slot1.dvesave";
    GameSaveStats first, second;
    CHECK(codec.write_file(slot, capture(a), &first, &error));
    play(a, 5);
    const GameSaveData latest = capture(a);
    CHECK(codec.write_file(slot, latest, &second, &error));
    CHECK(std::filesystem::exists(assets.dir / "saves" / "slot1.dvesave.bak"));
    CHECK(second.fileBytes == std::filesystem::file_size(slot));
    {
        SaveGameReadReport report;
        const auto loaded = codec.read_file(slot, {}, &report, &error);
        CHECK(loaded && report.source == SaveGameReadSource::Primary);
        CHECK(loaded && loaded->metadata.worldStateHash == latest.metadata.worldStateHash);
    }
    auto onDisk = read_bytes(slot);
    onDisk.resize(onDisk.size() / 2U);
    write_bytes(slot, onDisk);
    {
        SaveGameReadReport report;
        std::string message;
        const auto loaded = codec.read_file(slot, {}, &report, &message);
        CHECK(loaded.has_value());
        CHECK(report.source == SaveGameReadSource::Backup);
        CHECK(!report.primaryError.empty());
        SaveGameReadOptions strict;
        strict.allowBackup = false;
        message.clear();
        CHECK(!codec.read_file(slot, strict, nullptr, &message).has_value());
        CHECK(!message.empty());
    }
    std::filesystem::remove(assets.dir / "saves" / "slot1.dvesave.bak");
    {
        std::string message;
        CHECK(!codec.read_file(slot, {}, nullptr, &message).has_value());
        CHECK(!message.empty());
        message.clear();
        CHECK(!codec.read_file(assets.dir / "saves" / "missing.dvesave", {}, nullptr, &message).has_value());
        CHECK(!message.empty());
    }
    // Not a save at all.
    {
        const std::string text = "hello, this is not a save";
        std::string message;
        CHECK(!codec.decode(std::as_bytes(std::span(text.data(), text.size())), nullptr, &message).has_value());
        CHECK(!message.empty());
    }
    std::filesystem::remove_all(assets.dir);
}

// A synthetic v0 schema: the same payloads under the pre-release section names "meta",
// "world", "voxels", "physics" (no "dve." prefix) and a "format" marker section. A registered
// 0 -> 1 migration renames them; without it the load fails with a clear message, and a save
// from a newer engine is refused.
void test_migration() {
    const Assets assets = make_assets();
    int fires = 0;
    Scene a = boot(assets, Physics3DBackend::Reference, &fires);
    (void)a.world->damage_sphere(a.tower, {0.375F, 1.25F, 0.375F}, 0.3F);
    play(a, 8);
    const GameSaveData saved = capture(a);
    std::string error;

    GameSaveCodec plain(file_reader());
    auto document = plain.to_document(saved, nullptr, &error);
    CHECK(document.has_value());
    if (!document) return;
    document->sections.erase("dve.clock");
    SaveGameDocument v0;
    v0.schemaVersion = 0;
    v0.sequence = 7;
    for (const auto& [name, payload] : document->sections) {
        CHECK(name.rfind("dve.", 0) == 0);
        v0.sections[name.substr(4)] = payload;
    }
    const std::string marker = "v0";
    v0.sections["format"] = std::vector<std::byte>(reinterpret_cast<const std::byte*>(marker.data()),
                                                   reinterpret_cast<const std::byte*>(marker.data()) + marker.size());
    const auto v0Bytes = encode_save_game_document(v0);

    // No migration registered: refused, naming the version.
    CHECK(!plain.decode(v0Bytes, nullptr, &error).has_value());
    CHECK(error.find('0') != std::string::npos);

    GameSaveCodec migrating(file_reader());
    CHECK(migrating.register_migration(0, [](SaveGameDocument& doc, std::string* message) {
        if (!doc.sections.contains("format")) { if (message) *message = "not a v0 save"; return false; }
        std::map<std::string, std::vector<std::byte>, std::less<>> renamed;
        for (auto& [name, payload] : doc.sections)
            if (name != "format") renamed.emplace("dve." + name, std::move(payload));
        doc.sections = std::move(renamed);
        doc.schemaVersion = 1;
        return true;
    }, &error));
    CHECK(!migrating.register_migration(0, [](SaveGameDocument&, std::string*) { return true; }, &error));   // duplicate
    SaveGameReadReport report;
    const auto migrated = migrating.decode(v0Bytes, &report, &error);
    CHECK(migrated.has_value());
    if (!migrated) std::cerr << "migration: " << error << '\n';
    CHECK(report.storedSchemaVersion == 0U && report.migrationsApplied == 3U);   // game v0->1, engine v1->2->3
    int firesB = 0;
    Scene b = boot(assets, Physics3DBackend::Reference, &firesB);
    if (migrated) CHECK(restore(b, *migrated));
    CHECK(b.world->state_hash() == a.world->state_hash());

    // Current-version saves skip migrations.
    const auto v1Bytes = migrating.encode(saved, nullptr, &error);
    CHECK(v1Bytes && migrating.decode(*v1Bytes, &report, &error) && report.migrationsApplied == 0U);

    // A failing migration reports its own message.
    GameSaveCodec failing(file_reader());
    CHECK(failing.register_migration(0, [](SaveGameDocument&, std::string* message) {
        if (message) *message = "synthetic failure";
        return false;
    }, &error));
    CHECK(!failing.decode(v0Bytes, nullptr, &error).has_value());
    CHECK(error.find("synthetic failure") != std::string::npos);

    // A migration that does not advance the version is rejected rather than looping.
    GameSaveCodec stuck(file_reader());
    CHECK(stuck.register_migration(0, [](SaveGameDocument&, std::string*) { return true; }, &error));
    CHECK(!stuck.decode(v0Bytes, nullptr, &error).has_value());

    // Newer than this engine: refused.
    SaveGameDocument future = *document;
    future.schemaVersion = kGameSaveSchemaVersion + 1U;
    CHECK(!plain.decode(encode_save_game_document(future), nullptr, &error).has_value());
    CHECK(error.find("newer") != std::string::npos);
    std::filesystem::remove_all(assets.dir);
}

void test_game_sections_and_limits() {
    const Assets assets = make_assets();
    int fires = 0;
    Scene a = boot(assets, Physics3DBackend::Reference, &fires);
    GameSaveCodec codec(file_reader());
    GameSaveData data = capture(a);
    data.metadata.info["slot"] = "Quick save";
    data.scriptState = std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}};
    data.gameSections["mygame.inventory"] = std::vector<std::byte>(100, std::byte{7});
    std::string error;
    auto bytes = codec.encode(data, nullptr, &error);
    CHECK(bytes.has_value());
    const auto decoded = bytes ? codec.decode(*bytes, nullptr, &error) : std::nullopt;
    CHECK(decoded && decoded->metadata.info.at("slot") == "Quick save");
    CHECK(decoded && decoded->scriptState && decoded->scriptState->size() == 3U);
    CHECK(decoded && decoded->gameSections.at("mygame.inventory").size() == 100U);
    CHECK(decoded && decoded->metadata.engineVersion == std::string(kVersionString));

    data.gameSections["dve.sneaky"] = {};
    CHECK(!codec.encode(data, nullptr, &error).has_value());
    data.gameSections.erase("dve.sneaky");

    GameSaveLimits tight;
    tight.maximumScriptStateBytes = 2;
    GameSaveCodec small(file_reader(), tight);
    CHECK(!small.encode(data, nullptr, &error).has_value());
    CHECK(error.find("script") != std::string::npos);
    GameSaveLimits fewObjects;
    fewObjects.maximumObjects = 2;
    GameSaveCodec tiny(file_reader(), fewObjects);
    CHECK(!tiny.encode(data, nullptr, &error).has_value());
    // A save written under the default limits is refused by a reader with tighter ones.
    CHECK(bytes && !tiny.decode(*bytes, nullptr, &error).has_value());

    // Restoring a save into a world whose sources cannot be read fails and changes nothing.
    GameSaveCodec blind([](std::string_view, std::uint64_t, std::string* message) -> std::optional<std::vector<std::byte>> {
        if (message) *message = "no content";
        return std::nullopt;
    });
    CHECK(bytes && !blind.decode(*bytes, nullptr, &error).has_value());
    std::filesystem::remove_all(assets.dir);
}


// --- v2: sub-runtimes (characters, cameras, animation, ragdolls, hair) ----------------------

SkeletonAsset actor_skeleton() {
    SkeletonAsset skeleton;
    skeleton.name = "Save Actor";
    skeleton.bones = {
        {"pelvis", -1, make_rigid_transform({}, {})},
        {"spine", 0, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},
        {"head", 1, make_rigid_transform({0.0F, 1.0F, 0.0F}, {})},
    };
    return skeleton;
}

AnimationClipAsset actor_clip(const char* name, float lift) {
    AnimationClipAsset clip;
    clip.name = name;
    clip.durationSeconds = 1.0F;
    clip.looping = true;
    BoneAnimationTrack spine;
    spine.bone = 1U;
    spine.translations = {{0.0F, {0.0F, 1.0F, 0.0F}}, {1.0F, {0.0F, 1.0F + lift, 0.0F}}};
    spine.rotations = {{0.0F, {}}, {1.0F, quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, lift)}};
    clip.tracks.push_back(std::move(spine));
    return clip;
}

AnimationControllerAsset actor_controller() {
    AnimationControllerAsset controller;
    controller.name = "Locomotion";
    controller.initialState = "idle";
    controller.parameters.emplace("speed", 0.0);
    controller.parameters.emplace("armed", false);
    controller.parameters.emplace("stance", std::int64_t{0});
    controller.states = {{"idle", "Idle", 1.0F, false}, {"walk", "Walk", 1.25F, false}};
    controller.transitions = {
        {"idle", "walk", 0.2F, 0.0F, 10, {{"speed", AnimationConditionOperator::Greater, 0.5}}},
        {"walk", "idle", 0.2F, 0.0F, 10, {{"speed", AnimationConditionOperator::LessOrEqual, 0.5}}},
    };
    return controller;
}

RagdollDefinition actor_ragdoll() {
    RagdollDefinition ragdoll;
    ragdoll.name = "Save Ragdoll";
    ragdoll.bodies = {{0U}, {1U}, {2U}};
    ragdoll.joints = {{0U, 1U, RagdollJointKind::ConeTwist}, {1U, 2U, RagdollJointKind::Fixed}};
    return ragdoll;
}

ControlRigAsset actor_rig() {
    ControlRigAsset rig;
    rig.name = "Head Aim";
    ControlRigControl control;
    control.id = 1U;
    control.name = "Head";
    control.defaultLocal = make_rigid_transform({0.0F, 2.0F, 0.0F}, {});
    rig.controls = {control};
    ControlRigNode node;
    node.id = 1U;
    node.name = "Set Head";
    node.kind = ControlRigNodeKind::SetBoneTransform;
    node.bone = 2U;
    node.targetControl = 1U;
    rig.nodes = {node};
    return rig;
}

struct RuntimeIds {
    GameObjectId pawn{}, actor{}, rigged{}, hairy{};
    GamePlayerId player{};
    GameTriggerId trigger{};
};

GameObjectId add_marker(GameWorld& world, const char* name, Float3 at) {
    GameObjectDesc desc;
    desc.name = name;
    desc.transform = make_rigid_transform(at, {});
    std::string error;
    const GameObjectId id = world.create_object(std::move(desc), &error);
    CHECK(id != kInvalidGameObjectId);
    return id;
}

// What a game's startup code binds, identical in the saving and the loading process.
RuntimeIds boot_runtimes(GameWorld& world, bool withRagdoll = true) {
    RuntimeIds ids;
    std::string error;
    ids.pawn = add_marker(world, "Pawn", {20.0F, 20.0F, 5.0F});
    ids.actor = add_marker(world, "Actor", {-20.0F, 0.0F, -20.0F});
    ids.rigged = add_marker(world, "Rigged", {-25.0F, 0.0F, -20.0F});
    ids.hairy = add_marker(world, "Hairy", {-30.0F, 2.0F, -20.0F});
    CHECK(world.gameplay().add_character(ids.pawn, {}, &error));
    ids.player = world.gameplay().create_player("Local", true);
    CHECK(world.gameplay().possess(ids.player, ids.pawn, &error));
    TriggerVolumeDesc trigger;
    trigger.name = "Checkpoint";
    trigger.transform.position = {20.0F, 20.0F, 3.0F};
    trigger.halfExtents = {2.0F, 2.0F, 2.0F};
    ids.trigger = world.gameplay().create_trigger(trigger, &error);
    CHECK(ids.trigger != kInvalidGameTriggerId);
    for (const GameObjectId id : {ids.actor, ids.rigged}) {
        CHECK(world.animation().bind_skeleton(id, actor_skeleton(), &error));
        CHECK(world.animation().add_clip(id, actor_clip("Idle", 0.1F), &error));
        CHECK(world.animation().add_clip(id, actor_clip("Walk", 0.6F), &error));
    }
    CHECK(world.animation_controllers().bind(ids.actor, actor_controller(), &error));
    CHECK(world.control_rigs().bind(ids.rigged, actor_rig(), &error));
    if (!world.control_rigs().has_instance(ids.rigged)) std::cerr << "control rig: " << error << '\n';
    RagdollRuntimeConfig ragdollConfig;
    ragdollConfig.blendInSeconds = 0.2F;
    if (withRagdoll) CHECK(world.bind_ragdoll(ids.actor, actor_ragdoll(), ragdollConfig, &error));
#if defined(DVE_ENABLE_CPU_HAIR)
    CpuHairBindOptions hair;
    hair.simulation.solver.enableSleeping = false;
    CHECK(world.bind_cpu_hair(ids.hairy, make_straight_hair_groom(4U, 6U, 0.02F, 0.03F), hair, &error));
#endif
    camera::CameraViewportDesc viewport{1, "Main", 1, 0, 0, 1, 1, "main", true};
    CHECK(world.cameras().add_viewport(viewport, &error));
    camera::CameraRig a;
    a.id = 11; a.name = "Wide"; a.priority = 10; a.collision.enabled = false;
    a.authoredPose.position = {0, 3, 10}; a.authoredPose.target = {0, 1, 0};
    camera::CameraRig b = a;
    b.id = 12; b.name = "Close"; b.priority = 1; b.authoredPose.position = {2, 2, 4};
    auto* director = world.cameras().director(1);
    CHECK(director && director->add_or_replace_rig(a, &error) && director->add_or_replace_rig(b, &error));
    if (director) director->bind_state("close", 12);
    if (!error.empty()) std::cerr << "boot_runtimes: " << error << '\n';
    return ids;
}

GameSaveData capture_all(const Scene& scene) {
    GameSaveData data = capture(scene);
    data.runtimes = capture_game_runtime_state(*scene.world);
    return data;
}

std::vector<std::byte> section_of(const GameSaveCodec& codec, const GameSaveData& data, std::string_view name) {
    std::string error;
    const auto document = codec.to_document(data, nullptr, &error);
    if (!document) return {};
    const auto it = document->sections.find(name);
    return it == document->sections.end() ? std::vector<std::byte>{} : it->second;
}

void test_runtime_round_trip() {
    const Assets assets = make_assets();
    GameSaveCodec codec(file_reader());
    std::string error;
    int firesA = 0, firesB = 0;
    Scene a = boot(assets, Physics3DBackend::Reference, &firesA);
    const RuntimeIds ia = boot_runtimes(*a.world);
    GameWorld& wa = *a.world;

    // Play: move the character, add a runtime trigger, drive animation, ragdoll, hair, camera.
    CHECK(wa.gameplay().set_player_input(ia.player, {{1.0F, 0.5F, 0.0F}, true, false}));
    TriggerVolumeDesc late;
    late.name = "Late";
    late.shape = TriggerShape::Sphere;
    late.radiusMeters = 3.0F;
    late.transform.position = {21.0F, 20.0F, 4.0F};
    CHECK(wa.gameplay().create_trigger(late, &error) != kInvalidGameTriggerId);
    CHECK(wa.gameplay().begin_recording(ia.player));
    CHECK(wa.animation_controllers().set_parameter(ia.actor, "speed", 1.0, &error));
    CHECK(wa.animation_controllers().set_parameter(ia.actor, "stance", std::int64_t{2}, &error));
    CHECK(wa.animation_controllers().trigger(ia.actor, "missing", &error) == false);
    CHECK(wa.animation().play(ia.rigged, "Idle", true, &error));
    CHECK(wa.animation().crossfade(ia.rigged, "Walk", 2.0F, &error));
    CHECK(wa.animation().set_playback_speed(ia.rigged, 0.75F, &error));
    CHECK(wa.control_rigs().set_control_local(ia.rigged, ControlRigControlId{1}, make_rigid_transform({0.3F, 2.2F, 0.1F}, {}), &error));
#if defined(DVE_ENABLE_CPU_HAIR)
    CHECK(wa.set_cpu_hair_wind(ia.hairy, {1.5F, 0.0F, 0.25F}));
#endif
    CHECK(wa.cameras().director(1)->set_state("close"));
    play(a, 20);
    error.clear();
    RagdollActivationOptions knock;
    knock.linearVelocity = {1.0F, 2.0F, 0.0F};
    CHECK(wa.activate_ragdoll(ia.actor, knock, &error));
    if (!error.empty()) std::cerr << "activate_ragdoll: " << error << '\n';
    play(a, 12);
    CHECK(wa.ragdolls().state(ia.actor) != RagdollRuntimeState::Animated);
    CHECK(wa.animation_controllers().state(ia.actor) == "walk");

    const GameSaveData saved = capture_all(a);
    GameSaveStats stats;
    const auto bytes = codec.encode(saved, &stats, &error);
    CHECK(bytes.has_value());
    if (!bytes) { std::cerr << "runtime encode: " << error << '\n'; return; }
    for (const char* name : {"dve.gameplay", "dve.cameras", "dve.animation", "dve.ragdolls"})
        CHECK(stats.sectionBytes.contains(name));
#if defined(DVE_ENABLE_CPU_HAIR)
    CHECK(stats.sectionBytes.contains("dve.hair"));
#endif
    std::cout << "runtimes: save " << bytes->size() << " bytes (gameplay " << stats.sectionBytes["dve.gameplay"]
              << ", animation " << stats.sectionBytes["dve.animation"] << ", ragdolls " << stats.sectionBytes["dve.ragdolls"]
              << ")\n";
    const auto decoded = codec.decode(*bytes, nullptr, &error);
    CHECK(decoded.has_value());
    if (!decoded) { std::cerr << "runtime decode: " << error << '\n'; return; }
    CHECK(decoded->runtimes.gameplay && decoded->runtimes.cameras && decoded->runtimes.animation &&
          decoded->runtimes.animationControllers && decoded->runtimes.controlRigs && decoded->runtimes.ragdolls);

    Scene b = boot(assets, Physics3DBackend::Reference, &firesB);
    const RuntimeIds ib = boot_runtimes(*b.world);
    CHECK(ib.pawn == ia.pawn && ib.actor == ia.actor);
    CHECK(restore(b, *decoded));
    GameSaveRuntimeReport report;
    CHECK(restore_game_runtime_state(*b.world, decoded->runtimes, &report, &error));
    for (const std::string& warning : report.warnings) std::cerr << "unexpected warning: " << warning << '\n';
    CHECK(report.warnings.empty());
    CHECK(report.gameplayRestored && report.camerasRestored);
    CHECK(report.animationsRestored == 2U && report.controllersRestored == 1U && report.controlRigsRestored == 1U);
    CHECK(report.ragdollsRestored == 1U);
#if defined(DVE_ENABLE_CPU_HAIR)
    CHECK(report.hairRestored == 1U);
#endif
    CHECK(b.world->state_hash() == a.world->state_hash());
    // Every section re-encodes byte for byte.
    const GameSaveData again = capture_all(b);
    for (const char* name : {"dve.gameplay", "dve.cameras", "dve.animation", "dve.ragdolls", "dve.hair"}) {
        const bool same = section_of(codec, again, name) == section_of(codec, saved, name);
        if (!same) std::cerr << "section " << name << " differs after restore\n";
        CHECK(same);
    }
    GameWorld& wb = *b.world;
    CHECK(wb.gameplay().trigger_ids().size() == 2U);
    CHECK(wb.gameplay().controller_of(ib.pawn) == ib.player);
    CHECK(wb.animation_controllers().state(ib.actor) == "walk");
    CHECK(wb.animation().active_clip(ib.rigged) == "Idle" && wb.animation().playback_speed(ib.rigged) == 0.75F);
    CHECK(wb.ragdolls().state(ib.actor) == wa.ragdolls().state(ia.actor));
    CHECK(wb.ragdolls().body_handles(ib.actor).size() == 3U && wb.ragdolls().constraint_handles(ib.actor).size() == 2U);
    CHECK(wb.cameras().director(1)->state() == "close");

    // Continue both: characters, animation, hair and cameras stay in lock step; the ragdoll
    // is rebuilt from its activation pose, so it continues within a small tolerance.
    play(a, 30);
    play(b, 30);
    const GameSaveData afterA = capture_all(a), afterB = capture_all(b);
    for (const char* name : {"dve.gameplay", "dve.animation", "dve.cameras", "dve.hair"}) {
        const bool same = section_of(codec, afterA, name) == section_of(codec, afterB, name);
        if (!same) {
            std::cerr << "section " << name << " diverged after continuing\n";
            if (std::string_view(name) == "dve.cameras" && afterA.runtimes.cameras && afterB.runtimes.cameras)
                std::cerr << *afterA.runtimes.cameras << "---\n" << *afterB.runtimes.cameras;
        }
        CHECK(same);
    }
    const auto bodiesA = wa.ragdolls().body_handles(ia.actor);
    const auto bodiesB = wb.ragdolls().body_handles(ib.actor);
    CHECK(bodiesA.size() == bodiesB.size());
    float worst = 0.0F;
    for (std::size_t i = 0; i < std::min(bodiesA.size(), bodiesB.size()); ++i) {
        const auto sa = wa.physics().state(bodiesA[i]);
        const auto sb = wb.physics().state(bodiesB[i]);
        CHECK(sa && sb);
        if (sa && sb) worst = std::max(worst, length(subtract(sa->currentTransform.position, sb->currentTransform.position)));
    }
    std::cout << "runtimes: ragdoll continuation differs by at most " << worst << " m\n";
    CHECK(worst < 0.05F);
    CHECK(firesA > 0 && b.world->state_hash() == a.world->state_hash());

    // A boot that no longer binds something: a warning, the rest still loads.
    int firesC = 0;
    Scene c = boot(assets, Physics3DBackend::Reference, &firesC);
    (void)boot_runtimes(*c.world, false);
    CHECK(restore(c, *decoded));
    GameSaveRuntimeReport partial;
    CHECK(restore_game_runtime_state(*c.world, decoded->runtimes, &partial, &error));
    CHECK(partial.ragdollsRestored == 0U && partial.animationsRestored == 2U && partial.gameplayRestored);
    bool warned = false;
    for (const std::string& warning : partial.warnings) warned = warned || warning.find("ragdoll") != std::string::npos;
    CHECK(warned);

    // Characters are core state: a save naming a pawn the world does not have fails (and
    // leaves the runtime untouched).
    GameSaveRuntimeState broken;
    broken.gameplay = decoded->runtimes.gameplay;
    broken.gameplay->characters.front().pawn = 999999U;
    const auto beforeBroken = c.world->gameplay().capture_save_state();
    CHECK(!restore_game_runtime_state(*c.world, broken, nullptr, &error));
    CHECK(error.find("999999") != std::string::npos);
    CHECK(c.world->gameplay().capture_save_state().characters.size() == beforeBroken.characters.size());

    // Every truncation of a v2 section is reported against that section.
    auto document = codec.to_document(saved, nullptr, &error);
    CHECK(document.has_value());
    if (document) {
        for (const char* name : {"dve.gameplay", "dve.animation", "dve.ragdolls", "dve.hair"}) {
            if (!document->sections.contains(name)) continue;
            const auto& full = document->sections.at(name);
            for (std::size_t cut = 0; cut < full.size(); cut += std::max<std::size_t>(1U, full.size() / 97U)) {
                SaveGameDocument damaged = *document;
                damaged.sections[name].resize(cut);
                std::string message;
                CHECK(!codec.decode(encode_save_game_document(damaged), nullptr, &message).has_value());
                CHECK(message.find(name) != std::string::npos);
            }
        }
        SaveGameDocument unknown = *document;
        unknown.sections["dve.deformables"] = {};
        CHECK(!codec.decode(encode_save_game_document(unknown), nullptr, &error).has_value());
    }
    std::filesystem::remove_all(assets.dir);
}

// A v1 save (written by #29's engine) still loads: the codec's own v1 -> v2 step bumps the
// version, the sub-runtimes stay as the boot left them.
void test_v1_to_v2_migration() {
    const Assets assets = make_assets();
    GameSaveCodec codec(file_reader());
    std::string error;
    int firesA = 0, firesB = 0;
    Scene a = boot(assets, Physics3DBackend::Reference, &firesA);
    (void)boot_runtimes(*a.world);
    (void)a.world->damage_sphere(a.tower, {0.375F, 1.25F, 0.375F}, 0.3F);
    play(a, 10);
    auto document = codec.to_document(capture_all(a), nullptr, &error);
    CHECK(document.has_value());
    if (!document) return;
    SaveGameDocument v1 = *document;
    v1.schemaVersion = 1U;
    for (const char* name : {"dve.gameplay", "dve.cameras", "dve.animation", "dve.ragdolls", "dve.hair", "dve.clock"}) v1.sections.erase(name);
    const auto v1Bytes = encode_save_game_document(v1);
    SaveGameReadReport report;
    const auto migrated = codec.decode(v1Bytes, &report, &error);
    CHECK(migrated.has_value());
    if (!migrated) { std::cerr << "v1 decode: " << error << '\n'; return; }
    CHECK(report.storedSchemaVersion == 1U && report.migrationsApplied == 2U);
    CHECK(!migrated->runtimes.gameplay && !migrated->runtimes.cameras && !migrated->runtimes.animation &&
          !migrated->runtimes.ragdolls);
    Scene b = boot(assets, Physics3DBackend::Reference, &firesB);
    const RuntimeIds ib = boot_runtimes(*b.world);
    CHECK(restore(b, *migrated));
    CHECK(b.world->state_hash() == a.world->state_hash());
    GameSaveRuntimeReport runtimeReport;
    CHECK(restore_game_runtime_state(*b.world, migrated->runtimes, &runtimeReport, &error));
    CHECK(!runtimeReport.gameplayRestored && runtimeReport.warnings.empty());
    CHECK(b.world->gameplay().has_character(ib.pawn));   // as booted
    // Re-saving writes v2.
    const auto resaved = codec.to_document(capture_all(b), nullptr, &error);
    CHECK(resaved && resaved->schemaVersion == kGameSaveSchemaVersion && resaved->sections.contains("dve.gameplay"));

    // A "v1" document carrying an engine section v1 never had is refused by the migration.
    SaveGameDocument mislabelled = v1;
    mislabelled.sections["dve.gameplay"] = document->sections.at("dve.gameplay");
    CHECK(!codec.decode(encode_save_game_document(mislabelled), nullptr, &error).has_value());
    CHECK(error.find("v1 did not define") != std::string::npos);
    // The engine owns the v1 step; a game cannot register a second one.
    GameSaveCodec other(file_reader());
    CHECK(!other.register_migration(1U, [](SaveGameDocument&, std::string*) { return true; }, &error));
    std::filesystem::remove_all(assets.dir);
}

// Timers kept unbound by a restore are re-attached by id or dropped explicitly.
void test_unbound_timers() {
    GameWorld a(std::make_unique<ReferenceRigidBodyWorld>());
    int bootFires = 0;
    (void)a.schedule_repeating(0.5F, [&] { ++bootFires; });
    a.tick(0.1F);
    int runtimeFires = 0;
    const auto runtimeTimer = a.schedule_once(0.3F, [&] { ++runtimeFires; });
    const auto dropped = a.schedule_once(0.2F, [] {});
    const GameWorldSaveState saved = a.capture_save_state();
    const std::uint64_t hash = a.state_hash();

    GameWorld b(std::make_unique<ReferenceRigidBodyWorld>());
    int bootFiresB = 0;
    (void)b.schedule_repeating(0.5F, [&] { ++bootFiresB; });
    GameWorldRestoreReport report;
    std::string error;
    GameWorldRestoreOptions options;
    options.keepUnboundTimers = true;
    CHECK(b.restore_save_state(saved, &report, &error, options));
    CHECK(report.timersRestored == 1U && report.timersUnbound == 2U && report.timersDropped == 0U);
    CHECK(b.state_hash() == hash);   // unbound timers keep their place in the schedule
    CHECK(b.unbound_timer_ids() == (std::vector<GameWorld::TimerId>{runtimeTimer, dropped}));
    int runtimeFiresB = 0;
    CHECK(b.bind_restored_timer(runtimeTimer, [&] { ++runtimeFiresB; }));
    CHECK(!b.bind_restored_timer(runtimeTimer, [] {}));   // already bound
    CHECK(b.drop_unbound_timers() == 1U);
    CHECK(b.unbound_timer_ids().empty() && !b.has_timer(dropped) && b.has_timer(runtimeTimer));
    for (int i = 0; i < 10; ++i) { a.tick(0.1F); b.tick(0.1F); }
    CHECK(runtimeFires == 1 && runtimeFiresB == 1 && bootFires - 0 >= 1 && bootFiresB == bootFires);

    // Without the option the old behaviour holds: unmatched saved timers are dropped.
    GameWorld c(std::make_unique<ReferenceRigidBodyWorld>());
    GameWorldRestoreReport plain;
    CHECK(c.restore_save_state(saved, &plain, &error));
    CHECK(plain.timersDropped == 3U && plain.timersUnbound == 0U && c.unbound_timer_ids().empty());
}

#if defined(DVE_HAVE_LUA)
void test_script_state() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost host(world);
    std::string error;
    CHECK(host.run_string(R"(
        state = { score = 12, name = "hero", flags = { true, false, 3.5 }, nested = { a = { b = { c = "deep" } } } }
        world.set_global("difficulty", 2)
        world.set_global_vector("tint", 0.1, 0.2, 0.3, 0.4)
        world.on_save(function() return state end)
        world.on_save("counter", function() return 41 end)
        loaded = {}
        world.on_load(function(v) loaded.main = v end)
        world.on_load("counter", function(v) loaded.counter = v end)
    )", "save_test", &error));
    CHECK(host.save_handler_count() == 2U && host.load_handler_count() == 2U);
    const auto blob = host.save_state(&error);
    CHECK(blob.has_value());
    if (!blob) { std::cerr << "save_state: " << error << '\n'; return; }
    const auto blobAgain = host.save_state(&error);
    CHECK(blobAgain && *blobAgain == *blob);   // deterministic (sorted keys)

    GameWorld world2(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost fresh(world2);
    CHECK(fresh.run_string(R"(
        loaded = {}
        world.on_load(function(v) loaded.main = v end)
        world.on_load("counter", function(v) loaded.counter = v end)
        function check()
            local m = loaded.main
            return m.score == 12 and m.name == "hero" and m.flags[1] == true and m.flags[2] == false
               and m.flags[3] == 3.5 and m.nested.a.b.c == "deep" and loaded.counter == 41
               and math.type(m.score) == "integer"
        end
    )", "load_test", &error));
    CHECK(fresh.load_state(*blob, &error));
    CHECK(fresh.global_number("difficulty") == 2.0);
    CHECK(fresh.global_vector("tint") && fresh.global_vector("tint")->w == 0.4F);
    CHECK(fresh.run_string("assert(check(), 'restored state differs')", "verify", &error));
    if (!error.empty()) std::cerr << "verify: " << error << '\n';

    // Unsaveable values name the offending path and fail the save.
    GameWorld world3(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost bad(world3);
    CHECK(bad.run_string("world.on_save(function() return { cb = { f = print } } end)", "bad", &error));
    error.clear();
    CHECK(!bad.save_state(&error).has_value());
    CHECK(error.find(".cb.f") != std::string::npos && error.find("function") != std::string::npos);
    CHECK(bad.run_string("world.on_save(function() local t = {} t.self = t return t end)", "cycle", &error));
    CHECK(!bad.save_state(&error).has_value());
    CHECK(error.find("cycle") != std::string::npos);
    CHECK(bad.run_string("world.on_save(function() error('boom') end)", "raise", &error));
    CHECK(!bad.save_state(&error).has_value());
    CHECK(error.find("boom") != std::string::npos);
    CHECK(bad.run_string("world.on_save(function() local t = {} local c = t for i = 1, 40 do c.n = {} c = c.n end return t end)", "deep", &error));
    CHECK(!bad.save_state(&error).has_value());

    // Malformed blobs are rejected without touching the state.
    for (std::size_t length = 0; length < blob->size(); ++length) {
        std::string message;
        CHECK(!fresh.load_state(std::span(blob->data(), length), &message));
    }
    auto corrupt = *blob;
    corrupt[0] ^= std::byte{1};
    CHECK(!fresh.load_state(corrupt, &error));

    // save_game / load_game without a host handler fail softly; with one they reach the host.
    CHECK(fresh.run_string(R"(
        local ok, err = world.save_game("slot1")
        assert(ok == nil and err ~= nil)
        assert(world.save_exists("slot1") == false)
    )", "nohandler", &error));
    std::vector<std::string> requests;
    fresh.set_save_request_handler([&](ScriptSaveRequest kind, const std::string& slot, std::string*) {
        requests.push_back(std::to_string(static_cast<int>(kind)) + ":" + slot);
        return true;
    });
    CHECK(fresh.run_string(R"(
        assert(world.save_game("slot1") == true)
        assert(world.load_game() == true)
        assert(world.save_exists("x") == true)
    )", "handler", &error));
    CHECK(requests.size() == 3U && requests[0] == "0:slot1" && requests[1] == "1:quicksave" && requests[2] == "2:x");
}

// Script state v2: render environment, HUD, material overrides and named timers.
void test_script_state_v2() {
    const char* bootScript = R"(
        fired = {}
        world.timer_handler("spawn", function(data, id) fired[#fired + 1] = "spawn:" .. data.wave .. ":" .. data.name end)
        world.timer_handler("pulse", function(n) fired[#fired + 1] = "pulse:" .. n end)
        local master = world.create_master_material("Lava", {
            scalars = { {name="Roughness", default=0.9, min=0.0, max=1.0} },
            vectors = { {name="Emissive", default={0,0,0,0}} },
        })
        world.create_material_instance({ name = "Lava01", material_id = 7, master = master })
        -- Scheduled at boot: the loading process schedules it again with the same id.
        boot_pulse = world.schedule_repeating_named(0.25, "pulse", 1)
        function play()
            world.set_environment_scalar("SunIntensity", 5.5)
            world.set_environment_vector("SkyColor", 0.1, 0.2, 0.9)
            world.set_environment_shadow_mode("hard")
            world.hud_set_interaction_prompt("Press E")
            world.hud_set_tools({ {id="pick", label="Pickaxe"}, {id="bomb", label="Bomb"}, {id="off", label="Off", enabled=false} })
            world.hud_select_next_tool()
            world.set_material_parameter(7, "Roughness", 0.2)
            world.set_material_parameter(7, "Emissive", {2.0, 0.5, 0.0, 0.0})
            -- Scheduled at runtime: only the save knows about these.
            world.schedule_once_named(0.4, "spawn", { wave = 3, name = "goblins" })
            anonymous = world.schedule_once(0.4, function() fired[#fired + 1] = "anonymous" end)
            cancelled = world.schedule_once_named(0.3, "spawn", { wave = 9, name = "never" })
            world.cancel_timer(cancelled)
        end
        function summary() return table.concat(fired, ",") end
    )";
    std::string error;
    GameWorld worldA(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost a(worldA);
    CHECK(a.run_string(bootScript, "boot", &error));
    if (!error.empty()) { std::cerr << "boot: " << error << '\n'; return; }
    CHECK(a.run_string("play()", "play", &error));
    if (!error.empty()) std::cerr << "play: " << error << '\n';
    for (int i = 0; i < 6; ++i) worldA.tick(0.05F);   // t = 0.3
    const GameWorldSaveState world = worldA.capture_save_state();
    const auto blob = a.save_state(&error);
    CHECK(blob.has_value());
    if (!blob) { std::cerr << "save_state v2: " << error << '\n'; return; }

    // Unsaveable timer data fails the save with a path.
    GameWorld worldBad(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost bad(worldBad);
    CHECK(bad.run_string("world.timer_handler('x', function() end) world.schedule_once_named(1, 'x', { f = print })", "bad", &error));
    CHECK(!bad.save_state(&error).has_value() && error.find("named timer") != std::string::npos);
    // Unknown handler names are an error at schedule time.
    CHECK(!bad.run_string("world.schedule_once_named(1, 'nope')", "unknown", &error));

    GameWorld worldB(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost b(worldB);
    std::vector<std::string> logs;
    b.set_log_sink([&](bool, const std::string& message) { logs.push_back(message); });
    CHECK(b.run_string(bootScript, "boot", &error));
    GameWorldRestoreOptions options;
    options.keepUnboundTimers = true;
    GameWorldRestoreReport report;
    CHECK(worldB.restore_save_state(world, &report, &error, options));
    CHECK(report.timersRestored == 1U && report.timersUnbound == 2U);   // boot pulse; spawn + anonymous
    CHECK(b.load_state(*blob, &error));
    CHECK(worldB.unbound_timer_ids().size() == 1U);   // the anonymous closure
    CHECK(worldB.drop_unbound_timers() == 1U);
    CHECK(worldB.state_hash() != 0U);

    const RenderEnvironment& env = b.environment();
    CHECK(env.sunIntensity == 5.5F && env.skyColor.z == 0.9F && env.shadowMode == ShadowMode::Hard);
    CHECK(b.hud_model().interaction_prompt() == "Press E");
    CHECK(b.hud_model().tools().size() == 3U && b.hud_model().selected_tool() && b.hud_model().selected_tool()->id == "bomb");
    CHECK(b.material_library().runtime_scalar(7, "Roughness") == 0.2F);
    CHECK(b.material_library().runtime_vector(7, "Emissive") && b.material_library().runtime_vector(7, "Emissive")->x == 2.0F);
    CHECK(b.material_library().resolved(7) != nullptr);
    const auto blobB = b.save_state(&error);
    CHECK(blobB.has_value());
    {
        // clear_material_parameters drops scalar *and* vector overrides (it used to stop after
        // the scalars because of a short-circuited `||`).
        GameWorld worldC(std::make_unique<ReferenceRigidBodyWorld>());
        GameScriptHost c(worldC);
        CHECK(c.run_string(bootScript, "boot", &error) && c.load_state(*blob, &error));
        CHECK(c.run_string("world.clear_material_parameters(7)", "clear", &error));
        CHECK(!c.material_library().runtime_scalar(7, "Roughness") && !c.material_library().runtime_vector(7, "Emissive"));
    }

    CHECK(a.run_string("fired = {}", "reset", &error));   // compare what fires after the save only
    for (int i = 0; i < 6; ++i) { worldA.tick(0.05F); worldB.tick(0.05F); }   // t = 0.6
    CHECK(a.run_string("result = summary()", "sum", &error) && b.run_string("result = summary()", "sum", &error));
    const auto summaryA = [&](GameScriptHost& host) {
        (void)host.run_string("world.set_global('n', #fired)", "n", &error);
        return host.global_number("n").value_or(-1.0);
    };
    CHECK(b.run_string(R"(
        local s = summary()
        assert(s:find("spawn:3:goblins", 1, true), "restored named timer did not fire with its data: " .. s)
        assert(not s:find("never", 1, true), "cancelled named timer fired")
        assert(not s:find("anonymous", 1, true), "an anonymous closure was restored")
    )", "verifyB", &(error = std::string())));
    if (!error.empty()) std::cerr << "verifyB: " << error << '\n';
    CHECK(a.run_string(R"(assert(summary():find("anonymous", 1, true)))", "verifyA", &error));
    // Same named-timer firings in both processes (A additionally ran its anonymous closure).
    CHECK(summaryA(a) == summaryA(b) + 1.0);
    CHECK(a.run_string("world.set_global('same', summary() == 'spawn:3:goblins,anonymous,pulse:1' and 1 or 0)", "a", &error));
    CHECK(b.run_string("world.set_global('same', summary() == 'spawn:3:goblins,pulse:1' and 1 or 0)", "b", &error));
    CHECK(a.global_number("same") == 1.0 && b.global_number("same") == 1.0);

    // A v1 script blob (from a #29 save) still loads.
    GameWorld worldV1(std::make_unique<ReferenceRigidBodyWorld>());
    GameScriptHost v1(worldV1);
    CHECK(v1.run_string("world.set_global('score', 5)", "v1", &error));
    auto v1Blob = v1.save_state(&error);
    CHECK(v1Blob.has_value());
    if (v1Blob) {
        // Strip the v2 tail: environment (30*4 + 5*4), empty prompt (4), no tools (4),
        // selection (8), no materials (4), no named timers (4); then mark it version 1.
        v1Blob->resize(v1Blob->size() - (120U + 20U + 4U + 4U + 8U + 4U + 4U));
        (*v1Blob)[4] = std::byte{1};
        GameWorld worldV1b(std::make_unique<ReferenceRigidBodyWorld>());
        GameScriptHost v1b(worldV1b);
        CHECK(v1b.load_state(*v1Blob, &error));
        CHECK(v1b.global_number("score") == 5.0);
    }
}
#endif

} // namespace

int main() {
    test_round_trip(Physics3DBackend::Reference, "reference");
    test_destruction(Physics3DBackend::Reference, "reference");
    {
        std::string error;
        if (create_physics3d_world(Physics3DBackend::Jolt, nullptr, &error)) {
            test_round_trip(Physics3DBackend::Jolt, "jolt");
            test_destruction(Physics3DBackend::Jolt, "jolt");
        } else {
            std::cout << "Jolt not compiled in; skipping the Jolt round trips\n";
        }
    }
    test_corruption();
    test_migration();
    test_game_sections_and_limits();
    test_runtime_round_trip();
    test_v1_to_v2_migration();
    test_unbound_timers();
#if defined(DVE_HAVE_LUA)
    test_script_state();
    test_script_state_v2();
#endif
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dve_game_save_tests passed\n";
    return 0;
}
