#include "dve/sprite_animation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr float kDegreesToRadians = 0.01745329251994329577F;
constexpr std::size_t kMaximumMachineEntries = 65536U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template<class T> void hash_integer(std::uint64_t& hash, T value) noexcept {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        hash_byte(hash, static_cast<std::uint8_t>(
            (bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
    }
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (char raw : value)
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
}

void hash_parameter_value(std::uint64_t& hash,
                          const SpriteAnimationParameterValue& value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value.type));
    hash_byte(hash, value.booleanValue ? 1U : 0U);
    hash_integer(hash, value.integerValue);
    hash_float(hash, value.floatValue);
}

bool finite_parameter(const SpriteAnimationParameterValue& value) noexcept {
    return static_cast<unsigned>(value.type) <=
               static_cast<unsigned>(SpriteAnimationParameterType::Trigger) &&
        std::isfinite(value.floatValue);
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes,
                  std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create sprite state-machine directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary sprite state-machine asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete sprite state-machine asset");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (!ec) return true;
    std::filesystem::remove(temporary, ec);
    return fail(error, "could not publish sprite state-machine asset: " + ec.message());
}

const SpriteAnimationState* find_state(const SpriteAnimationStateMachineAsset& machine,
                                       std::string_view name) noexcept {
    const auto found = std::find_if(machine.states.begin(), machine.states.end(),
        [name](const SpriteAnimationState& state) { return state.name == name; });
    return found == machine.states.end() ? nullptr : &*found;
}

const SpriteAnimationParameterDefinition* find_parameter_definition(
    const SpriteAnimationStateMachineAsset& machine, std::string_view name) noexcept {
    const auto found = std::find_if(machine.parameters.begin(), machine.parameters.end(),
        [name](const SpriteAnimationParameterDefinition& parameter) {
            return parameter.name == name;
        });
    return found == machine.parameters.end() ? nullptr : &*found;
}

bool operation_valid_for_type(SpriteAnimationCompareOp operation,
                              SpriteAnimationParameterType type) noexcept {
    switch (type) {
    case SpriteAnimationParameterType::Boolean:
        return operation == SpriteAnimationCompareOp::IsTrue ||
            operation == SpriteAnimationCompareOp::IsFalse ||
            operation == SpriteAnimationCompareOp::Equal ||
            operation == SpriteAnimationCompareOp::NotEqual;
    case SpriteAnimationParameterType::Integer:
    case SpriteAnimationParameterType::Float:
        return operation == SpriteAnimationCompareOp::Equal ||
            operation == SpriteAnimationCompareOp::NotEqual ||
            operation == SpriteAnimationCompareOp::Greater ||
            operation == SpriteAnimationCompareOp::GreaterEqual ||
            operation == SpriteAnimationCompareOp::Less ||
            operation == SpriteAnimationCompareOp::LessEqual;
    case SpriteAnimationParameterType::Trigger:
        return operation == SpriteAnimationCompareOp::Triggered ||
            operation == SpriteAnimationCompareOp::NotTriggered;
    }
    return false;
}

Float3 root_axis(GameplayPlane2D plane) noexcept {
    return plane == GameplayPlane2D::XY ? Float3{0.0F, 0.0F, 1.0F}
                                       : Float3{0.0F, 1.0F, 0.0F};
}

TileVec2 world_translation_to_pixels(const SpriteRootMotionDelta& delta,
                                     const SpriteRootMotionApplyOptions& options) noexcept {
    const float scale = options.pixelsPerWorldUnit * options.translationScale;
    if (options.plane == GameplayPlane2D::XY)
        return {delta.worldTranslation.x * scale, delta.worldTranslation.y * scale};
    return {delta.worldTranslation.x * scale, delta.worldTranslation.z * scale};
}

bool validate_root_options(const SpriteRootMotionApplyOptions& options,
                           std::string* error) {
    if (static_cast<unsigned>(options.mode) >
            static_cast<unsigned>(SpriteRootMotionApplyMode::Velocity) ||
        static_cast<unsigned>(options.plane) > static_cast<unsigned>(GameplayPlane2D::XZ) ||
        !std::isfinite(options.pixelsPerWorldUnit) || options.pixelsPerWorldUnit <= 0.0F ||
        !std::isfinite(options.deltaSeconds) || options.deltaSeconds <= 0.0F ||
        !std::isfinite(options.translationScale)) {
        return fail(error, "sprite root-motion application options are invalid");
    }
    return true;
}

std::uint64_t legacy_machine_hash_v1(const SpriteAnimationStateMachineAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_integer(hash, asset.compatibleSpriteAssetHash);
    hash_string(hash, asset.initialState);
    hash_integer(hash, static_cast<std::uint64_t>(asset.parameters.size()));
    for (const SpriteAnimationParameterDefinition& parameter : asset.parameters) {
        hash_string(hash, parameter.name);
        hash_parameter_value(hash, parameter.defaultValue);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.states.size()));
    for (const SpriteAnimationState& state : asset.states) {
        hash_string(hash, state.name);
        hash_string(hash, state.clip);
        hash_float(hash, state.playbackSpeed);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.transitions.size()));
    for (const SpriteAnimationTransition& transition : asset.transitions) {
        hash_string(hash, transition.fromState);
        hash_string(hash, transition.toState);
        hash_integer(hash, transition.priority);
        hash_byte(hash, transition.hasExitTime ? 1U : 0U);
        hash_float(hash, transition.exitNormalizedTime);
        hash_float(hash, transition.minimumStateTimeSeconds);
        hash_byte(hash, transition.resetTime ? 1U : 0U);
        hash_byte(hash, transition.consumeTriggers ? 1U : 0U);
        hash_integer(hash, static_cast<std::uint64_t>(transition.conditions.size()));
        for (const SpriteAnimationCondition& condition : transition.conditions) {
            hash_string(hash, condition.parameter);
            hash_byte(hash, static_cast<std::uint8_t>(condition.operation));
            hash_parameter_value(hash, condition.comparison);
        }
    }
    return hash;
}

std::uint64_t legacy_machine_hash_v2(const SpriteAnimationStateMachineAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_integer(hash, asset.compatibleSpriteAssetHash);
    hash_string(hash, asset.initialState);
    hash_integer(hash, static_cast<std::uint64_t>(asset.parameters.size()));
    for (const SpriteAnimationParameterDefinition& parameter : asset.parameters) {
        hash_string(hash, parameter.name);
        hash_parameter_value(hash, parameter.defaultValue);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.states.size()));
    for (const SpriteAnimationState& state : asset.states) {
        hash_string(hash, state.name);
        hash_string(hash, state.clip);
        hash_float(hash, state.playbackSpeed);
        hash_float(hash, state.graphPosition.x);
        hash_float(hash, state.graphPosition.y);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.transitions.size()));
    for (const SpriteAnimationTransition& transition : asset.transitions) {
        hash_string(hash, transition.fromState);
        hash_string(hash, transition.toState);
        hash_integer(hash, transition.priority);
        hash_byte(hash, transition.hasExitTime ? 1U : 0U);
        hash_float(hash, transition.exitNormalizedTime);
        hash_float(hash, transition.minimumStateTimeSeconds);
        hash_byte(hash, transition.resetTime ? 1U : 0U);
        hash_byte(hash, transition.consumeTriggers ? 1U : 0U);
        hash_float(hash, transition.blendDurationSeconds);
        hash_byte(hash, transition.synchronizeNormalizedTime ? 1U : 0U);
        hash_byte(hash, static_cast<std::uint8_t>(transition.interruptionSource));
        hash_integer(hash, static_cast<std::uint64_t>(transition.conditions.size()));
        for (const SpriteAnimationCondition& condition : transition.conditions) {
            hash_string(hash, condition.parameter);
            hash_byte(hash, static_cast<std::uint8_t>(condition.operation));
            hash_parameter_value(hash, condition.comparison);
        }
    }
    return hash;
}

float length(TileVec2 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y);
}

TileVec2 add_tile(TileVec2 left, TileVec2 right) noexcept {
    return {left.x + right.x, left.y + right.y};
}

TileVec2 multiply_tile(TileVec2 value, float scale) noexcept {
    return {value.x * scale, value.y * scale};
}

float dot_tile(TileVec2 left, TileVec2 right) noexcept {
    return left.x * right.x + left.y * right.y;
}

void translate_query_shape(Physics2DQueryShape& shape, TileVec2 translation) {
    shape.centerPixels = add_tile(shape.centerPixels, translation);
    shape.capsulePoint1Pixels = add_tile(shape.capsulePoint1Pixels, translation);
    shape.capsulePoint2Pixels = add_tile(shape.capsulePoint2Pixels, translation);
    for (TileVec2& vertex : shape.verticesPixels) vertex = add_tile(vertex, translation);
}

SpriteSideEffectCommandId nonzero_hash(std::uint64_t hash) noexcept {
    return hash == kInvalidSpriteSideEffectCommandId ? 1U : hash;
}

} // namespace

float SpriteAnimationBlendSnapshot::destination_weight() const noexcept {
    if (!active || durationSeconds <= 0.0F) return 1.0F;
    const float value = std::clamp(elapsedSeconds / durationSeconds, 0.0F, 1.0F);
    switch (curve) {
    case SpriteAnimationBlendCurve::Linear: return value;
    case SpriteAnimationBlendCurve::SmoothStep: return value * value * (3.0F - 2.0F * value);
    case SpriteAnimationBlendCurve::EaseIn: return value * value;
    case SpriteAnimationBlendCurve::EaseOut: {
        const float inverse = 1.0F - value;
        return 1.0F - inverse * inverse;
    }
    }
    return value;
}

std::uint64_t sprite_animation_state_machine_content_hash(
    const SpriteAnimationStateMachineAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_integer(hash, asset.compatibleSpriteAssetHash);
    hash_string(hash, asset.initialState);
    hash_integer(hash, static_cast<std::uint64_t>(asset.parameters.size()));
    for (const SpriteAnimationParameterDefinition& parameter : asset.parameters) {
        hash_string(hash, parameter.name);
        hash_parameter_value(hash, parameter.defaultValue);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.states.size()));
    for (const SpriteAnimationState& state : asset.states) {
        hash_string(hash, state.name);
        hash_string(hash, state.clip);
        hash_float(hash, state.playbackSpeed);
        hash_float(hash, state.graphPosition.x);
        hash_float(hash, state.graphPosition.y);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.transitions.size()));
    for (const SpriteAnimationTransition& transition : asset.transitions) {
        hash_string(hash, transition.fromState);
        hash_string(hash, transition.toState);
        hash_integer(hash, transition.priority);
        hash_byte(hash, transition.hasExitTime ? 1U : 0U);
        hash_float(hash, transition.exitNormalizedTime);
        hash_float(hash, transition.minimumStateTimeSeconds);
        hash_byte(hash, transition.resetTime ? 1U : 0U);
        hash_byte(hash, transition.consumeTriggers ? 1U : 0U);
        hash_float(hash, transition.blendDurationSeconds);
        hash_byte(hash, transition.synchronizeNormalizedTime ? 1U : 0U);
        hash_byte(hash, static_cast<std::uint8_t>(transition.interruptionSource));
        hash_byte(hash, static_cast<std::uint8_t>(transition.blendCurve));
        hash_byte(hash, static_cast<std::uint8_t>(transition.trackPolicy));
        hash_byte(hash, static_cast<std::uint8_t>(transition.eventPolicy));
        hash_byte(hash, static_cast<std::uint8_t>(transition.rootMotionPolicy));
        hash_integer(hash, static_cast<std::uint64_t>(transition.conditions.size()));
        for (const SpriteAnimationCondition& condition : transition.conditions) {
            hash_string(hash, condition.parameter);
            hash_byte(hash, static_cast<std::uint8_t>(condition.operation));
            hash_parameter_value(hash, condition.comparison);
        }
    }
    return hash;
}

bool SpriteAnimationStateMachineAsset::validate(const SpriteAsset& sprite,
                                                 std::string* error) const {
    if (name.empty() || name.size() > 255U)
        return fail(error, "sprite state-machine name is invalid");
    if (states.empty() || states.size() > kMaximumMachineEntries ||
        parameters.size() > kMaximumMachineEntries || transitions.size() > kMaximumMachineEntries)
        return fail(error, "sprite state-machine entry count is out of range");
    if (compatibleSpriteAssetHash != 0U && compatibleSpriteAssetHash != sprite.contentHash)
        return fail(error, "sprite state-machine compatible asset hash does not match");

    std::set<std::string, std::less<>> stateNames;
    for (const SpriteAnimationState& state : states) {
        if (state.name.empty() || state.name.size() > 255U || !stateNames.insert(state.name).second)
            return fail(error, "sprite state-machine state name is invalid or duplicated");
        if (find_sprite_clip(sprite, state.clip) == nullptr)
            return fail(error, "sprite state-machine state references a missing clip");
        if (!std::isfinite(state.playbackSpeed) || state.playbackSpeed < 0.0F ||
            state.playbackSpeed > 1024.0F || !std::isfinite(state.graphPosition.x) ||
            !std::isfinite(state.graphPosition.y) || std::fabs(state.graphPosition.x) > 1000000.0F ||
            std::fabs(state.graphPosition.y) > 1000000.0F)
            return fail(error, "sprite state-machine playback or graph position is invalid");
    }
    if (!stateNames.contains(initialState))
        return fail(error, "sprite state-machine initial state does not exist");

    std::set<std::string, std::less<>> parameterNames;
    for (const SpriteAnimationParameterDefinition& parameter : parameters) {
        if (parameter.name.empty() || parameter.name.size() > 255U ||
            !parameterNames.insert(parameter.name).second ||
            !finite_parameter(parameter.defaultValue))
            return fail(error, "sprite state-machine parameter is invalid or duplicated");
        if (parameter.defaultValue.type == SpriteAnimationParameterType::Trigger &&
            parameter.defaultValue.booleanValue)
            return fail(error, "sprite state-machine triggers must default to reset");
    }

    for (const SpriteAnimationTransition& transition : transitions) {
        if ((transition.fromState != "*" && !stateNames.contains(transition.fromState)) ||
            !stateNames.contains(transition.toState))
            return fail(error, "sprite state-machine transition references a missing state");
        if (!std::isfinite(transition.exitNormalizedTime) ||
            transition.exitNormalizedTime < 0.0F || transition.exitNormalizedTime > 1.0F ||
            !std::isfinite(transition.minimumStateTimeSeconds) ||
            transition.minimumStateTimeSeconds < 0.0F ||
            transition.minimumStateTimeSeconds > 86400.0F ||
            !std::isfinite(transition.blendDurationSeconds) ||
            transition.blendDurationSeconds < 0.0F || transition.blendDurationSeconds > 3600.0F ||
            static_cast<unsigned>(transition.interruptionSource) >
                static_cast<unsigned>(SpriteAnimationInterruptionSource::PreviousThenCurrent) ||
            static_cast<unsigned>(transition.blendCurve) >
                static_cast<unsigned>(SpriteAnimationBlendCurve::EaseOut) ||
            static_cast<unsigned>(transition.trackPolicy) >
                static_cast<unsigned>(SpriteAnimationBlendTrackPolicy::BothWeighted) ||
            static_cast<unsigned>(transition.eventPolicy) >
                static_cast<unsigned>(SpriteAnimationBlendEventPolicy::Both) ||
            static_cast<unsigned>(transition.rootMotionPolicy) >
                static_cast<unsigned>(SpriteAnimationBlendRootMotionPolicy::Weighted) ||
            transition.conditions.size() > 1024U)
            return fail(error, "sprite state-machine transition timing or interruption policy is invalid");
        for (const SpriteAnimationCondition& condition : transition.conditions) {
            const SpriteAnimationParameterDefinition* definition =
                find_parameter_definition(*this, condition.parameter);
            if (definition == nullptr)
                return fail(error, "sprite state-machine condition references a missing parameter");
            if (condition.comparison.type != definition->defaultValue.type ||
                !finite_parameter(condition.comparison) ||
                !operation_valid_for_type(condition.operation, definition->defaultValue.type))
                return fail(error, "sprite state-machine condition is incompatible with its parameter");
        }
    }
    if (contentHash != 0U && contentHash != sprite_animation_state_machine_content_hash(*this))
        return fail(error, "sprite state-machine content hash is stale");
    return true;
}

void SpriteAnimationStateMachineAsset::recompute_hash() noexcept {
    contentHash = sprite_animation_state_machine_content_hash(*this);
}

bool write_dvesprite_machine(const std::filesystem::path& path,
                             const SpriteAnimationStateMachineAsset& asset,
                             std::string* error) {
    std::ostringstream stream;
    stream << std::setprecision(9);
    stream << "DVE_SPRITE_MACHINE 3\n";
    stream << "name " << std::quoted(asset.name) << '\n';
    stream << "sprite_hash " << asset.compatibleSpriteAssetHash << '\n';
    stream << "initial " << std::quoted(asset.initialState) << '\n';
    stream << "parameters " << asset.parameters.size() << '\n';
    for (const SpriteAnimationParameterDefinition& parameter : asset.parameters) {
        const SpriteAnimationParameterValue& value = parameter.defaultValue;
        stream << "parameter " << std::quoted(parameter.name) << ' '
               << static_cast<unsigned>(value.type) << ' ' << (value.booleanValue ? 1U : 0U)
               << ' ' << value.integerValue << ' ' << value.floatValue << '\n';
    }
    stream << "states " << asset.states.size() << '\n';
    for (const SpriteAnimationState& state : asset.states) {
        stream << "state " << std::quoted(state.name) << ' ' << std::quoted(state.clip)
               << ' ' << state.playbackSpeed << ' ' << state.graphPosition.x << ' '
               << state.graphPosition.y << '\n';
    }
    stream << "transitions " << asset.transitions.size() << '\n';
    for (const SpriteAnimationTransition& transition : asset.transitions) {
        stream << "transition " << std::quoted(transition.fromState) << ' '
               << std::quoted(transition.toState) << ' ' << transition.priority << ' '
               << (transition.hasExitTime ? 1U : 0U) << ' ' << transition.exitNormalizedTime
               << ' ' << transition.minimumStateTimeSeconds << ' '
               << (transition.resetTime ? 1U : 0U) << ' '
               << (transition.consumeTriggers ? 1U : 0U) << ' '
               << transition.blendDurationSeconds << ' '
               << (transition.synchronizeNormalizedTime ? 1U : 0U) << ' '
               << static_cast<unsigned>(transition.interruptionSource) << ' '
               << static_cast<unsigned>(transition.blendCurve) << ' '
               << static_cast<unsigned>(transition.trackPolicy) << ' '
               << static_cast<unsigned>(transition.eventPolicy) << ' '
               << static_cast<unsigned>(transition.rootMotionPolicy) << ' '
               << transition.conditions.size() << '\n';
        for (const SpriteAnimationCondition& condition : transition.conditions) {
            const SpriteAnimationParameterValue& value = condition.comparison;
            stream << "condition " << std::quoted(condition.parameter) << ' '
                   << static_cast<unsigned>(condition.operation) << ' '
                   << static_cast<unsigned>(value.type) << ' '
                   << (value.booleanValue ? 1U : 0U) << ' ' << value.integerValue << ' '
                   << value.floatValue << '\n';
        }
    }
    const std::uint64_t hash = sprite_animation_state_machine_content_hash(asset);
    stream << "hash " << hash << '\n';
    return write_atomic(path, stream.str(), error);
}

SpriteAnimationMachineReadResult read_dvesprite_machine(const std::filesystem::path& path,
                                                         std::uint64_t maximumBytes) {
    SpriteAnimationMachineReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) {
        result.error = ec ? "could not stat sprite state-machine asset"
                          : "sprite state-machine asset exceeds byte limit";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "could not open sprite state-machine asset";
        return result;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) {
        result.error = "could not read complete sprite state-machine asset";
        return result;
    }
    std::istringstream stream(bytes);
    std::string token;
    unsigned version{};
    if (!(stream >> token >> version) || token != "DVE_SPRITE_MACHINE" ||
        (version != 1U && version != 2U && version != 3U)) {
        result.error = "unsupported sprite state-machine header";
        return result;
    }
    if (!(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name)) ||
        !(stream >> token) || token != "sprite_hash" ||
        !(stream >> result.asset.compatibleSpriteAssetHash) ||
        !(stream >> token) || token != "initial" ||
        !(stream >> std::quoted(result.asset.initialState))) {
        result.error = "invalid sprite state-machine metadata";
        return result;
    }
    std::size_t count{};
    if (!(stream >> token >> count) || token != "parameters" || count > kMaximumMachineEntries) {
        result.error = "invalid sprite state-machine parameter count";
        return result;
    }
    result.asset.parameters.resize(count);
    for (SpriteAnimationParameterDefinition& parameter : result.asset.parameters) {
        unsigned type{}, booleanValue{};
        if (!(stream >> token) || token != "parameter" ||
            !(stream >> std::quoted(parameter.name) >> type >> booleanValue >>
              parameter.defaultValue.integerValue >> parameter.defaultValue.floatValue) ||
            type > static_cast<unsigned>(SpriteAnimationParameterType::Trigger) ||
            booleanValue > 1U) {
            result.error = "invalid sprite state-machine parameter";
            return result;
        }
        parameter.defaultValue.type = static_cast<SpriteAnimationParameterType>(type);
        parameter.defaultValue.booleanValue = booleanValue != 0U;
    }
    if (!(stream >> token >> count) || token != "states" || count == 0U ||
        count > kMaximumMachineEntries) {
        result.error = "invalid sprite state-machine state count";
        return result;
    }
    result.asset.states.resize(count);
    for (SpriteAnimationState& state : result.asset.states) {
        if (!(stream >> token) || token != "state" ||
            !(stream >> std::quoted(state.name) >> std::quoted(state.clip) >> state.playbackSpeed)) {
            result.error = "invalid sprite state-machine state";
            return result;
        }
        if (version >= 2U && !(stream >> state.graphPosition.x >> state.graphPosition.y)) {
            result.error = "invalid sprite state-machine graph position";
            return result;
        }
    }
    if (!(stream >> token >> count) || token != "transitions" || count > kMaximumMachineEntries) {
        result.error = "invalid sprite state-machine transition count";
        return result;
    }
    result.asset.transitions.resize(count);
    for (SpriteAnimationTransition& transition : result.asset.transitions) {
        unsigned hasExit{}, reset{}, consume{}, synchronized{}, interruption{};
        unsigned blendCurve{}, trackPolicy{}, eventPolicy{}, rootMotionPolicy{};
        std::size_t conditions{};
        if (!(stream >> token) || token != "transition" ||
            !(stream >> std::quoted(transition.fromState) >> std::quoted(transition.toState) >>
              transition.priority >> hasExit >> transition.exitNormalizedTime >>
              transition.minimumStateTimeSeconds >> reset >> consume) ||
            hasExit > 1U || reset > 1U || consume > 1U) {
            result.error = "invalid sprite state-machine transition";
            return result;
        }
        if (version >= 2U) {
            if (!(stream >> transition.blendDurationSeconds >> synchronized >> interruption) ||
                synchronized > 1U || interruption > static_cast<unsigned>(
                    SpriteAnimationInterruptionSource::PreviousThenCurrent)) {
                result.error = "invalid sprite state-machine transition blend policy";
                return result;
            }
            transition.synchronizeNormalizedTime = synchronized != 0U;
            transition.interruptionSource =
                static_cast<SpriteAnimationInterruptionSource>(interruption);
            if (version >= 3U) {
                if (!(stream >> blendCurve >> trackPolicy >> eventPolicy >> rootMotionPolicy >> conditions) ||
                    blendCurve > static_cast<unsigned>(SpriteAnimationBlendCurve::EaseOut) ||
                    trackPolicy > static_cast<unsigned>(SpriteAnimationBlendTrackPolicy::BothWeighted) ||
                    eventPolicy > static_cast<unsigned>(SpriteAnimationBlendEventPolicy::Both) ||
                    rootMotionPolicy > static_cast<unsigned>(SpriteAnimationBlendRootMotionPolicy::Weighted) ||
                    conditions > 1024U) {
                    result.error = "invalid sprite state-machine gameplay blend policy";
                    return result;
                }
                transition.blendCurve = static_cast<SpriteAnimationBlendCurve>(blendCurve);
                transition.trackPolicy = static_cast<SpriteAnimationBlendTrackPolicy>(trackPolicy);
                transition.eventPolicy = static_cast<SpriteAnimationBlendEventPolicy>(eventPolicy);
                transition.rootMotionPolicy =
                    static_cast<SpriteAnimationBlendRootMotionPolicy>(rootMotionPolicy);
            } else if (!(stream >> conditions) || conditions > 1024U) {
                result.error = "invalid sprite state-machine transition condition count";
                return result;
            }
        } else if (!(stream >> conditions) || conditions > 1024U) {
            result.error = "invalid sprite state-machine transition condition count";
            return result;
        }
        transition.hasExitTime = hasExit != 0U;
        transition.resetTime = reset != 0U;
        transition.consumeTriggers = consume != 0U;
        transition.conditions.resize(conditions);
        for (SpriteAnimationCondition& condition : transition.conditions) {
            unsigned operation{}, type{}, booleanValue{};
            if (!(stream >> token) || token != "condition" ||
                !(stream >> std::quoted(condition.parameter) >> operation >> type >> booleanValue >>
                  condition.comparison.integerValue >> condition.comparison.floatValue) ||
                operation > static_cast<unsigned>(SpriteAnimationCompareOp::NotTriggered) ||
                type > static_cast<unsigned>(SpriteAnimationParameterType::Trigger) ||
                booleanValue > 1U) {
                result.error = "invalid sprite state-machine condition";
                return result;
            }
            condition.operation = static_cast<SpriteAnimationCompareOp>(operation);
            condition.comparison.type = static_cast<SpriteAnimationParameterType>(type);
            condition.comparison.booleanValue = booleanValue != 0U;
        }
    }
    std::uint64_t storedHash{};
    if (!(stream >> token >> storedHash) || token != "hash") {
        result.error = "missing sprite state-machine content hash";
        return result;
    }
    stream >> std::ws;
    if (!stream.eof()) result.error = "trailing sprite state-machine data";
    const std::uint64_t expected = version == 1U ? legacy_machine_hash_v1(result.asset)
        : (version == 2U ? legacy_machine_hash_v2(result.asset)
                         : sprite_animation_state_machine_content_hash(result.asset));
    if (result.error.empty() && storedHash != expected)
        result.error = "sprite state-machine content hash mismatch";
    if (result.error.empty()) result.asset.recompute_hash();
    return result;
}

bool SpriteAnimationStateRuntime::register_machine(
    SpriteAnimationMachineId id, SpriteAssetId spriteAsset,
    SpriteAnimationStateMachineAsset machineValue, std::string* error) {
    if (id == kInvalidSpriteAnimationMachineId || machines_.contains(id))
        return fail(error, "sprite animation machine ID is invalid or already registered");
    const SpriteAsset* sprite = sprites_->asset(spriteAsset);
    if (sprite == nullptr) return fail(error, "sprite animation machine asset is not registered");
    if (!machineValue.validate(*sprite, error)) return false;
    machineValue.contentHash = sprite_animation_state_machine_content_hash(machineValue);
    machines_.emplace(id, RegisteredMachine{spriteAsset, std::move(machineValue)});
    return true;
}

bool SpriteAnimationStateRuntime::unregister_machine(SpriteAnimationMachineId id) noexcept {
    if (std::any_of(controllers_.begin(), controllers_.end(), [id](const auto& item) {
            return item.second.machine == id;
        })) return false;
    return machines_.erase(id) != 0U;
}

const SpriteAnimationStateMachineAsset* SpriteAnimationStateRuntime::machine(
    SpriteAnimationMachineId id) const noexcept {
    const auto found = machines_.find(id);
    return found == machines_.end() ? nullptr : &found->second.asset;
}

bool SpriteAnimationStateRuntime::bind(SpriteOwnerId owner, SpriteAnimationMachineId machineId,
                                       std::string* error) {
    if (owner == kInvalidSpriteOwnerId || controllers_.contains(owner) || !sprites_->contains(owner))
        return fail(error, "sprite animation owner is invalid, unbound, or already controlled");
    const auto registered = machines_.find(machineId);
    if (registered == machines_.end()) return fail(error, "sprite animation machine is not registered");
    const SpriteInstanceDesc* desc = sprites_->instance_desc(owner);
    if (desc == nullptr || desc->asset != registered->second.spriteAsset)
        return fail(error, "sprite animation machine is incompatible with the owner asset");
    Controller controller;
    controller.machine = machineId;
    controller.state = registered->second.asset.initialState;
    for (const SpriteAnimationParameterDefinition& parameter : registered->second.asset.parameters)
        controller.parameters.emplace(parameter.name, parameter.defaultValue);
    if (!apply_state(owner, controller, controller.state, true, error)) return false;
    controllers_.emplace(owner, std::move(controller));
    return true;
}

bool SpriteAnimationStateRuntime::unbind(SpriteOwnerId owner) noexcept {
    return controllers_.erase(owner) != 0U;
}

bool SpriteAnimationStateRuntime::contains(SpriteOwnerId owner) const noexcept {
    return controllers_.contains(owner);
}

std::string_view SpriteAnimationStateRuntime::current_state(SpriteOwnerId owner) const noexcept {
    const auto found = controllers_.find(owner);
    return found == controllers_.end() ? std::string_view{} : std::string_view{found->second.state};
}

float SpriteAnimationStateRuntime::state_elapsed_seconds(SpriteOwnerId owner) const noexcept {
    const auto found = controllers_.find(owner);
    return found == controllers_.end() ? 0.0F : found->second.stateElapsedSeconds;
}

bool SpriteAnimationStateRuntime::is_blending(SpriteOwnerId owner) const noexcept {
    const auto found = controllers_.find(owner);
    return found != controllers_.end() && found->second.blend.active;
}

std::optional<SpriteAnimationBlendSample> SpriteAnimationStateRuntime::blend_sample(
    SpriteOwnerId owner) const noexcept {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return std::nullopt;
    const Controller& controller = found->second;
    SpriteAnimationBlendSample result;
    result.owner = owner;
    result.active = controller.blend.active;
    result.sourceState = controller.blend.sourceState;
    result.destinationState = controller.state;
    result.destination = sprites_->sample(owner);
    result.destinationWeight = controller.blend.destination_weight();
    result.sourceWeight = 1.0F - result.destinationWeight;
    if (controller.blend.active) {
        const RegisteredMachine& registered = machines_.at(controller.machine);
        const SpriteAnimationState* source = find_state(registered.asset, controller.blend.sourceState);
        const SpriteAsset* sprite = sprites_->asset(registered.spriteAsset);
        if (source != nullptr && sprite != nullptr)
            result.source = sample_sprite_clip(*sprite, source->clip, controller.blend.sourceTimeSeconds);
    }
    return result;
}

std::optional<SpriteAnimationBlendGameplaySample>
SpriteAnimationStateRuntime::blend_gameplay_sample(SpriteOwnerId owner) const noexcept {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return std::nullopt;
    const Controller& controller = found->second;
    SpriteAnimationBlendGameplaySample result;
    result.owner = owner;
    result.active = controller.blend.active;
    result.destinationWeight = controller.blend.destination_weight();
    result.sourceWeight = 1.0F - result.destinationWeight;

    const std::optional<SpriteTrackSample> destination = sprites_->sample_tracks(owner);
    std::optional<SpriteTrackSample> source;
    if (controller.blend.active) {
        const RegisteredMachine& registered = machines_.at(controller.machine);
        const SpriteAnimationState* sourceState =
            find_state(registered.asset, controller.blend.sourceState);
        if (sourceState != nullptr) {
            source = sprites_->sample_tracks(
                owner, sourceState->clip, controller.blend.sourceTimeSeconds);
        }
    }

    const auto append_sample = [&](const SpriteTrackSample& sample, float weight, bool fromSource) {
        for (const SpriteCombatVolumeSample& value : sample.combatVolumes)
            result.combatVolumes.push_back({value, weight, fromSource});
        for (const SpriteSocketSample& value : sample.sockets)
            result.sockets.push_back({value, weight, fromSource});
        for (const SpritePropertySample& value : sample.properties)
            result.properties.push_back({value, weight, fromSource});
    };

    if (!controller.blend.active || !source) {
        if (destination) append_sample(*destination, 1.0F, false);
    } else {
        switch (controller.blend.trackPolicy) {
        case SpriteAnimationBlendTrackPolicy::DestinationOnly:
            if (destination) append_sample(*destination, 1.0F, false);
            break;
        case SpriteAnimationBlendTrackPolicy::SourceOnly:
            append_sample(*source, 1.0F, true);
            break;
        case SpriteAnimationBlendTrackPolicy::HighestWeight:
            if (result.destinationWeight >= result.sourceWeight) {
                if (destination) append_sample(*destination, 1.0F, false);
            } else {
                append_sample(*source, 1.0F, true);
            }
            break;
        case SpriteAnimationBlendTrackPolicy::BothWeighted:
            append_sample(*source, result.sourceWeight, true);
            if (destination) append_sample(*destination, result.destinationWeight, false);
            break;
        }
    }

    const auto root_from_track = [](const SpriteTrackSample& sample) {
        SpriteRootMotionDelta delta;
        delta.deltaPixels = sample.rootMotionDeltaPixels;
        delta.rotationDegrees = sample.rootMotionRotationDegrees;
        return delta;
    };
    if (!controller.blend.active || !source) {
        if (destination) result.rootMotion = root_from_track(*destination);
    } else {
        const SpriteRootMotionDelta sourceRoot = root_from_track(*source);
        const SpriteRootMotionDelta destinationRoot = destination
            ? root_from_track(*destination) : SpriteRootMotionDelta{};
        switch (controller.blend.rootMotionPolicy) {
        case SpriteAnimationBlendRootMotionPolicy::DestinationOnly:
            result.rootMotion = destinationRoot;
            break;
        case SpriteAnimationBlendRootMotionPolicy::SourceOnly:
            result.rootMotion = sourceRoot;
            break;
        case SpriteAnimationBlendRootMotionPolicy::Weighted:
            result.rootMotion.deltaPixels = {
                sourceRoot.deltaPixels.x * result.sourceWeight +
                    destinationRoot.deltaPixels.x * result.destinationWeight,
                sourceRoot.deltaPixels.y * result.sourceWeight +
                    destinationRoot.deltaPixels.y * result.destinationWeight};
            result.rootMotion.rotationDegrees =
                sourceRoot.rotationDegrees * result.sourceWeight +
                destinationRoot.rotationDegrees * result.destinationWeight;
            break;
        }
    }
    return result;
}

SpriteRenderList SpriteAnimationStateRuntime::build_render_list(Float3 cameraOrigin) const {
    std::vector<SpriteBlendRenderOverride> overrides;
    overrides.reserve(controllers_.size());
    for (const auto& [owner, controller] : controllers_) {
        if (!controller.blend.active) continue;
        const std::optional<SpriteAnimationBlendSample> sample = blend_sample(owner);
        if (!sample) continue;
        overrides.push_back({owner, sample->source, sample->destination,
                             sample->sourceWeight, sample->destinationWeight});
    }
    return sprites_->build_render_list(cameraOrigin, overrides);
}

namespace {
float sprite_clip_duration(const SpriteAsset& asset, std::string_view clipName) noexcept {
    const SpriteClip* clip = find_sprite_clip(asset, clipName);
    if (clip == nullptr) return 0.0F;
    float duration = 0.0F;
    for (SpriteFrameIndex index : clip->frames) {
        if (index >= asset.frames.size()) return 0.0F;
        duration += asset.frames[index].durationSeconds;
    }
    return duration;
}

int interruption_rank(const SpriteAnimationBlendSnapshot& blend,
                      const SpriteAnimationTransition& transition) noexcept {
    if (!blend.active) return transition.fromState == "*" ? 1 : 0;
    if (blend.interruptionSource == SpriteAnimationInterruptionSource::NoInterruption) return -1;
    if (transition.fromState == "*") return 2;
    const bool current = transition.fromState == blend.destinationState;
    const bool previous = transition.fromState == blend.sourceState;
    switch (blend.interruptionSource) {
    case SpriteAnimationInterruptionSource::NoInterruption: return -1;
    case SpriteAnimationInterruptionSource::CurrentState: return current ? 0 : -1;
    case SpriteAnimationInterruptionSource::PreviousState: return previous ? 0 : -1;
    case SpriteAnimationInterruptionSource::CurrentThenPrevious:
        return current ? 0 : (previous ? 1 : -1);
    case SpriteAnimationInterruptionSource::PreviousThenCurrent:
        return previous ? 0 : (current ? 1 : -1);
    }
    return -1;
}

bool set_controller_parameter(std::map<std::string, SpriteAnimationParameterValue, std::less<>>& values,
                              std::string_view name, SpriteAnimationParameterType type,
                              SpriteAnimationParameterValue value, std::string* error) {
    const auto found = values.find(name);
    if (found == values.end()) return fail(error, "sprite animation parameter does not exist");
    if (found->second.type != type) return fail(error, "sprite animation parameter type mismatch");
    found->second = value;
    return true;
}
} // namespace

bool SpriteAnimationStateRuntime::set_boolean(SpriteOwnerId owner, std::string_view name, bool value,
                                              std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    SpriteAnimationParameterValue parameterValue;
    parameterValue.type = SpriteAnimationParameterType::Boolean;
    parameterValue.booleanValue = value;
    return set_controller_parameter(found->second.parameters, name,
                                    SpriteAnimationParameterType::Boolean, parameterValue, error);
}

bool SpriteAnimationStateRuntime::set_integer(SpriteOwnerId owner, std::string_view name,
                                              std::int64_t value, std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    SpriteAnimationParameterValue parameterValue;
    parameterValue.type = SpriteAnimationParameterType::Integer;
    parameterValue.integerValue = value;
    return set_controller_parameter(found->second.parameters, name,
                                    SpriteAnimationParameterType::Integer, parameterValue, error);
}

bool SpriteAnimationStateRuntime::set_float(SpriteOwnerId owner, std::string_view name, float value,
                                            std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    if (!std::isfinite(value)) return fail(error, "sprite animation float parameter is not finite");
    SpriteAnimationParameterValue parameterValue;
    parameterValue.type = SpriteAnimationParameterType::Float;
    parameterValue.floatValue = value;
    return set_controller_parameter(found->second.parameters, name,
                                    SpriteAnimationParameterType::Float, parameterValue, error);
}

bool SpriteAnimationStateRuntime::fire_trigger(SpriteOwnerId owner, std::string_view name,
                                               std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    SpriteAnimationParameterValue parameterValue;
    parameterValue.type = SpriteAnimationParameterType::Trigger;
    parameterValue.booleanValue = true;
    return set_controller_parameter(found->second.parameters, name,
                                    SpriteAnimationParameterType::Trigger, parameterValue, error);
}

bool SpriteAnimationStateRuntime::reset_trigger(SpriteOwnerId owner, std::string_view name,
                                                std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    SpriteAnimationParameterValue parameterValue;
    parameterValue.type = SpriteAnimationParameterType::Trigger;
    return set_controller_parameter(found->second.parameters, name,
                                    SpriteAnimationParameterType::Trigger, parameterValue, error);
}

std::optional<SpriteAnimationParameterValue> SpriteAnimationStateRuntime::parameter(
    SpriteOwnerId owner, std::string_view name) const noexcept {
    const auto controller = controllers_.find(owner);
    if (controller == controllers_.end()) return std::nullopt;
    const auto found = controller->second.parameters.find(name);
    return found == controller->second.parameters.end()
        ? std::nullopt : std::optional<SpriteAnimationParameterValue>{found->second};
}

bool SpriteAnimationStateRuntime::apply_state(SpriteOwnerId owner, Controller& controller,
                                              std::string_view stateName, bool restart,
                                              std::string* error) {
    const RegisteredMachine& registered = machines_.at(controller.machine);
    const SpriteAnimationState* state = find_state(registered.asset, stateName);
    if (state == nullptr) return fail(error, "sprite animation state does not exist");
    if (!sprites_->set_clip(owner, state->clip, restart, error) ||
        !sprites_->set_playback_speed(owner, state->playbackSpeed) ||
        !sprites_->set_playing(owner, true))
        return fail(error, "sprite animation state could not update the sprite runtime");
    controller.state = state->name;
    controller.stateElapsedSeconds = 0.0F;
    return true;
}

bool SpriteAnimationStateRuntime::force_state(SpriteOwnerId owner, std::string_view state,
                                              bool restart, std::string* error) {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return fail(error, "sprite animation owner is not controlled");
    if (!apply_state(owner, found->second, state, restart, error)) return false;
    found->second.blend = {};
    return true;
}

std::optional<SpriteAnimationControllerSnapshot> SpriteAnimationStateRuntime::capture_snapshot(
    SpriteOwnerId owner) const {
    const auto found = controllers_.find(owner);
    if (found == controllers_.end()) return std::nullopt;
    SpriteAnimationControllerSnapshot result;
    result.owner = owner;
    result.machine = found->second.machine;
    result.state = found->second.state;
    result.stateElapsedSeconds = found->second.stateElapsedSeconds;
    result.spriteTimeSeconds = sprites_->time_seconds(owner);
    result.transitionSerial = found->second.transitionSerial;
    result.blend = found->second.blend;
    result.parameters = found->second.parameters;
    return result;
}

bool SpriteAnimationStateRuntime::restore_snapshot(
    const SpriteAnimationControllerSnapshot& snapshot, std::string* error) {
    const auto found = controllers_.find(snapshot.owner);
    if (found == controllers_.end() || found->second.machine != snapshot.machine)
        return fail(error, "sprite animation snapshot owner or machine does not match");
    if (!std::isfinite(snapshot.stateElapsedSeconds) || snapshot.stateElapsedSeconds < 0.0F ||
        !std::isfinite(snapshot.spriteTimeSeconds) || snapshot.spriteTimeSeconds < 0.0F ||
        !std::isfinite(snapshot.blend.sourceTimeSeconds) || snapshot.blend.sourceTimeSeconds < 0.0F ||
        !std::isfinite(snapshot.blend.elapsedSeconds) || snapshot.blend.elapsedSeconds < 0.0F ||
        !std::isfinite(snapshot.blend.durationSeconds) || snapshot.blend.durationSeconds < 0.0F)
        return fail(error, "sprite animation snapshot timing is invalid");
    const RegisteredMachine& registered = machines_.at(snapshot.machine);
    if (find_state(registered.asset, snapshot.state) == nullptr)
        return fail(error, "sprite animation snapshot state does not exist");
    if (snapshot.blend.active &&
        (find_state(registered.asset, snapshot.blend.sourceState) == nullptr ||
         snapshot.blend.destinationState != snapshot.state ||
         snapshot.blend.durationSeconds <= 0.0F ||
         snapshot.blend.elapsedSeconds >= snapshot.blend.durationSeconds ||
         static_cast<unsigned>(snapshot.blend.interruptionSource) >
             static_cast<unsigned>(SpriteAnimationInterruptionSource::PreviousThenCurrent) ||
         static_cast<unsigned>(snapshot.blend.curve) >
             static_cast<unsigned>(SpriteAnimationBlendCurve::EaseOut) ||
         static_cast<unsigned>(snapshot.blend.trackPolicy) >
             static_cast<unsigned>(SpriteAnimationBlendTrackPolicy::BothWeighted) ||
         static_cast<unsigned>(snapshot.blend.eventPolicy) >
             static_cast<unsigned>(SpriteAnimationBlendEventPolicy::Both) ||
         static_cast<unsigned>(snapshot.blend.rootMotionPolicy) >
             static_cast<unsigned>(SpriteAnimationBlendRootMotionPolicy::Weighted)))
        return fail(error, "sprite animation snapshot blend state is invalid");
    if (snapshot.parameters.size() != registered.asset.parameters.size())
        return fail(error, "sprite animation snapshot parameter set is incomplete");
    for (const SpriteAnimationParameterDefinition& definition : registered.asset.parameters) {
        const auto parameterValue = snapshot.parameters.find(definition.name);
        if (parameterValue == snapshot.parameters.end() ||
            parameterValue->second.type != definition.defaultValue.type ||
            !finite_parameter(parameterValue->second))
            return fail(error, "sprite animation snapshot parameter is invalid");
    }
    if (!apply_state(snapshot.owner, found->second, snapshot.state, true, error) ||
        !sprites_->seek(snapshot.owner, snapshot.spriteTimeSeconds))
        return fail(error, "sprite animation snapshot could not restore sprite time");
    found->second.stateElapsedSeconds = snapshot.stateElapsedSeconds;
    found->second.transitionSerial = snapshot.transitionSerial;
    found->second.blend = snapshot.blend;
    found->second.parameters = snapshot.parameters;
    return true;
}

bool SpriteAnimationStateRuntime::transition_conditions_met(
    const SpriteAnimationTransition& transition, const Controller& controller) const noexcept {
    for (const SpriteAnimationCondition& condition : transition.conditions) {
        const auto found = controller.parameters.find(condition.parameter);
        if (found == controller.parameters.end()) return false;
        const SpriteAnimationParameterValue& value = found->second;
        bool met = false;
        switch (condition.operation) {
        case SpriteAnimationCompareOp::IsTrue: met = value.booleanValue; break;
        case SpriteAnimationCompareOp::IsFalse: met = !value.booleanValue; break;
        case SpriteAnimationCompareOp::Equal:
            if (value.type == SpriteAnimationParameterType::Boolean)
                met = value.booleanValue == condition.comparison.booleanValue;
            else if (value.type == SpriteAnimationParameterType::Integer)
                met = value.integerValue == condition.comparison.integerValue;
            else
                met = value.floatValue == condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::NotEqual:
            if (value.type == SpriteAnimationParameterType::Boolean)
                met = value.booleanValue != condition.comparison.booleanValue;
            else if (value.type == SpriteAnimationParameterType::Integer)
                met = value.integerValue != condition.comparison.integerValue;
            else
                met = value.floatValue != condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::Greater:
            met = value.type == SpriteAnimationParameterType::Integer
                ? value.integerValue > condition.comparison.integerValue
                : value.floatValue > condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::GreaterEqual:
            met = value.type == SpriteAnimationParameterType::Integer
                ? value.integerValue >= condition.comparison.integerValue
                : value.floatValue >= condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::Less:
            met = value.type == SpriteAnimationParameterType::Integer
                ? value.integerValue < condition.comparison.integerValue
                : value.floatValue < condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::LessEqual:
            met = value.type == SpriteAnimationParameterType::Integer
                ? value.integerValue <= condition.comparison.integerValue
                : value.floatValue <= condition.comparison.floatValue;
            break;
        case SpriteAnimationCompareOp::Triggered: met = value.booleanValue; break;
        case SpriteAnimationCompareOp::NotTriggered: met = !value.booleanValue; break;
        }
        if (!met) return false;
    }
    return true;
}

bool SpriteAnimationStateRuntime::evaluate_transition(SpriteOwnerId owner, Controller& controller,
                                                      bool exitTimePass) {
    const RegisteredMachine& registered = machines_.at(controller.machine);
    const SpriteAnimationStateMachineAsset& machineValue = registered.asset;
    const std::optional<SpriteSample> sample = sprites_->sample(owner);
    if (!sample) return false;
    const SpriteAnimationTransition* best = nullptr;
    std::size_t bestIndex{};
    int bestRank = std::numeric_limits<int>::max();
    for (std::size_t index = 0U; index < machineValue.transitions.size(); ++index) {
        const SpriteAnimationTransition& transition = machineValue.transitions[index];
        const int rank = interruption_rank(controller.blend, transition);
        if (rank < 0 || transition.hasExitTime != exitTimePass ||
            (!controller.blend.active && transition.fromState != "*" &&
             transition.fromState != controller.state) ||
            transition.toState == controller.state ||
            controller.stateElapsedSeconds < transition.minimumStateTimeSeconds ||
            (transition.hasExitTime && sample->normalizedTime < transition.exitNormalizedTime) ||
            !transition_conditions_met(transition, controller)) continue;
        if (best == nullptr || transition.priority > best->priority ||
            (transition.priority == best->priority && rank < bestRank)) {
            best = &transition;
            bestIndex = index;
            bestRank = rank;
        }
    }
    if (best == nullptr) return false;
    const std::string previousControllerState = controller.state;
    std::string transitionSourceState = previousControllerState;
    float transitionSourceTime = sprites_->time_seconds(owner);
    float transitionSourceNormalized = sample->normalizedTime;
    const SpriteAsset* sprite = sprites_->asset(registered.spriteAsset);
    if (controller.blend.active && best->fromState == controller.blend.sourceState) {
        transitionSourceState = controller.blend.sourceState;
        transitionSourceTime = controller.blend.sourceTimeSeconds;
        const SpriteAnimationState* source = find_state(machineValue, transitionSourceState);
        if (source != nullptr && sprite != nullptr) {
            const std::optional<SpriteSample> sourceSample =
                sample_sprite_clip(*sprite, source->clip, transitionSourceTime);
            if (sourceSample) transitionSourceNormalized = sourceSample->normalizedTime;
        }
    }
    if (!apply_state(owner, controller, best->toState, best->resetTime, nullptr)) return false;
    if (best->synchronizeNormalizedTime) {
        const SpriteAnimationState* destination = find_state(machineValue, controller.state);
        if (destination != nullptr && sprite != nullptr) {
            const float duration = sprite_clip_duration(*sprite, destination->clip);
            if (duration > 0.0F) (void)sprites_->seek(owner, transitionSourceNormalized * duration);
        }
    }
    if (best->blendDurationSeconds > 0.0F) {
        controller.blend.active = true;
        controller.blend.sourceState = transitionSourceState;
        controller.blend.destinationState = controller.state;
        controller.blend.sourceTimeSeconds = transitionSourceTime;
        controller.blend.elapsedSeconds = 0.0F;
        controller.blend.durationSeconds = best->blendDurationSeconds;
        controller.blend.interruptionSource = best->interruptionSource;
        controller.blend.curve = best->blendCurve;
        controller.blend.trackPolicy = best->trackPolicy;
        controller.blend.eventPolicy = best->eventPolicy;
        controller.blend.rootMotionPolicy = best->rootMotionPolicy;
    } else {
        controller.blend = {};
    }
    if (best->consumeTriggers) {
        for (const SpriteAnimationCondition& condition : best->conditions) {
            auto parameterValue = controller.parameters.find(condition.parameter);
            if (parameterValue != controller.parameters.end() &&
                parameterValue->second.type == SpriteAnimationParameterType::Trigger)
                parameterValue->second.booleanValue = false;
        }
    }
    ++controller.transitionSerial;
    transitionEvents_.push_back({owner, controller.machine, transitionSourceState, controller.state,
                                 bestIndex, sprites_->time_seconds(owner),
                                 controller.transitionSerial});
    return true;
}

void SpriteAnimationStateRuntime::tick(float deltaSeconds) {
    transitionEvents_.clear();
    gameplayEvents_.clear();
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) return;
    for (auto& [owner, controller] : controllers_) {
        if (sprites_->contains(owner)) (void)evaluate_transition(owner, controller, false);
    }

    struct BlendAdvance {
        SpriteOwnerId owner{kInvalidSpriteOwnerId};
        SpriteAnimationMachineId machine{kInvalidSpriteAnimationMachineId};
        std::string sourceState;
        float sourceFrom{};
        float sourceTo{};
        SpriteAnimationBlendEventPolicy eventPolicy{
            SpriteAnimationBlendEventPolicy::DestinationOnly};
        SpriteAnimationBlendRootMotionPolicy rootMotionPolicy{
            SpriteAnimationBlendRootMotionPolicy::Weighted};
        float sourceWeight{};
        float destinationWeight{1.0F};
        SpriteRootMotionDelta pendingBefore;
    };
    std::vector<BlendAdvance> advances;
    advances.reserve(controllers_.size());
    for (const auto& [owner, controller] : controllers_) {
        if (!controller.blend.active || !sprites_->contains(owner)) continue;
        const RegisteredMachine& registered = machines_.at(controller.machine);
        const SpriteAnimationState* source = find_state(registered.asset, controller.blend.sourceState);
        const float speed = source == nullptr ? 1.0F : source->playbackSpeed;
        SpriteAnimationBlendSnapshot midpoint = controller.blend;
        midpoint.elapsedSeconds += deltaSeconds * 0.5F;
        const float destinationWeight = midpoint.destination_weight();
        BlendAdvance advance;
        advance.owner = owner;
        advance.machine = controller.machine;
        advance.sourceState = controller.blend.sourceState;
        advance.sourceFrom = controller.blend.sourceTimeSeconds;
        advance.sourceTo = controller.blend.sourceTimeSeconds + deltaSeconds * speed;
        advance.eventPolicy = controller.blend.eventPolicy;
        advance.rootMotionPolicy = controller.blend.rootMotionPolicy;
        advance.destinationWeight = destinationWeight;
        advance.sourceWeight = 1.0F - destinationWeight;
        if (const auto pending = sprites_->pending_root_motion(owner))
            advance.pendingBefore = *pending;
        advances.push_back(std::move(advance));
    }

    sprites_->tick(deltaSeconds);

    const auto advance_for = [&advances](SpriteOwnerId owner) -> const BlendAdvance* {
        const auto found = std::find_if(advances.begin(), advances.end(),
            [owner](const BlendAdvance& value) { return value.owner == owner; });
        return found == advances.end() ? nullptr : &*found;
    };
    for (const SpriteIntervalEvent& event : sprites_->interval_events()) {
        const BlendAdvance* advance = advance_for(event.owner);
        if (advance == nullptr ||
            advance->eventPolicy != SpriteAnimationBlendEventPolicy::SourceOnly) {
            gameplayEvents_.push_back(event);
        }
    }

    for (const BlendAdvance& advance : advances) {
        const RegisteredMachine& registered = machines_.at(advance.machine);
        const SpriteAnimationState* sourceState =
            find_state(registered.asset, advance.sourceState);
        const SpriteAsset* sprite = sprites_->asset(registered.spriteAsset);
        const SpriteInstanceDesc* desc = sprites_->instance_desc(advance.owner);
        if (sourceState == nullptr || sprite == nullptr || desc == nullptr) continue;

        if (advance.eventPolicy != SpriteAnimationBlendEventPolicy::DestinationOnly) {
            SpriteIntervalQueryResult sourceEvents;
            SpriteIntervalQueryOptions options;
            options.maximumEvents = 16384U;
            std::string ignored;
            if (query_sprite_clip_interval_events(
                    *sprite, sourceState->clip, advance.sourceFrom, advance.sourceTo,
                    options, sourceEvents, &ignored)) {
                for (SpriteIntervalEvent& event : sourceEvents.events) {
                    event.owner = advance.owner;
                    gameplayEvents_.push_back(std::move(event));
                }
            }
        }

        if (advance.rootMotionPolicy != SpriteAnimationBlendRootMotionPolicy::DestinationOnly) {
            const auto sourceDelta = accumulate_sprite_root_motion(
                *sprite, sourceState->clip, advance.sourceFrom, advance.sourceTo,
                desc->plane, desc->flipX, desc->flipY);
            const auto pendingAfter = sprites_->pending_root_motion(advance.owner);
            if (sourceDelta && pendingAfter) {
                SpriteRootMotionDelta destinationDelta;
                destinationDelta.deltaPixels = {
                    pendingAfter->deltaPixels.x - advance.pendingBefore.deltaPixels.x,
                    pendingAfter->deltaPixels.y - advance.pendingBefore.deltaPixels.y};
                destinationDelta.rotationDegrees =
                    pendingAfter->rotationDegrees - advance.pendingBefore.rotationDegrees;
                destinationDelta.worldTranslation = {
                    pendingAfter->worldTranslation.x - advance.pendingBefore.worldTranslation.x,
                    pendingAfter->worldTranslation.y - advance.pendingBefore.worldTranslation.y,
                    pendingAfter->worldTranslation.z - advance.pendingBefore.worldTranslation.z};
                destinationDelta.crossedLoopBoundary = pendingAfter->crossedLoopBoundary;
                destinationDelta.enteredFrames = pendingAfter->enteredFrames >=
                    advance.pendingBefore.enteredFrames
                    ? pendingAfter->enteredFrames - advance.pendingBefore.enteredFrames : 0U;

                SpriteRootMotionDelta selected;
                if (advance.rootMotionPolicy == SpriteAnimationBlendRootMotionPolicy::SourceOnly) {
                    selected = *sourceDelta;
                } else {
                    selected.deltaPixels = {
                        sourceDelta->deltaPixels.x * advance.sourceWeight +
                            destinationDelta.deltaPixels.x * advance.destinationWeight,
                        sourceDelta->deltaPixels.y * advance.sourceWeight +
                            destinationDelta.deltaPixels.y * advance.destinationWeight};
                    selected.rotationDegrees =
                        sourceDelta->rotationDegrees * advance.sourceWeight +
                        destinationDelta.rotationDegrees * advance.destinationWeight;
                    selected.worldTranslation = {
                        sourceDelta->worldTranslation.x * advance.sourceWeight +
                            destinationDelta.worldTranslation.x * advance.destinationWeight,
                        sourceDelta->worldTranslation.y * advance.sourceWeight +
                            destinationDelta.worldTranslation.y * advance.destinationWeight,
                        sourceDelta->worldTranslation.z * advance.sourceWeight +
                            destinationDelta.worldTranslation.z * advance.destinationWeight};
                    selected.crossedLoopBoundary = sourceDelta->crossedLoopBoundary ||
                        destinationDelta.crossedLoopBoundary;
                    selected.enteredFrames = std::max(
                        sourceDelta->enteredFrames, destinationDelta.enteredFrames);
                }
                SpriteRootMotionDelta combined = advance.pendingBefore;
                combined.deltaPixels.x += selected.deltaPixels.x;
                combined.deltaPixels.y += selected.deltaPixels.y;
                combined.rotationDegrees += selected.rotationDegrees;
                combined.worldTranslation.x += selected.worldTranslation.x;
                combined.worldTranslation.y += selected.worldTranslation.y;
                combined.worldTranslation.z += selected.worldTranslation.z;
                combined.crossedLoopBoundary = combined.crossedLoopBoundary ||
                    selected.crossedLoopBoundary;
                const std::uint64_t entered = static_cast<std::uint64_t>(combined.enteredFrames) +
                    selected.enteredFrames;
                combined.enteredFrames = entered > std::numeric_limits<std::uint32_t>::max()
                    ? std::numeric_limits<std::uint32_t>::max()
                    : static_cast<std::uint32_t>(entered);
                (void)sprites_->set_pending_root_motion(advance.owner, combined);
            }
        }
    }

    std::stable_sort(gameplayEvents_.begin(), gameplayEvents_.end(),
        [](const SpriteIntervalEvent& left, const SpriteIntervalEvent& right) {
            if (left.owner != right.owner) return left.owner < right.owner;
            if (left.reverse != right.reverse) return !left.reverse;
            if (left.timeSeconds != right.timeSeconds)
                return left.reverse ? left.timeSeconds > right.timeSeconds
                                    : left.timeSeconds < right.timeSeconds;
            if (left.kind != right.kind)
                return static_cast<unsigned>(left.kind) < static_cast<unsigned>(right.kind);
            if (left.trackId != right.trackId) return left.trackId < right.trackId;
            if (left.frame != right.frame) return left.frame < right.frame;
            return left.name < right.name;
        });

    for (auto& [owner, controller] : controllers_) {
        if (!sprites_->contains(owner)) continue;
        controller.stateElapsedSeconds += deltaSeconds;
        if (controller.blend.active) {
            const RegisteredMachine& registered = machines_.at(controller.machine);
            const SpriteAnimationState* source = find_state(registered.asset, controller.blend.sourceState);
            const float speed = source == nullptr ? 1.0F : source->playbackSpeed;
            controller.blend.sourceTimeSeconds += deltaSeconds * speed;
            controller.blend.elapsedSeconds += deltaSeconds;
            if (controller.blend.elapsedSeconds >= controller.blend.durationSeconds)
                controller.blend = {};
        }
        (void)evaluate_transition(owner, controller, true);
    }
}

std::optional<SpriteSideEffectCommand> SpriteSideEffectLedger::issue(
    std::uint64_t simulationTick, const SpriteIntervalEvent& event) {
    if (event.reverse) return std::nullopt;
    std::uint64_t hash = kFnvOffset;
    hash_byte(hash, static_cast<std::uint8_t>(SpriteSideEffectSource::IntervalEvent));
    hash_integer(hash, event.owner);
    hash_byte(hash, static_cast<std::uint8_t>(event.kind));
    hash_integer(hash, event.cycle);
    hash_integer(hash, static_cast<std::uint64_t>(event.playbackSequenceIndex));
    hash_integer(hash, static_cast<std::uint64_t>(event.authoredSequenceIndex));
    hash_integer(hash, event.frame);
    hash_integer(hash, event.trackId);
    hash_string(hash, event.name);
    const SpriteSideEffectCommandId id = nonzero_hash(hash);
    if (committed_.contains(id)) return std::nullopt;
    committed_.emplace(id, simulationTick);
    SpriteSideEffectCommand command;
    command.id = id;
    command.simulationTick = simulationTick;
    command.source = SpriteSideEffectSource::IntervalEvent;
    command.intervalEvent = event;
    return command;
}

std::optional<SpriteSideEffectCommand> SpriteSideEffectLedger::issue(
    std::uint64_t simulationTick, const SpriteAnimationTransitionEvent& event) {
    std::uint64_t hash = kFnvOffset;
    hash_byte(hash, static_cast<std::uint8_t>(SpriteSideEffectSource::StateTransition));
    hash_integer(hash, event.owner);
    hash_integer(hash, event.machine);
    hash_integer(hash, event.transitionSerial);
    hash_integer(hash, static_cast<std::uint64_t>(event.transitionIndex));
    hash_string(hash, event.fromState);
    hash_string(hash, event.toState);
    const SpriteSideEffectCommandId id = nonzero_hash(hash);
    if (committed_.contains(id)) return std::nullopt;
    committed_.emplace(id, simulationTick);
    SpriteSideEffectCommand command;
    command.id = id;
    command.simulationTick = simulationTick;
    command.source = SpriteSideEffectSource::StateTransition;
    command.transitionEvent = event;
    return command;
}

bool SpriteSideEffectLedger::contains(SpriteSideEffectCommandId id) const noexcept {
    return id != kInvalidSpriteSideEffectCommandId && committed_.contains(id);
}

void SpriteSideEffectLedger::rollback_after(std::uint64_t simulationTick) noexcept {
    for (auto it = committed_.begin(); it != committed_.end();) {
        if (it->second > simulationTick) it = committed_.erase(it);
        else ++it;
    }
}


bool apply_sprite_root_motion_to_physics2d(
    const SpriteRootMotionDelta& delta, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionApplyOptions& options, std::string* error) {
    if (!body) return fail(error, "sprite root-motion 2D body handle is invalid");
    if (!validate_root_options(options, error)) return false;
    Physics2DBodyState state;
    if (!world.body_state(body, state))
        return fail(error, "sprite root-motion 2D body does not exist");
    const TileVec2 translation = world_translation_to_pixels(delta, options);
    const float rotation = options.applyRotation
        ? delta.rotationDegrees * kDegreesToRadians : 0.0F;
    if (options.mode == SpriteRootMotionApplyMode::Transform) {
        if (!world.set_body_transform(body,
                {state.positionPixels.x + translation.x,
                 state.positionPixels.y + translation.y},
                state.angleRadians + rotation))
            return fail(error, "sprite root-motion could not update the 2D body transform");
        return true;
    }
    TileVec2 velocity{translation.x / options.deltaSeconds,
                      translation.y / options.deltaSeconds};
    if (options.additiveVelocity) {
        velocity.x += state.linearVelocityPixelsPerSecond.x;
        velocity.y += state.linearVelocityPixelsPerSecond.y;
    }
    if (!world.set_body_linear_velocity(body, velocity))
        return fail(error, "sprite root-motion could not update the 2D body velocity");
    if (options.applyRotation && rotation != 0.0F &&
        !world.set_body_transform(body, state.positionPixels, state.angleRadians + rotation))
        return fail(error, "sprite root-motion could not update the 2D body rotation");
    return true;
}

bool apply_pending_sprite_root_motion_to_physics2d(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world,
    Physics2DBodyHandle body, const SpriteRootMotionApplyOptions& options,
    std::string* error) {
    const std::optional<SpriteRootMotionDelta> pending = sprites.pending_root_motion(owner);
    if (!pending) return fail(error, "sprite root-motion owner is not bound");
    if (!apply_sprite_root_motion_to_physics2d(*pending, world, body, options, error)) return false;
    (void)sprites.consume_root_motion(owner);
    return true;
}

bool apply_sprite_root_motion_to_game_object(
    const SpriteRootMotionDelta& delta, GameWorld& world, GameObjectId object,
    const SpriteRootMotionApplyOptions& options, std::string* error) {
    if (object == kInvalidGameObjectId || !world.has_object(object))
        return fail(error, "sprite root-motion game object is invalid");
    if (!validate_root_options(options, error)) return false;
    const Float3 translation = multiply(delta.worldTranslation, options.translationScale);
    if (options.mode == SpriteRootMotionApplyMode::Transform) {
        const std::optional<RigidTransform> transform = world.transform(object);
        if (!transform || !world.set_position(object, add(transform->position, translation)))
            return fail(error, "sprite root-motion could not update the game-object position");
        if (options.applyRotation) {
            const Quaternion rotation = quaternion_from_axis_angle(
                root_axis(options.plane), delta.rotationDegrees * kDegreesToRadians);
            if (!world.set_rotation(object, multiply(transform->rotation, rotation)))
                return fail(error, "sprite root-motion could not update the game-object rotation");
        }
        return true;
    }
    Float3 velocity = multiply(translation, 1.0F / options.deltaSeconds);
    if (options.additiveVelocity) {
        const std::optional<Float3> current = world.linear_velocity(object);
        if (current) velocity = add(*current, velocity);
    }
    if (!world.set_linear_velocity(object, velocity))
        return fail(error, "sprite root-motion could not update the game-object velocity");
    if (options.applyRotation) {
        const std::optional<RigidTransform> transform = world.transform(object);
        const Quaternion rotation = quaternion_from_axis_angle(
            root_axis(options.plane), delta.rotationDegrees * kDegreesToRadians);
        if (!transform || !world.set_rotation(object, multiply(transform->rotation, rotation)))
            return fail(error, "sprite root-motion could not update the game-object rotation");
    }
    return true;
}

bool apply_pending_sprite_root_motion_to_game_object(
    SpriteRuntime& sprites, SpriteOwnerId owner, GameWorld& world, GameObjectId object,
    const SpriteRootMotionApplyOptions& options, std::string* error) {
    const std::optional<SpriteRootMotionDelta> pending = sprites.pending_root_motion(owner);
    if (!pending) return fail(error, "sprite root-motion owner is not bound");
    if (!apply_sprite_root_motion_to_game_object(*pending, world, object, options, error)) return false;
    (void)sprites.consume_root_motion(owner);
    return true;
}

bool apply_sprite_root_motion_collision_aware(
    const SpriteRootMotionDelta& delta, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionCollisionOptions& options, SpriteRootMotionCollisionResult& result,
    std::string* error) {
    result = {};
    if (!body) return fail(error, "collision-aware sprite root-motion body handle is invalid");
    if (!validate_root_options(options.application, error)) return false;
    if (options.application.mode != SpriteRootMotionApplyMode::Transform)
        return fail(error, "collision-aware sprite root motion requires transform application mode");
    if (!std::isfinite(options.skinPixels) || options.skinPixels < 0.0F ||
        options.skinPixels > 1024.0F || options.maximumSlideIterations > 16U ||
        !std::isfinite(options.maximumStepHeightPixels) ||
        options.maximumStepHeightPixels < 0.0F || options.maximumStepHeightPixels > 1024.0F ||
        !std::isfinite(options.groundSnapDistancePixels) ||
        options.groundSnapDistancePixels < 0.0F || options.groundSnapDistancePixels > 1024.0F ||
        !std::isfinite(options.maximumGroundSlopeDegrees) ||
        options.maximumGroundSlopeDegrees < 0.0F || options.maximumGroundSlopeDegrees > 89.9F ||
        !std::isfinite(options.movingPlatformDeltaPixels.x) ||
        !std::isfinite(options.movingPlatformDeltaPixels.y))
        return fail(error, "collision-aware sprite root-motion options are invalid");
    if (!world.capabilities().shapeCasts)
        return fail(error, "selected 2D physics backend does not support shape casts");
    Physics2DBodyState state;
    if (!world.body_state(body, state))
        return fail(error, "collision-aware sprite root-motion body does not exist");

    result.requestedPixels = add_tile(
        world_translation_to_pixels(delta, options.application), options.movingPlatformDeltaPixels);
    result.movingPlatformApplied = length(options.movingPlatformDeltaPixels) > 0.00001F;
    TileVec2 authoredApplied{};
    Physics2DQueryShape shape = options.localShape;
    translate_query_shape(shape, state.positionPixels);
    Physics2DQueryFilter filter = options.filter;
    if (!filter.ignoredBody) filter.ignoredBody = body;
    TileVec2 remaining = result.requestedPixels;
    const std::uint32_t maximumIterations = options.maximumSlideIterations + 1U;
    for (std::uint32_t iteration = 0U; iteration < maximumIterations; ++iteration) {
        const float remainingLength = length(remaining);
        if (remainingLength <= 0.00001F) {
            remaining = {};
            break;
        }
        const Physics2DShapeCastHit hit = world.cast_shape(shape, remaining, filter);
        if (hit.startedOverlapping) {
            result.startedOverlapping = true;
            result.blocked = true;
            break;
        }
        if (!hit.hit) {
            result.appliedPixels = add_tile(result.appliedPixels, remaining);
            authoredApplied = add_tile(authoredApplied, remaining);
            translate_query_shape(shape, remaining);
            remaining = {};
            break;
        }
        result.supportNormal = hit.normal;
        ++result.collisionCount;
        if (options.allowStepUp && !result.steppedUp &&
            options.maximumStepHeightPixels > 0.0F && std::fabs(hit.normal.x) > 0.5F) {
            const TileVec2 upward{0.0F, options.maximumStepHeightPixels};
            const Physics2DShapeCastHit ceiling = world.cast_shape(shape, upward, filter);
            if (!ceiling.hit && !ceiling.startedOverlapping) {
                Physics2DQueryShape raised = shape;
                translate_query_shape(raised, upward);
                const Physics2DShapeCastHit forward = world.cast_shape(raised, remaining, filter);
                if (!forward.hit && !forward.startedOverlapping) {
                    result.appliedPixels = add_tile(result.appliedPixels, upward);
                    result.appliedPixels = add_tile(result.appliedPixels, remaining);
                    authoredApplied = add_tile(authoredApplied, remaining);
                    shape = std::move(raised);
                    translate_query_shape(shape, remaining);
                    remaining = {};
                    result.steppedUp = true;
                    break;
                }
            }
        }
        const float skinFraction = options.skinPixels / remainingLength;
        const float allowedFraction = std::clamp(hit.fraction - skinFraction, 0.0F, 1.0F);
        const TileVec2 step = multiply_tile(remaining, allowedFraction);
        result.appliedPixels = add_tile(result.appliedPixels, step);
        authoredApplied = add_tile(authoredApplied, step);
        translate_query_shape(shape, step);
        TileVec2 leftover = multiply_tile(remaining, 1.0F - allowedFraction);
        if (!options.slideAlongSurfaces || iteration + 1U >= maximumIterations) {
            remaining = leftover;
            result.blocked = length(remaining) > 0.00001F;
            break;
        }
        const float intoSurface = dot_tile(leftover, hit.normal);
        if (intoSurface < 0.0F)
            leftover = add_tile(leftover, multiply_tile(hit.normal, -intoSurface));
        if (length(leftover) >= length(remaining) - 0.00001F) {
            remaining = leftover;
            result.blocked = true;
            break;
        }
        remaining = leftover;
    }
    if (options.snapToGround && options.groundSnapDistancePixels > 0.0F) {
        const TileVec2 downward{0.0F, -options.groundSnapDistancePixels};
        const Physics2DShapeCastHit ground = world.cast_shape(shape, downward, filter);
        const float minimumGroundNormal = std::cos(
            options.maximumGroundSlopeDegrees * kDegreesToRadians);
        if (ground.hit && !ground.startedOverlapping && ground.normal.y >= minimumGroundNormal) {
            const float snapFraction = std::clamp(
                ground.fraction - options.skinPixels / options.groundSnapDistancePixels,
                0.0F, 1.0F);
            const TileVec2 snap = multiply_tile(downward, snapFraction);
            result.appliedPixels = add_tile(result.appliedPixels, snap);
            translate_query_shape(shape, snap);
            result.snappedToGround = true;
            result.supportNormal = ground.normal;
        }
    }
    result.residualPixels = {result.requestedPixels.x - authoredApplied.x,
                             result.requestedPixels.y - authoredApplied.y};
    result.blocked = result.blocked || result.startedOverlapping ||
        length(result.residualPixels) > 0.00001F;
    const float rotation = options.application.applyRotation
        ? delta.rotationDegrees * kDegreesToRadians : 0.0F;
    if (!world.set_body_transform(body, add_tile(state.positionPixels, result.appliedPixels),
                                  state.angleRadians + rotation))
        return fail(error, "collision-aware sprite root motion could not update the body transform");
    return true;
}

bool apply_pending_sprite_root_motion_collision_aware(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world, Physics2DBodyHandle body,
    const SpriteRootMotionCollisionOptions& options, SpriteRootMotionCollisionResult& result,
    std::string* error) {
    const std::optional<SpriteRootMotionDelta> pending = sprites.pending_root_motion(owner);
    if (!pending) return fail(error, "sprite root-motion owner is not bound");
    if (!apply_sprite_root_motion_collision_aware(*pending, world, body, options, result, error))
        return false;
    (void)sprites.consume_root_motion(owner);
    if (options.requeueResidual && length(result.residualPixels) > 0.00001F) {
        const float scale = options.application.pixelsPerWorldUnit *
            options.application.translationScale;
        if (std::fabs(scale) <= std::numeric_limits<float>::epsilon())
            return fail(error, "sprite root-motion residual cannot be requeued with zero scale");
        SpriteRootMotionDelta residual;
        residual.deltaPixels = {result.residualPixels.x, result.residualPixels.y};
        const float inverseScale = 1.0F / scale;
        if (options.application.plane == GameplayPlane2D::XY)
            residual.worldTranslation = {result.residualPixels.x * inverseScale,
                                         result.residualPixels.y * inverseScale, 0.0F};
        else
            residual.worldTranslation = {result.residualPixels.x * inverseScale, 0.0F,
                                         result.residualPixels.y * inverseScale};
        (void)sprites.set_pending_root_motion(owner, residual);
    }
    return true;
}

bool apply_pending_sprite_root_motion_to_character_controller(
    SpriteRuntime& sprites, SpriteOwnerId owner, Physics2DWorld& world, Physics2DBodyHandle body,
    CharacterController2D& controller, const SpriteRootMotionCollisionOptions& options,
    SpriteRootMotionCollisionResult& result, std::string* error) {
    if (!apply_pending_sprite_root_motion_collision_aware(
            sprites, owner, world, body, options, result, error)) return false;
    if (!controller.post_step(world, body, options.application.deltaSeconds))
        return fail(error, "character controller could not reconcile collision-aware root motion");
    return true;
}


bool SpriteGameObjectAttachmentBridge::bind(SpriteGameObjectAttachmentBinding binding,
                                            std::string* error) {
    if (binding.attachment == kInvalidSpriteAttachmentId ||
        binding.object == kInvalidGameObjectId || bindings_.contains(binding.attachment))
        return fail(error, "sprite game-object attachment binding is invalid or duplicated");
    bindings_.emplace(binding.attachment, binding);
    return true;
}

bool SpriteGameObjectAttachmentBridge::unbind(SpriteAttachmentId attachment) noexcept {
    return bindings_.erase(attachment) != 0U;
}

SpriteAttachmentSyncReport SpriteGameObjectAttachmentBridge::sync(
    const SpriteRuntime& sprites, GameWorld& world) const {
    SpriteAttachmentSyncReport report;
    const std::vector<SpriteSocketAttachmentSample> samples = sprites.sample_socket_attachments();
    for (const auto& [attachment, binding] : bindings_) {
        if (!world.has_object(binding.object)) {
            ++report.missing;
            report.diagnostics.push_back("attachment " + std::to_string(attachment) +
                                         " targets a missing game object");
            continue;
        }
        const auto sample = std::find_if(samples.begin(), samples.end(),
            [attachment](const SpriteSocketAttachmentSample& candidate) {
                return candidate.id == attachment;
            });
        if (sample == samples.end()) {
            ++report.missing;
            if (binding.hideWhenSocketMissing && world.set_enabled(binding.object, false))
                ++report.hidden;
            else if (binding.hideWhenSocketMissing)
                ++report.failed;
            continue;
        }
        bool succeeded = world.set_position(binding.object, sample->worldTransform.position) &&
            world.set_rotation(binding.object, sample->worldTransform.rotation);
        if (binding.inheritVisibility)
            succeeded = world.set_enabled(binding.object, sample->visible) && succeeded;
        if (succeeded) ++report.updated;
        else {
            ++report.failed;
            report.diagnostics.push_back("attachment " + std::to_string(attachment) +
                                         " could not update its game object");
        }
    }
    return report;
}

} // namespace dve
