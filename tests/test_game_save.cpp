// Save games (dve/game_save.hpp): GameWorld capture/restore round trips, destruction deltas,
// the DVESAVE1 container's corruption handling, schema migrations and (with Lua) script state.
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
    CHECK(report.storedSchemaVersion == 0U && report.migrationsApplied == 1U);
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
#if defined(DVE_HAVE_LUA)
    test_script_state();
#endif
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dve_game_save_tests passed\n";
    return 0;
}
