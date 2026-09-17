#include "dve/jolt_replay.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

namespace dve {
namespace {
constexpr std::array<char, 8> kMagic{{'D','V','E','J','R','P','L','Y'}};
constexpr std::uint32_t kVersion = 1U;
constexpr std::uint64_t kMaximumElementCount = 100'000'000ULL;

template <typename T>
void write_scalar(std::ostream& stream, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool read_scalar(std::istream& stream, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

void write_float3(std::ostream& stream, Float3 value) {
    write_scalar(stream, value.x); write_scalar(stream, value.y); write_scalar(stream, value.z);
}

bool read_float3(std::istream& stream, Float3& value) {
    return read_scalar(stream, value.x) && read_scalar(stream, value.y) && read_scalar(stream, value.z);
}

void write_quaternion(std::ostream& stream, Quaternion value) {
    write_scalar(stream, value.x); write_scalar(stream, value.y); write_scalar(stream, value.z); write_scalar(stream, value.w);
}

bool read_quaternion(std::istream& stream, Quaternion& value) {
    return read_scalar(stream, value.x) && read_scalar(stream, value.y) &&
        read_scalar(stream, value.z) && read_scalar(stream, value.w);
}

void write_transform(std::ostream& stream, const RigidTransform& value) {
    write_float3(stream, value.position); write_quaternion(stream, value.rotation);
}

bool read_transform(std::istream& stream, RigidTransform& value) {
    return read_float3(stream, value.position) && read_quaternion(stream, value.rotation);
}

void write_body_state(std::ostream& stream, const RigidBodyState& state) {
    write_transform(stream, state.previousTransform);
    write_transform(stream, state.currentTransform);
    write_float3(stream, state.linearVelocity);
    write_float3(stream, state.angularVelocity);
    const std::uint8_t sleeping = state.sleeping ? 1U : 0U;
    write_scalar(stream, sleeping);
}

bool read_body_state(std::istream& stream, RigidBodyState& state) {
    std::uint8_t sleeping{};
    if (!read_transform(stream, state.previousTransform) || !read_transform(stream, state.currentTransform) ||
        !read_float3(stream, state.linearVelocity) || !read_float3(stream, state.angularVelocity) ||
        !read_scalar(stream, sleeping)) return false;
    state.sleeping = sleeping != 0U;
    return true;
}

void write_character_state(std::ostream& stream, const JoltVirtualCharacterState& state) {
    write_transform(stream, state.transform);
    write_float3(stream, state.linearVelocity);
    write_float3(stream, state.groundVelocity);
    write_float3(stream, state.groundNormal);
    write_scalar(stream, state.groundBody);
    write_scalar(stream, static_cast<std::uint8_t>(state.groundState));
}

bool read_character_state(std::istream& stream, JoltVirtualCharacterState& state) {
    std::uint8_t groundState{};
    if (!read_transform(stream, state.transform) || !read_float3(stream, state.linearVelocity) ||
        !read_float3(stream, state.groundVelocity) || !read_float3(stream, state.groundNormal) ||
        !read_scalar(stream, state.groundBody) || !read_scalar(stream, groundState)) return false;
    if (groundState > static_cast<std::uint8_t>(JoltCharacterGroundState::NotSupported)) return false;
    state.groundState = static_cast<JoltCharacterGroundState>(groundState);
    return true;
}

template <typename T, typename Writer>
void write_vector(std::ostream& stream, const std::vector<T>& values, Writer writer) {
    const std::uint64_t count = values.size();
    write_scalar(stream, count);
    for (const T& value : values) writer(stream, value);
}

template <typename T, typename Reader>
bool read_vector(std::istream& stream, std::vector<T>& values, Reader reader) {
    std::uint64_t count{};
    if (!read_scalar(stream, count) || count > kMaximumElementCount ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
    try { values.resize(static_cast<std::size_t>(count)); } catch (...) { return false; }
    for (T& value : values) if (!reader(stream, value)) return false;
    return true;
}

void write_bytes(std::ostream& stream, const std::vector<std::byte>& bytes) {
    const std::uint64_t count = bytes.size();
    write_scalar(stream, count);
    if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool read_bytes(std::istream& stream, std::vector<std::byte>& bytes) {
    std::uint64_t count{};
    if (!read_scalar(stream, count) || count > kMaximumElementCount ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
    try { bytes.resize(static_cast<std::size_t>(count)); } catch (...) { return false; }
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

void write_mask(std::ostream& stream, const std::vector<std::uint8_t>& mask) {
    const std::uint64_t count = mask.size();
    write_scalar(stream, count);
    if (!mask.empty()) stream.write(reinterpret_cast<const char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
}

bool read_mask(std::istream& stream, std::vector<std::uint8_t>& mask) {
    std::uint64_t count{};
    if (!read_scalar(stream, count) || count > kMaximumElementCount ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
    try { mask.resize(static_cast<std::size_t>(count)); } catch (...) { return false; }
    if (!mask.empty()) stream.read(reinterpret_cast<char*>(mask.data()), static_cast<std::streamsize>(mask.size()));
    if (!stream) return false;
    for (std::uint8_t value : mask) if (value > 1U) return false;
    return true;
}

void write_snapshot(std::ostream& stream, const JoltWorldSnapshot& snapshot) {
    write_bytes(stream, snapshot.solverBytes);
    write_vector(stream, snapshot.bodyStates, [](std::ostream& out, const RigidBodyState& value) { write_body_state(out, value); });
    write_mask(stream, snapshot.liveBodyMask);
    write_vector(stream, snapshot.characterStates, [](std::ostream& out, const JoltVirtualCharacterState& value) { write_character_state(out, value); });
    write_bytes(stream, snapshot.characterSolverBytes);
    write_mask(stream, snapshot.liveCharacterMask);
    write_mask(stream, snapshot.liveSoftBodyMask);
    write_mask(stream, snapshot.liveVehicleMask);
    write_scalar(stream, snapshot.contactTimeSeconds);
}

bool read_snapshot(std::istream& stream, JoltWorldSnapshot& snapshot) {
    return read_bytes(stream, snapshot.solverBytes) &&
        read_vector(stream, snapshot.bodyStates, [](std::istream& in, RigidBodyState& value) { return read_body_state(in, value); }) &&
        read_mask(stream, snapshot.liveBodyMask) &&
        read_vector(stream, snapshot.characterStates, [](std::istream& in, JoltVirtualCharacterState& value) { return read_character_state(in, value); }) &&
        read_bytes(stream, snapshot.characterSolverBytes) &&
        read_mask(stream, snapshot.liveCharacterMask) &&
        read_mask(stream, snapshot.liveSoftBodyMask) &&
        read_mask(stream, snapshot.liveVehicleMask) &&
        read_scalar(stream, snapshot.contactTimeSeconds);
}
}

bool JoltReplayTrack::capture(const JoltRigidBodyWorld& world, std::uint64_t simulationFrame) {
    const auto snapshot = world.save_snapshot();
    if (!snapshot) return false;
    try { frames_.push_back({simulationFrame, *snapshot}); } catch (...) { return false; }
    return true;
}

bool JoltReplayTrack::restore(JoltRigidBodyWorld& world, std::size_t frameIndex) const {
    return frameIndex < frames_.size() && world.restore_snapshot(frames_[frameIndex].snapshot);
}

bool JoltReplayTrack::write(const std::filesystem::path& path, std::string* error) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "could not open replay for writing"; return false; }
    stream.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_scalar(stream, kVersion);
    const std::uint64_t count = frames_.size();
    write_scalar(stream, count);
    for (const JoltReplayFrame& frame : frames_) {
        write_scalar(stream, frame.simulationFrame);
        write_snapshot(stream, frame.snapshot);
    }
    if (!stream) { if (error) *error = "failed while writing replay"; return false; }
    if (error) error->clear();
    return true;
}

JoltReplayReadResult JoltReplayTrack::read(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    JoltReplayReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) { result.error = ec ? "could not stat replay" : "replay exceeds size limit"; return result; }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open replay"; return result; }
    std::array<char, 8> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t version{};
    std::uint64_t count{};
    if (!stream || magic != kMagic || !read_scalar(stream, version) || version != kVersion ||
        !read_scalar(stream, count) || count > 1'000'000ULL) {
        result.error = "invalid or unsupported replay header";
        return result;
    }
    try { result.frames.resize(static_cast<std::size_t>(count)); } catch (...) { result.error = "replay allocation failed"; return result; }
    for (JoltReplayFrame& frame : result.frames) {
        if (!read_scalar(stream, frame.simulationFrame) || !read_snapshot(stream, frame.snapshot)) {
            result.error = "truncated or malformed replay";
            result.frames.clear();
            return result;
        }
    }
    return result;
}

} // namespace dve
