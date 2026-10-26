#include "dve/render/rdv_bbv.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dve::render::rdv {
namespace {

int wrap(std::int64_t coordinate, int dimension) noexcept {
    const auto value = coordinate % dimension;
    return static_cast<int>(value < 0 ? value + dimension : value);
}

std::int64_t key_axis(BrickKey key, int axis) noexcept {
    return axis == 0 ? key.x : axis == 1 ? key.y : key.z;
}
std::int64_t int_axis(Int3 key, int axis) noexcept {
    return axis == 0 ? key.x : axis == 1 ? key.y : key.z;
}
float float_axis(Float3 p, int axis) noexcept {
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}
void set_axis(Int3& p, int axis, std::int32_t value) noexcept {
    if (axis == 0) p.x = value;
    else if (axis == 1) p.y = value;
    else p.z = value;
}
int floor_div_8(int x) noexcept {
    const int quotient = x / 8;
    return quotient - static_cast<int>(x % 8 < 0);
}
bool has_any_occupancy(const BbvBrick& brick) noexcept {
    for (auto word : brick.occupancy) if (word != 0U) return true;
    return false;
}
bool valid_finite(Float3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

} // namespace

BbvClipmap::BbvClipmap(Int3 dimensions) : dimensions_(dimensions) {
    if (dimensions.x < 1 || dimensions.y < 1 || dimensions.z < 1 ||
        dimensions.x > 64 || dimensions.y > 64 || dimensions.z > 64) {
        throw std::invalid_argument("BBV clipmap dimensions must lie in [1,64] per axis");
    }
    const auto count = static_cast<std::size_t>(dimensions.x) *
                       static_cast<std::size_t>(dimensions.y) *
                       static_cast<std::size_t>(dimensions.z);
    physicalKeys_.resize(count);
    bricks_.resize(count);
    dirty_.assign(count, 0U);
}

std::optional<std::size_t> BbvClipmap::physical_index(BrickKey key) const noexcept {
    if (!initialized_) return std::nullopt;
    const auto x = key_axis(key, 0) - key_axis(minimum_, 0);
    const auto y = key_axis(key, 1) - key_axis(minimum_, 1);
    const auto z = key_axis(key, 2) - key_axis(minimum_, 2);
    if (x < 0 || y < 0 || z < 0 ||
        x >= dimensions_.x || y >= dimensions_.y || z >= dimensions_.z) return std::nullopt;
    const auto px = static_cast<std::size_t>(wrap(x + offset_.x, dimensions_.x));
    const auto py = static_cast<std::size_t>(wrap(y + offset_.y, dimensions_.y));
    const auto pz = static_cast<std::size_t>(wrap(z + offset_.z, dimensions_.z));
    return px + static_cast<std::size_t>(dimensions_.x) *
        (py + static_cast<std::size_t>(dimensions_.y) * pz);
}

bool BbvClipmap::publish_key(BrickKey key, const PackedBrickmapScene& source,
                             bool forceDirty) noexcept {
    const auto slot = physical_index(key);
    if (!slot) return false;
    BbvBrick replacement;
    if (const auto* record = source.find_record(key)) replacement.occupancy = record->occupancy32;
    const bool changed = physicalKeys_[*slot] != key || bricks_[*slot].occupancy != replacement.occupancy;
    physicalKeys_[*slot] = key;
    bricks_[*slot] = replacement;
    if (changed || forceDirty) dirty_[*slot] = 1U;
    return changed || forceDirty;
}

BbvUpdateStats BbvClipmap::recenter(BrickKey center, const PackedBrickmapScene& source) {
    const auto make_start = [](std::int64_t value, int width) -> std::int32_t {
        const auto start = value - width / 2;
        // The shared voxel-query interface uses int32 global voxel coordinates.
        constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) / 8 + 1;
        constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) / 8 - 64;
        if (start < minimum || start > maximum)
            throw std::out_of_range("BBV clipmap center exceeds safe voxel-index range");
        return static_cast<std::int32_t>(start);
    };
    const BrickKey nextMin{make_start(center.x, dimensions_.x),
                           make_start(center.y, dimensions_.y),
                           make_start(center.z, dimensions_.z)};
    BbvUpdateStats stats;
    if (initialized_ && nextMin == minimum_) {
        stats.dirtyBricks = dirty_count();
        return stats;
    }
    const bool initial = !initialized_;
    if (!initial) {
        offset_.x = wrap(static_cast<std::int64_t>(offset_.x) +
                             static_cast<std::int64_t>(nextMin.x) - minimum_.x, dimensions_.x);
        offset_.y = wrap(static_cast<std::int64_t>(offset_.y) +
                             static_cast<std::int64_t>(nextMin.y) - minimum_.y, dimensions_.y);
        offset_.z = wrap(static_cast<std::int64_t>(offset_.z) +
                             static_cast<std::int64_t>(nextMin.z) - minimum_.z, dimensions_.z);
    }
    minimum_ = nextMin;
    initialized_ = true;
    stats.recentered = true;
    stats.fullRefresh = initial;
    for (int z = 0; z < dimensions_.z; ++z)
        for (int y = 0; y < dimensions_.y; ++y)
            for (int x = 0; x < dimensions_.x; ++x) {
                BrickKey key{minimum_.x + x, minimum_.y + y, minimum_.z + z};
                const auto index = physical_index(key);
                if (!initial && index && physicalKeys_[*index] == key) continue;
                ++stats.examinedBricks;
                if (publish_key(key, source, initial)) ++stats.changedBricks;
            }
    stats.dirtyBricks = dirty_count();
    return stats;
}

BbvUpdateStats BbvClipmap::recenter_camera(Float3 cameraWorldMeters,
                                              float metresPerVoxel,
                                              const PackedBrickmapScene& source) {
    if (!valid_finite(cameraWorldMeters) || !std::isfinite(metresPerVoxel) ||
        !(metresPerVoxel > 0.0F)) {
        throw std::invalid_argument("BBV camera position and metres-per-voxel must be finite");
    }
    const double brickMetres = 8.0 * static_cast<double>(metresPerVoxel);
    const auto key = [&](float coordinate) -> std::int32_t {
        const double v = std::floor(static_cast<double>(coordinate) / brickMetres);
        if (v < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            v > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            throw std::out_of_range("BBV camera position exceeds brick-key range");
        return static_cast<std::int32_t>(v);
    };
    return recenter({key(cameraWorldMeters.x), key(cameraWorldMeters.y),
                     key(cameraWorldMeters.z)}, source);
}

BbvUpdateStats BbvClipmap::refresh(std::span<const BrickKey> keys,
                                    const PackedBrickmapScene& source) {
    BbvUpdateStats stats;
    for (BrickKey key : keys) {
        if (!physical_index(key)) continue;
        ++stats.examinedBricks;
        if (publish_key(key, source, false)) ++stats.changedBricks;
    }
    stats.dirtyBricks = dirty_count();
    return stats;
}

BbvUpdateStats BbvClipmap::refresh_all(const PackedBrickmapScene& source) {
    BbvUpdateStats stats;
    if (!initialized_) return stats;
    stats.fullRefresh = true;
    for (int z = 0; z < dimensions_.z; ++z)
        for (int y = 0; y < dimensions_.y; ++y)
            for (int x = 0; x < dimensions_.x; ++x) {
                const BrickKey key{minimum_.x + x, minimum_.y + y, minimum_.z + z};
                ++stats.examinedBricks;
                if (publish_key(key, source, false)) ++stats.changedBricks;
            }
    stats.dirtyBricks = dirty_count();
    return stats;
}

void BbvClipmap::mark_all_dirty() noexcept {
    std::fill(dirty_.begin(), dirty_.end(), 1U);
}
void BbvClipmap::clear_dirty() noexcept {
    std::fill(dirty_.begin(), dirty_.end(), 0U);
}
std::size_t BbvClipmap::dirty_count() const noexcept {
    return static_cast<std::size_t>(std::count(dirty_.begin(), dirty_.end(), std::uint8_t{1U}));
}
std::vector<std::size_t> BbvClipmap::dirty_indices() const {
    std::vector<std::size_t> result;
    result.reserve(dirty_count());
    for (std::size_t i = 0; i < dirty_.size(); ++i)
        if (dirty_[i]) result.push_back(i);
    return result;
}

const BbvBrick* BbvClipmap::find_brick(BrickKey key) const noexcept {
    const auto index = physical_index(key);
    return index && physicalKeys_[*index] == key ? &bricks_[*index] : nullptr;
}
bool BbvClipmap::occupied_at(Int3 voxel) const noexcept {
    const BrickKey key{floor_div_8(voxel.x), floor_div_8(voxel.y), floor_div_8(voxel.z)};
    const auto* brick = find_brick(key);
    if (!brick || !has_any_occupancy(*brick)) return false;
    const unsigned localX = static_cast<unsigned>(voxel.x - key.x * 8);
    const unsigned localY = static_cast<unsigned>(voxel.y - key.y * 8);
    const unsigned localZ = static_cast<unsigned>(voxel.z - key.z * 8);
    const unsigned bit = localX + 8U * localY + 64U * localZ;
    return (brick->occupancy[bit >> 5U] & (1U << (bit & 31U))) != 0U;
}

std::optional<BbvHit> BbvClipmap::trace(Float3 origin, Float3 direction,
                                        float maxDistance) const noexcept {
    if (!initialized_ || !valid_finite(origin) || !valid_finite(direction) ||
        !std::isfinite(maxDistance) || maxDistance < 0.0F) return std::nullopt;
    const float magnitude = std::sqrt(direction.x * direction.x +
                                      direction.y * direction.y + direction.z * direction.z);
    if (!(magnitude > 0.0F) || !std::isfinite(magnitude)) return std::nullopt;
    const Float3 unit{direction.x / magnitude, direction.y / magnitude, direction.z / magnitude};
    const std::array<float, 3> low{minimum_.x * 8.0F, minimum_.y * 8.0F, minimum_.z * 8.0F};
    const std::array<float, 3> high{
        (static_cast<float>(minimum_.x) + dimensions_.x) * 8.0F,
        (static_cast<float>(minimum_.y) + dimensions_.y) * 8.0F,
        (static_cast<float>(minimum_.z) + dimensions_.z) * 8.0F};
    float entry = 0.0F;
    float exit = maxDistance;
    Int3 entryNormal{};
    for (int axis = 0; axis < 3; ++axis) {
        const float p = float_axis(origin, axis);
        const float d = float_axis(unit, axis);
        if (d == 0.0F) {
            if (p < low[axis] || p >= high[axis]) return std::nullopt;
            continue;
        }
        const float t0 = (low[axis] - p) / d;
        const float t1 = (high[axis] - p) / d;
        const float nearT = std::min(t0, t1);
        const float farT = std::max(t0, t1);
        if (nearT > entry) {
            entry = nearT;
            entryNormal = {};
            set_axis(entryNormal, axis, d > 0.0F ? -1 : 1);
        }
        exit = std::min(exit, farT);
        if (entry > exit) return std::nullopt;
    }
    if (entry > maxDistance) return std::nullopt;
    const float start = entry == 0.0F ? 0.0F : std::nextafter(entry, INFINITY);
    Int3 voxel{};
    for (int axis = 0; axis < 3; ++axis) {
        const float p = float_axis(origin, axis) + float_axis(unit, axis) * start;
        const float nudged = entry > 0.0F ?
            std::nextafter(p, float_axis(unit, axis) >= 0.0F ? INFINITY : -INFINITY) : p;
        const float bounded = std::clamp(nudged, low[axis], std::nextafter(high[axis], -INFINITY));
        set_axis(voxel, axis, static_cast<std::int32_t>(std::floor(bounded)));
    }

    float t = entry;
    Int3 normal = entryNormal;
    // Each iteration enters a new voxel; clipmap has at most 512 fine cells per axis.
    for (std::uint32_t iterations = 0; iterations < 65536U && t <= exit; ++iterations) {
        const BrickKey brickKey{floor_div_8(voxel.x), floor_div_8(voxel.y), floor_div_8(voxel.z)};
        if (!find_brick(brickKey)) break;
        if (occupied_at(voxel)) return BbvHit{voxel, normal, t};

        int axis = -1;
        float next = INFINITY;
        for (int candidate = 0; candidate < 3; ++candidate) {
            const float d = float_axis(unit, candidate);
            if (d == 0.0F) continue;
            const int coordinate = static_cast<int>(int_axis(voxel, candidate));
            const float boundary = static_cast<float>(coordinate + (d > 0.0F ? 1 : 0));
            const float candidateT = (boundary - float_axis(origin, candidate)) / d;
            if (candidateT < next) { next = candidateT; axis = candidate; }
        }
        if (axis < 0 || !std::isfinite(next) || next > exit || next > maxDistance) break;
        if (next + 1.0e-5F < t) break; // reject floating-point non-progress
        t = std::max(t, next);
        const int delta = float_axis(unit, axis) > 0.0F ? 1 : -1;
        const auto old = static_cast<std::int32_t>(int_axis(voxel, axis));
        set_axis(voxel, axis, old + delta);
        normal = {};
        set_axis(normal, axis, -delta);
    }
    return std::nullopt;
}

BbvRhiMirror::~BbvRhiMirror() { reset(); }

bool BbvRhiMirror::publish(BbvClipmap& clipmap, std::string* error) {
    if (!clipmap.initialized()) {
        set_error(error, "BBV clipmap must be centered before GPU publication");
        return false;
    }
    const std::size_t bytes = clipmap.bytes();
    rhi::BufferHandle target = buffer_;
    const bool replace = !target || capacity_ != bytes;
    if (replace) {
        rhi::BufferDesc desc;
        desc.bytes = bytes;
        desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource |
                     rhi::BufferUsage::CopyDestination;
        desc.memory = rhi::MemoryDomain::DeviceLocal;
        desc.initialState = rhi::ResourceState::ShaderRead;
        desc.debugName = "RDV BBV clipmap occupancy";
        target = device_.create_buffer(desc, error);
        if (!target) return false;
    }
    const auto dirty = replace ? std::vector<std::size_t>{} : clipmap.dirty_indices();
    const auto bytesSpan = std::as_bytes(clipmap.bricks());
    std::uint64_t newBytes = 0U, newRanges = 0U;
    bool valid = true;
    if (replace) {
        valid = device_.write_buffer(target, 0U, bytesSpan, error);
        if (valid) { newBytes = bytes; newRanges = 1U; }
    } else {
        for (std::size_t i = 0; i < dirty.size() && valid;) {
            const auto begin = dirty[i];
            auto end = begin + 1U;
            while (++i < dirty.size() && dirty[i] == end) ++end;
            const auto from = begin * sizeof(BbvBrick);
            const auto count = (end - begin) * sizeof(BbvBrick);
            valid = device_.write_buffer(target, from, bytesSpan.subspan(from, count), error);
            if (valid) { newBytes += count; ++newRanges; }
        }
    }
    if (!valid) {
        if (replace) { std::string ignored; device_.destroy_buffer(target, &ignored); }
        return false;
    }
    if (replace) {
        const auto old = buffer_;
        buffer_ = target;
        capacity_ = bytes;
        if (old) { std::string ignored; device_.destroy_buffer(old, &ignored); }
    }
    bytesUploaded_ += newBytes;
    uploadRanges_ += newRanges;
    clipmap.clear_dirty();
    return true;
}

bool BbvRhiMirror::readback_matches(const BbvClipmap& clipmap, std::string* error) const {
    if (!buffer_ || capacity_ != clipmap.bytes()) {
        set_error(error, "BBV mirror has no matching allocation");
        return false;
    }
    std::vector<std::byte> observed(capacity_);
    if (!device_.read_buffer(buffer_, 0U, observed, error)) return false;
    const auto expected = std::as_bytes(clipmap.bricks());
    if (!std::equal(observed.begin(), observed.end(), expected.begin(), expected.end())) {
        set_error(error, "BBV GPU occupancy readback differs from the clipmap");
        return false;
    }
    return true;
}

void BbvRhiMirror::reset() noexcept {
    if (buffer_) {
        std::string ignored;
        device_.destroy_buffer(buffer_, &ignored);
    }
    buffer_ = {};
    capacity_ = 0U;
}

bool execute_bbv_gpu_trace(rhi::IDevice& device, BbvRhiMirror& mirror,
                           BbvClipmap& clipmap, std::span<const GpuTraceRay> rays,
                           std::span<const std::byte> bytecode,
                           std::vector<BbvGpuHit>& results, std::string* error) {
    results.clear();
    if (rays.empty() || rays.size() > 1000000U || bytecode.empty()) {
        set_error(error, "BBV trace requires 1..1,000,000 rays and nonempty shader bytecode");
        return false;
    }
    if (!mirror.publish(clipmap, error)) return false;
    BbvTraceConstants constants;
    const BrickKey minimum = clipmap.minimum_key();
    constants.minimumBrick = {minimum.x, minimum.y, minimum.z};
    constants.rayCount = static_cast<std::uint32_t>(rays.size());
    constants.gridDimensions = clipmap.dimensions();
    constants.toroidalOffset = clipmap.toroidal_offset();

    rhi::BufferHandle rayBuffer{}, hitBuffer{}, constantBuffer{};
    rhi::BindGroupLayoutHandle layout{};
    rhi::BindGroupHandle group{};
    rhi::ComputePipelineHandle pipeline{};
    const auto cleanup = [&]() {
        device.wait_idle();
        if (pipeline) (void)device.destroy_compute_pipeline(pipeline);
        if (group) (void)device.destroy_bind_group(group);
        if (layout) (void)device.destroy_bind_group_layout(layout);
        if (constantBuffer) (void)device.destroy_buffer(constantBuffer);
        if (hitBuffer) (void)device.destroy_buffer(hitBuffer);
        if (rayBuffer) (void)device.destroy_buffer(rayBuffer);
    };
    const std::size_t hitBytes = rays.size() * sizeof(BbvGpuHit);
    const auto storage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource |
                         rhi::BufferUsage::CopyDestination;
    rayBuffer = device.create_buffer({rays.size_bytes(), storage,
        rhi::MemoryDomain::Upload, "BBV trace rays", rhi::ResourceState::ShaderRead}, error);
    hitBuffer = device.create_buffer({hitBytes, storage,
        rhi::MemoryDomain::Readback, "BBV trace results", rhi::ResourceState::ShaderWrite}, error);
    constantBuffer = device.create_buffer({sizeof(constants),
        rhi::BufferUsage::Constant | rhi::BufferUsage::CopyDestination,
        rhi::MemoryDomain::Upload, "BBV trace constants", rhi::ResourceState::ShaderRead}, error);
    if (!rayBuffer || !hitBuffer || !constantBuffer ||
        !device.write_buffer(rayBuffer, 0U, std::as_bytes(rays), error) ||
        !device.write_buffer(constantBuffer, 0U, std::as_bytes(std::span(&constants, 1)), error)) {
        cleanup();
        return false;
    }
    // Matches shaders/compiled/ao convention:
    // b0 => binding 0, t0/t1 => 16/17, u0 => 32 in descriptor set 0.
    rhi::BindGroupLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {0U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Compute},
        {16U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Compute},
        {17U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Compute},
        {32U, rhi::BindingType::StorageBufferReadWrite, rhi::ShaderStage::Compute}
    };
    layout = device.create_bind_group_layout(layoutDesc, error);
    if (!layout) { cleanup(); return false; }
    rhi::BindGroupDesc bindDesc;
    bindDesc.layout = layout;
    bindDesc.entries = {
        {0U, constantBuffer, {}, 0U, sizeof(constants), {}},
        {16U, mirror.buffer(), {}, 0U, clipmap.bytes(), {}},
        {17U, rayBuffer, {}, 0U, rays.size_bytes(), {}},
        {32U, hitBuffer, {}, 0U, hitBytes, {}}
    };
    group = device.create_bind_group(bindDesc, error);
    if (!group) { cleanup(); return false; }
    rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.debugName = "RdvBbvTrace";
    pipelineDesc.bytecode.assign(bytecode.begin(), bytecode.end());
    pipelineDesc.bindGroupLayouts.push_back(layout);
    pipelineDesc.threadsX = 64U;
    pipeline = device.create_compute_pipeline(pipelineDesc, error);
    if (!pipeline) { cleanup(); return false; }
    auto commands = device.begin_commands(rhi::QueueKind::Compute, "RdvBbvTrace", error);
    if (!commands || !device.bind_compute_bind_group(commands, 0U, group, error) ||
        !device.dispatch(commands, pipeline,
                         static_cast<std::uint32_t>((rays.size() + 63U) / 64U),
                         1U, 1U, error)) {
        cleanup();
        return false;
    }
    const auto fence = device.submit(commands, error);
    if (!fence || !device.wait(fence, error)) { cleanup(); return false; }
    results.resize(rays.size());
    const bool ok = device.read_buffer(hitBuffer, 0U,
                                       std::as_writable_bytes(std::span(results)), error);
    if (!ok) results.clear();
    cleanup();
    return ok;
}

} // namespace dve::render::rdv
