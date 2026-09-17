#include "dve/render/brickmap_rhi_mirror.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dve::render {
namespace {

std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t capacity = std::max<std::size_t>(current, 256U);
    while (capacity < required) capacity = capacity + capacity / 2U;
    return capacity;
}

std::span<const std::byte> material_bytes(std::span<const std::uint8_t> values) noexcept {
    return {reinterpret_cast<const std::byte*>(values.data()), values.size()};
}

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

} // namespace

PackedBrickmapRhiMirror::~PackedBrickmapRhiMirror() {
    reset();
}

bool PackedBrickmapRhiMirror::ensure_buffer(rhi::BufferHandle& handle,
                                            std::size_t& capacity,
                                            std::size_t requiredBytes,
                                            std::string_view debugName,
                                            std::string* error) {
    const std::size_t nonzeroRequired = std::max<std::size_t>(requiredBytes, 4U);
    if (handle && capacity >= nonzeroRequired) return true;
    if (handle && !device_.destroy_buffer(handle, error)) return false;

    capacity = grown_capacity(0U, nonzeroRequired);
    rhi::BufferDesc desc;
    desc.bytes = capacity;
    desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource |
                 rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.debugName.assign(debugName.begin(), debugName.end());
    handle = device_.create_buffer(desc, error);
    if (!handle) {
        capacity = 0U;
        return false;
    }
    ++stats_.reallocations;
    return true;
}

bool PackedBrickmapRhiMirror::upload(const PackedBrickmapScene& scene, std::string* error) {
    const auto index = std::as_bytes(scene.index_grid());
    const auto records = std::as_bytes(scene.records());
    const auto materials = material_bytes(scene.material_arena());

    if (!ensure_buffer(indexBuffer_, stats_.indexCapacityBytes, index.size(),
                       "DVE brick index grid", error) ||
        !ensure_buffer(recordBuffer_, stats_.recordCapacityBytes, records.size(),
                       "DVE packed brick records", error) ||
        !ensure_buffer(materialBuffer_, stats_.materialCapacityBytes, materials.size(),
                       "DVE material arena", error)) {
        return false;
    }

    if ((!index.empty() && !device_.write_buffer(indexBuffer_, 0U, index, error)) ||
        (!records.empty() && !device_.write_buffer(recordBuffer_, 0U, records, error)) ||
        (!materials.empty() && !device_.write_buffer(materialBuffer_, 0U, materials, error))) {
        return false;
    }

    indexBytes_ = index.size();
    recordBytes_ = records.size();
    materialBytes_ = materials.size();
    stats_.uploadedBytes += indexBytes_ + recordBytes_ + materialBytes_;
    ++stats_.publications;
    return true;
}

bool PackedBrickmapRhiMirror::readback_matches(const PackedBrickmapScene& scene,
                                                std::string* error) {
    const auto expectedIndex = std::as_bytes(scene.index_grid());
    const auto expectedRecords = std::as_bytes(scene.records());
    const auto expectedMaterials = material_bytes(scene.material_arena());
    if (expectedIndex.size() != indexBytes_ || expectedRecords.size() != recordBytes_ ||
        expectedMaterials.size() != materialBytes_) {
        set_error(error, "RHI mirror byte counts differ from the current packed scene");
        return false;
    }

    const auto compare = [&](rhi::BufferHandle handle, std::span<const std::byte> expected,
                             std::string_view label) {
        if (expected.empty()) return true;
        std::vector<std::byte> actual(expected.size());
        if (!device_.read_buffer(handle, 0U, actual, error)) return false;
        if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end())) {
            if (error != nullptr) *error = std::string(label) + " readback differs from CPU publication";
            return false;
        }
        return true;
    };

    return compare(indexBuffer_, expectedIndex, "index grid") &&
           compare(recordBuffer_, expectedRecords, "brick records") &&
           compare(materialBuffer_, expectedMaterials, "material arena");
}

void PackedBrickmapRhiMirror::reset() noexcept {
    std::string ignored;
    if (indexBuffer_) device_.destroy_buffer(indexBuffer_, &ignored);
    if (recordBuffer_) device_.destroy_buffer(recordBuffer_, &ignored);
    if (materialBuffer_) device_.destroy_buffer(materialBuffer_, &ignored);
    indexBuffer_ = {};
    recordBuffer_ = {};
    materialBuffer_ = {};
    indexBytes_ = 0U;
    recordBytes_ = 0U;
    materialBytes_ = 0U;
    stats_.indexCapacityBytes = 0U;
    stats_.recordCapacityBytes = 0U;
    stats_.materialCapacityBytes = 0U;
}

} // namespace dve::render
