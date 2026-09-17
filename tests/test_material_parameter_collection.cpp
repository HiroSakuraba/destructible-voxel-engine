#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

#include "dve/gpu_material_parameter_collection.hpp"

namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #__VA_ARGS__ "\n"; ++failures; } } while (false)
using namespace dve;

void test_canonical_slots_and_updates() {
    MaterialParameterCollection collection;
    CHECK(collection.scalar_slot("TimeSeconds") == kMpcTimeSecondsSlot);
    CHECK(collection.scalar_slot("WindStrength") == kMpcWindStrengthSlot);
    CHECK(collection.scalar_slot("Wetness") == kMpcWetnessSlot);
    CHECK(collection.vector_slot("TimeOfDayTint") == kMpcTimeOfDayTintSlot);
    CHECK(collection.vector_slot("WindDirection") == kMpcWindDirectionSlot);
    CHECK(collection.set_scalar("WindStrength", 0.75F));
    CHECK(collection.set_vector("WindDirection", {0.0F, 0.0F, 1.0F, 0.0F}));
    CHECK(std::fabs(*collection.scalar("WindStrength") - 0.75F) < 1.0e-6F);
    CHECK(collection.vector("WindDirection")->z == 1.0F);
    std::string error;
    CHECK(collection.validate(&error));
}

void test_validation_and_capacity() {
    MaterialParameterCollection collection;
    std::string error;
    CHECK(!collection.set_scalar("bad name", 1.0F, &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!collection.set_scalar("Infinite", std::numeric_limits<float>::infinity(), &error));
    CHECK(!error.empty());
    for (std::size_t i = collection.scalar_count(); i < kMaximumMaterialGlobalScalars; ++i) {
        CHECK(collection.set_scalar("Scalar" + std::to_string(i), static_cast<float>(i)));
    }
    CHECK(!collection.set_scalar("Overflow", 1.0F, &error));
}

void test_versioned_persistence_and_corruption_rejection() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dve_mpc_tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "world.dvematparams";

    MaterialParameterCollection collection;
    std::string error;
    CHECK(collection.set_scalar("RoughnessScale", 0.625F, &error));
    CHECK(collection.set_vector("StormTint", {0.4F, 0.5F, 0.8F, 1.0F}, &error));
    CHECK(collection.save(path, &error));
    const auto loaded = MaterialParameterCollection::load(path, &error);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->scalar_slot("RoughnessScale") == collection.scalar_slot("RoughnessScale"));
        CHECK(loaded->vector_slot("StormTint") == collection.vector_slot("StormTint"));
        CHECK(std::fabs(*loaded->scalar("RoughnessScale") - 0.625F) < 1.0e-6F);
        CHECK(std::fabs(loaded->vector("StormTint")->z - 0.8F) < 1.0e-6F);
    }

    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append.put(static_cast<char>(0x5A));
    }
    error.clear();
    CHECK(!MaterialParameterCollection::load(path, &error).has_value());
    CHECK(!error.empty());
    std::filesystem::remove_all(root);
}

void test_gpu_pack_layout() {
    MaterialParameterCollection collection;
    CHECK(collection.set_scalar("WindStrength", 0.5F));
    CHECK(collection.set_scalar("Wetness", 0.25F));
    CHECK(collection.set_vector("TimeOfDayTint", {0.8F, 0.9F, 1.0F, 1.0F}));
    const GpuMaterialParameterCollection gpu = pack_gpu_material_parameter_collection(collection);
    CHECK(gpu.scalarCount == collection.scalar_count());
    CHECK(gpu.vectorCount == collection.vector_count());
    CHECK(gpu.scalarGroups[0].x == 0.0F); // TimeSeconds
    CHECK(gpu.scalarGroups[0].y == 0.5F); // WindStrength
    CHECK(gpu.scalarGroups[0].z == 0.25F); // Wetness
    CHECK(gpu.vectors[0].x == 0.8F && gpu.vectors[0].z == 1.0F);
    CHECK(offsetof(GpuMaterialParameterCollection, scalarGroups) == 0);
    CHECK(offsetof(GpuMaterialParameterCollection, vectors) == 128);
    CHECK(offsetof(GpuMaterialParameterCollection, scalarCount) == 384);
    CHECK(offsetof(GpuMaterialParameterCollection, vectorCount) == 388);
    CHECK(sizeof(GpuMaterialParameterCollection) == 400);
}
}

int main() {
    test_canonical_slots_and_updates();
    test_validation_and_capacity();
    test_versioned_persistence_and_corruption_rejection();
    test_gpu_pack_layout();
    if (failures == 0) { std::cout << "dve_material_parameter_collection_tests: PASS\n"; return 0; }
    std::cerr << "dve_material_parameter_collection_tests: " << failures << " FAILURE(S)\n";
    return 1;
}
