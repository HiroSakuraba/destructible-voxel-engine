#include "dve/cpu_hair_runtime.hpp"
#include "dve/tilemap_authoring.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    std::string error;

    dve::TileSetAuthoringSession tilesetDocument;
    require(tilesetDocument.create("merge_tiles", "textures/merge_tiles.png",
                                   16U, 16U, 16U, 16U, 0U, 0U, &error),
            "could not create merged tileset: " + error);

    dve::TileDef tile = tilesetDocument.tileset().tiles.front();
    tile.name = "solid_ground";
    tile.collision = dve::TileCollision::Solid;
    tile.terrain = "ground";
    tile.material = "stone";
    require(tilesetDocument.update_tile(0U, tile, &error),
            "could not author merged tile semantics: " + error);

    dve::TileMapAuthoringSession levelDocument;
    require(levelDocument.create("merge_level", 8U, 6U, tilesetDocument.tileset(),
                                 "tiles/merge_tiles.dvetileset", &error),
            "could not create merged tilemap: " + error);
    require(levelDocument.select_tile(1U), "could not select authored tile");
    require(levelDocument.paint_cell(3U, 4U, 1U, &error),
            "could not paint authored tile: " + error);
    require(dve::tile_id(levelDocument.map().layers.front().at(3U, 4U)) == 1U,
            "painted tile did not survive in merged tilemap runtime");
    const std::uint64_t tileHashBeforeHair = levelDocument.map().content_hash();

    dve::CpuHairRuntime hairRuntime(2U);
    dve::CpuHairBindOptions hairOptions;
    hairOptions.simulation.gravity = {0.0F, -9.81F, 0.0F};
    hairOptions.simulation.solver.enableSleeping = false;
    hairOptions.simulation.solver.maximumSubsteps = 2U;
    hairOptions.simulation.solver.enableSelfCollision = true;
    hairOptions.simulation.solver.selfCollisionIterations = 1U;
    const dve::HairAsset groom = dve::make_straight_hair_groom(8U, 8U, 0.015F, 0.02F);
    require(hairRuntime.bind(42U, groom, dve::RigidTransform{}, hairOptions, &error),
            "could not bind CPU hair in merged runtime: " + error);

    const dve::CpuHairStepTelemetry telemetry = hairRuntime.tick(1.0F / 60.0F);
    const dve::CpuHairView hairView = hairRuntime.view(42U);
    require(!hairView.positions.empty() && !hairView.strands.empty(),
            "merged CPU hair view is invalid");
    require(hairView.strands.size() == 8U && hairView.strands.front().pointCount == 8U,
            "merged CPU hair topology is incorrect");
    require(hairView.positions.size() == 64U,
            "merged CPU hair position span is incomplete");
    require(telemetry.simulatedGuides > 0U,
            "merged CPU hair did not execute simulation work");

    require(levelDocument.map().content_hash() == tileHashBeforeHair,
            "CPU hair activity mutated tile-world authoring state");
    require(hairRuntime.contains(42U),
            "CPU hair owner mapping was lost while tile-world state remained active");

    if (failures != 0) {
        std::cerr << failures << " v2.16 mainline merge checks failed\n";
        return 1;
    }
    std::cout << "v2.16 tile-world + CPU-hair mainline merge checks passed\n";
    return 0;
}
