#include "dve/render/brick_palette_rhi.hpp"
#include "dve/rhi/null_device.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

dve::VoxelMaterialDefinition material(const char* name, float r, float g, float b) {
    dve::VoxelMaterialDefinition value;
    value.name = name;
    value.baseColor = {r, g, b, 1.0F};
    value.roughness = 0.35F;
    return value;
}

} // namespace

int main() {
    try {
        using namespace dve;
        using namespace dve::render;
        using namespace dve::rhi;

        VoxelObject object(226U);
        object.fill_brick({0, 0, 0}, 1U); // single/baked fallback brick
        object.set_voxel({8, 0, 0}, 1U);
        object.set_voxel({9, 0, 0}, 2U);
        object.set_voxel({10, 0, 0}, 3U);
        object.set_voxel({11, 0, 0}, 4U); // palette4 brick

        std::vector<VoxelMaterialDefinition> materials;
        materials.push_back(material("Air", 0.0F, 0.0F, 0.0F));
        materials.push_back(material("Stone", 0.5F, 0.5F, 0.5F));
        materials.push_back(material("Gold", 1.0F, 0.7F, 0.1F));
        materials.push_back(material("Copper", 0.8F, 0.25F, 0.1F));
        materials.push_back(material("Paint", 0.1F, 0.2F, 0.9F));

        BrickPaletteAssetCookSettings cookSettings;
        cookSettings.palette2Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        cookSettings.palette4Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        const auto asset = cook_voxel_object_brick_palettes(object, materials, cookSettings);
        require(asset.bricks.size() == 2U, "expected two cooked bricks");

        BrickPaletteSubmissionSettings submission;
        submission.preferredPath = VoxelMaterialRuntimePath::DeferredPalette4;
        submission.mapping = BrickPaletteMappingMode::WorldTriplanar;
        const auto packet = build_brick_palette_upload_packet(asset, submission);
        const auto repeat = build_brick_palette_upload_packet(asset, submission);
        require(packet.contentHash == repeat.contentHash, "upload packet hash is nondeterministic");
        require(packet.records.size() == 2U, "upload packet record count is wrong");
        require(packet.fallbackBrickCount == 1U, "single-material brick did not use a fallback");
        require(!packet.paletteSamples.empty(), "palette samples were not packed");
        require(!packet.paletteSlots.empty(), "palette slots were not packed");
        require(!packet.bakedSamples.empty(), "fallback baked samples were not packed");
        std::string error;
        require(validate_brick_palette_upload_packet(packet, &error), error.c_str());

        const auto contracts = brick_palette_pipeline_contracts();
        require(contracts[0].maximumSlots == 2U && contracts[2].maximumSlots == 4U,
                "pipeline slot contracts are wrong");
        require(contracts[1].mapping == BrickPaletteMappingMode::WorldTriplanar,
                "triplanar pipeline contract is wrong");
        const auto layout = brick_palette_bind_group_layout_desc();
        require(layout.bindings.size() == kBrickPaletteBindingCount,
                "brick-palette binding contract count is wrong");

        NullDevice device;
        BrickPaletteRhiMirror mirror(device);
        require(mirror.upload(packet, &error), error.c_str());
        require(mirror.readback_matches(packet, &error), error.c_str());
        require(mirror.stats().publications == 1U, "RHI publication count is wrong");
        require(mirror.stats().reallocations == kBrickPaletteBindingCount,
                "RHI did not allocate every logical buffer");

        BrickPaletteRhiBindings bindings;
        require(create_brick_palette_rhi_bindings(device, mirror, bindings, &error), error.c_str());
        require(bindings.layout && bindings.group, "brick-palette bindings were not created");
        require(!device.destroy_buffer(mirror.record_buffer(), &error),
                "live bind group did not retain the record buffer");
        require(destroy_brick_palette_rhi_bindings(device, bindings, &error), error.c_str());

        const auto commands = device.begin_commands(QueueKind::Graphics,
                                                     "brick palette reference submission", &error);
        require(static_cast<bool>(commands), error.c_str());
        require(device.begin_debug_label(commands, "BrickPaletteSubmission", &error), error.c_str());
        require(device.end_debug_label(commands, &error), error.c_str());
        const auto fence = device.submit(commands, &error);
        require(fence && device.fence_complete(fence), "Null-RHI submission did not complete");

        mirror.reset();
        const auto stats = device.statistics();
        require(stats.bindGroupLayoutsCreated == 1U && stats.bindGroupsCreated == 1U,
                "binding creation statistics are wrong");
        require(stats.commandListsSubmitted == 1U, "reference submission was not recorded");

        std::cout << "dve_v226_brick_palette_rhi_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v226_brick_palette_rhi_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
