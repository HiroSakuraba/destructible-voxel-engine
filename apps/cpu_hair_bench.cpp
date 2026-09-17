#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "dve/cpu_hair.hpp"
#ifdef DVE_ENABLE_SOFT_BODIES
#include "dve/soft_body.hpp"
#endif

namespace {

std::uint32_t parse_or_default(char* value, std::uint32_t fallback) {
    if (value == nullptr) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0UL || parsed > 1000000UL) return fallback;
    return static_cast<std::uint32_t>(parsed);
}

template<class Function>
double measure_ms(std::uint32_t frames, Function&& function) {
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint32_t frame = 0U; frame < frames; ++frame) function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

#ifdef DVE_ENABLE_SOFT_BODIES
dve::SoftBodyAsset make_generic_rope_groom(const dve::HairAsset& groom) {
    dve::SoftBodyAsset asset;
    asset.name = "Generic Rope Groom Baseline";
    asset.kind = dve::SoftBodyKind::Rope;
    asset.bendModel = dve::SoftBodyBendModel::Distance;
    asset.linearDamping = 0.02F;
    for (const dve::HairGuide& guide : groom.guides) {
        const std::uint32_t first = static_cast<std::uint32_t>(asset.vertices.size());
        for (std::size_t point = 0U; point < guide.points.size(); ++point) {
            asset.vertices.push_back({guide.points[point], guide.anchored[point] != 0U ? 0.0F : 1.0F,
                                      groom.pointRadiusMeters});
            if (point > 0U) {
                const std::uint32_t current = first + static_cast<std::uint32_t>(point);
                asset.stretchConstraints.push_back({current - 1U, current,
                    dve::length(dve::subtract(guide.points[point], guide.points[point - 1U])),
                    groom.stretchCompliance});
            }
            if (point > 1U) {
                const std::uint32_t current = first + static_cast<std::uint32_t>(point);
                asset.bendConstraints.push_back({current - 2U, current,
                    dve::length(dve::subtract(guide.points[point], guide.points[point - 2U])),
                    groom.bendCompliance});
            }
        }
    }
    asset.recompute_hash();
    return asset;
}
#endif

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t guides = parse_or_default(argc > 1 ? argv[1] : nullptr, 2048U);
    const std::uint32_t points = parse_or_default(argc > 2 ? argv[2] : nullptr, 16U);
    const std::uint32_t frames = parse_or_default(argc > 3 ? argv[3] : nullptr, 120U);
    const dve::HairAsset groom = dve::make_straight_hair_groom(guides, points, 0.004F, 0.015F);

    dve::CpuHairInstanceDesc desc;
    desc.solver.solverIterations = 5U;
    desc.solver.maximumSubsteps = 2U;
    desc.solver.workerChunkGuides = 32U;
    desc.solver.enableSleeping = false;
    desc.windVelocity = {0.75F, 0.0F, 0.15F};
    desc.collision.spheres.push_back({{0.0F, -0.04F, 0.0F}, 0.12F});

    std::string error;
    dve::CpuHairWorld serial(0U);
    const dve::CpuHairId serialId = serial.create(groom, desc, &error);
    if (serialId == 0U) {
        std::cerr << error << '\n';
        return 1;
    }
    dve::CpuHairWorld parallel;
    const dve::CpuHairId parallelId = parallel.create(groom, desc, &error);
    if (parallelId == 0U) {
        std::cerr << error << '\n';
        return 1;
    }
    for (int warmup = 0; warmup < 10; ++warmup) {
        (void)serial.step(1.0F / 60.0F);
        (void)parallel.step(1.0F / 60.0F);
    }
    const double serialMs = measure_ms(frames, [&] { (void)serial.step(1.0F / 60.0F); });
    const double parallelMs = measure_ms(frames, [&] { (void)parallel.step(1.0F / 60.0F); });

    std::cout << "{\"guides\":" << guides
              << ",\"points_per_guide\":" << points
              << ",\"points\":" << static_cast<std::uint64_t>(guides) * points
              << ",\"frames\":" << frames
              << ",\"workers\":" << parallel.worker_count()
              << ",\"serial_ms_per_frame\":" << serialMs / static_cast<double>(frames)
              << ",\"parallel_ms_per_frame\":" << parallelMs / static_cast<double>(frames)
              << ",\"parallel_speedup\":" << serialMs / std::max(parallelMs, 1.0e-9)
              << "}" << '\n';

    dve::CpuHairInstanceDesc selfCollisionDesc = desc;
    selfCollisionDesc.solver.enableSelfCollision = true;
    selfCollisionDesc.solver.selfCollisionIterations = 1U;
    selfCollisionDesc.solver.selfCollisionMaximumNeighbors = 24U;
    selfCollisionDesc.solver.selfCollisionStiffness = 0.85F;
    dve::CpuHairWorld selfCollisionParallel;
    const dve::CpuHairId selfCollisionId = selfCollisionParallel.create(
        groom, selfCollisionDesc, &error);
    if (selfCollisionId == 0U) {
        std::cerr << error << '\n';
        return 1;
    }
    for (int warmup = 0; warmup < 5; ++warmup) {
        (void)selfCollisionParallel.step(1.0F / 60.0F);
    }
    dve::CpuHairStepTelemetry selfCollisionTelemetry{};
    const double selfCollisionMs = measure_ms(frames, [&] {
        selfCollisionTelemetry = selfCollisionParallel.step(1.0F / 60.0F);
    });
    std::cout << "{\"self_collision_guides\":" << guides
              << ",\"points_per_guide\":" << points
              << ",\"frames\":" << frames
              << ",\"workers\":" << selfCollisionParallel.worker_count()
              << ",\"parallel_ms_per_frame\":"
              << selfCollisionMs / static_cast<double>(frames)
              << ",\"overhead_vs_no_self_collision\":"
              << selfCollisionMs / std::max(parallelMs, 1.0e-9)
              << ",\"self_collision_points_last_frame\":"
              << selfCollisionTelemetry.selfCollisionPoints
              << ",\"self_collision_tests_last_frame\":"
              << selfCollisionTelemetry.selfCollisionTests
              << ",\"self_collision_projections_last_frame\":"
              << selfCollisionTelemetry.selfCollisionProjections
              << ",\"hash_insert_failures_last_frame\":"
              << selfCollisionTelemetry.selfCollisionHashInsertFailures
              << "}" << '\n';

    if (guides > 512U) {
        dve::CpuHairInstanceDesc budgetedDesc = selfCollisionDesc;
        budgetedDesc.solver.maximumSelfCollisionGuides = 512U;
        dve::CpuHairWorld budgetedParallel;
        const dve::CpuHairId budgetedId = budgetedParallel.create(groom, budgetedDesc, &error);
        if (budgetedId == 0U) {
            std::cerr << error << '\n';
            return 1;
        }
        for (int warmup = 0; warmup < 5; ++warmup) {
            (void)budgetedParallel.step(1.0F / 60.0F);
        }
        dve::CpuHairStepTelemetry budgetedTelemetry{};
        const double budgetedMs = measure_ms(frames, [&] {
            budgetedTelemetry = budgetedParallel.step(1.0F / 60.0F);
        });
        std::cout << "{\"self_collision_guides\":" << guides
                  << ",\"self_collision_guide_budget\":512"
                  << ",\"points_per_guide\":" << points
                  << ",\"frames\":" << frames
                  << ",\"workers\":" << budgetedParallel.worker_count()
                  << ",\"parallel_ms_per_frame\":"
                  << budgetedMs / static_cast<double>(frames)
                  << ",\"overhead_vs_no_self_collision\":"
                  << budgetedMs / std::max(parallelMs, 1.0e-9)
                  << ",\"self_collision_points_last_frame\":"
                  << budgetedTelemetry.selfCollisionPoints
                  << ",\"self_collision_tests_last_frame\":"
                  << budgetedTelemetry.selfCollisionTests
                  << ",\"self_collision_projections_last_frame\":"
                  << budgetedTelemetry.selfCollisionProjections
                  << ",\"hash_insert_failures_last_frame\":"
                  << budgetedTelemetry.selfCollisionHashInsertFailures
                  << "}" << '\n';
    }

#ifdef DVE_ENABLE_SOFT_BODIES
    dve::SoftBodyWorld generic;
    dve::RuntimeSoftBodyInstance genericInstance;
    genericInstance.solverIterations = desc.solver.solverIterations;
    genericInstance.maximumSubsteps = desc.solver.maximumSubsteps;
    genericInstance.collideWithGround = false;
    const dve::RuntimeSoftBodyId genericId = generic.create(
        make_generic_rope_groom(groom), genericInstance, &error);
    if (genericId != 0U) {
        for (int warmup = 0; warmup < 5; ++warmup)
            (void)generic.step(1.0F / 60.0F, desc.gravity);
        const double genericMs = measure_ms(frames, [&] {
            (void)generic.step(1.0F / 60.0F, desc.gravity);
        });
        std::cout << "{\"generic_soft_body_ms_per_frame\":"
                  << genericMs / static_cast<double>(frames)
                  << ",\"batched_parallel_vs_generic\":"
                  << genericMs / std::max(parallelMs, 1.0e-9) << "}" << '\n';
    }
#endif
    return 0;
}
