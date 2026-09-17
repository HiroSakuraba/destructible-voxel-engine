#include "dve/fluoddity.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "dve/ai/json.hpp"

namespace dve {
namespace {

constexpr std::array<char, 8> kMagic{'D', 'F', 'L', 'U', 'O', 'D', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kMaximumStringBytes = 1U << 20U;
constexpr std::size_t kMaximumPresetBytes = 8U << 20U;

struct ParameterSpec {
    FluoddityParameter parameter;
    std::string_view displayName;
    std::string_view physicsKey;
    std::string_view sweepKey;
    float defaultValue;
    float defaultMinimum;
    float defaultMaximum;
};

constexpr std::array<ParameterSpec, kFluoddityParameterCount> kParameterSpecs{{
    {FluoddityParameter::SensorGain, "Sensor Gain", "sensor_gain", "SENSOR_GAIN", 0.116F, 0.0F, 5.0F},
    {FluoddityParameter::SensorAngle, "Sensor Angle", "sensor_angle", "SENSOR_ANGLE", 0.45F, -1.0F, 1.0F},
    {FluoddityParameter::SensorDistance, "Sensor Distance", "sensor_distance", "SENSOR_DISTANCE", 1.0F, 0.0F, 4.0F},
    {FluoddityParameter::MutationScale, "Mutation Scale", "mutation_scale", "MUTATION_SCALE", 0.0F, -0.5F, 0.5F},
    {FluoddityParameter::GlobalForceMultiplier, "Global Force Mult", "global_force_mult", "GLOBAL_FORCE_MULT", 1.0F, 0.0F, 2.0F},
    {FluoddityParameter::Drag, "Drag", "drag", "DRAG", 0.504F, -1.0F, 1.0F},
    {FluoddityParameter::AxialForce, "Axial Force", "axial_force", "AXIAL_FORCE", 0.371F, -1.0F, 1.0F},
    {FluoddityParameter::LateralForce, "Lateral Force", "lateral_force", "LATERAL_FORCE", -0.707F, -1.0F, 1.0F},
    {FluoddityParameter::StrafePower, "Strafe Power", "strafe_power", "STRAFE_POWER", 0.224F, 0.0F, 0.5F},
    {FluoddityParameter::TrailPersistence, "Trail Persistence", "trail_persistence", "TRAIL_PERSISTENCE", 0.938F, 0.0F, 1.0F},
    {FluoddityParameter::TrailDiffusion, "Trail Diffusion", "trail_diffusion", "TRAIL_DIFFUSION", 1.0F, 0.0F, 1.0F},
    {FluoddityParameter::HazardRate, "Hazard Rate", "hazard_rate", "HAZARD_RATE", 0.0F, 0.0F, 0.05F},
}};

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] std::size_t parameter_index(FluoddityParameter parameter) noexcept {
    return static_cast<std::size_t>(parameter);
}

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] std::uint64_t hash_bytes(
    std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

void hash_u8(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash = hash_bytes(hash, &value, sizeof(value));
}

void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 24U),
    };
    hash = hash_bytes(hash, bytes.data(), bytes.size());
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash_u8(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_u32(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value.size()));
    hash = hash_bytes(hash, value.data(), value.size());
}

template <class T, class HashValue>
void hash_optional(std::uint64_t& hash, const std::optional<T>& value, HashValue hashValue) noexcept {
    hash_u8(hash, value.has_value() ? 1U : 0U);
    if (value.has_value()) hashValue(hash, *value);
}

[[nodiscard]] bool write_u8(std::ostream& output, std::uint8_t value) {
    output.put(static_cast<char>(value));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_u32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value), static_cast<char>(value >> 8U),
        static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_u64(std::ostream& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes[shift / 8U] = static_cast<char>(value >> shift);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_i32(std::ostream& output, std::int32_t value) {
    return write_u32(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] bool write_float(std::ostream& output, float value) {
    return write_u32(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] bool write_string(std::ostream& output, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    if (!write_u32(output, static_cast<std::uint32_t>(value.size()))) return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool read_u8(std::istream& input, std::uint8_t& value) {
    const int byte = input.get();
    if (byte == std::char_traits<char>::eof()) return false;
    value = static_cast<std::uint8_t>(byte);
    return true;
}

[[nodiscard]] bool read_u32(std::istream& input, std::uint32_t& value) {
    std::array<std::uint8_t, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return true;
}

[[nodiscard]] bool read_u64(std::istream& input, std::uint64_t& value) {
    std::array<std::uint8_t, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[shift / 8U]) << shift;
    }
    return true;
}

[[nodiscard]] bool read_i32(std::istream& input, std::int32_t& value) {
    std::uint32_t bits{};
    if (!read_u32(input, bits)) return false;
    value = std::bit_cast<std::int32_t>(bits);
    return true;
}

[[nodiscard]] bool read_float(std::istream& input, float& value) {
    std::uint32_t bits{};
    if (!read_u32(input, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

[[nodiscard]] bool read_string(std::istream& input, std::string& value) {
    std::uint32_t size{};
    if (!read_u32(input, size) || size > kMaximumStringBytes) return false;
    value.resize(size);
    input.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(input);
}

template <class T, class WriteValue>
[[nodiscard]] bool write_optional(
    std::ostream& output, const std::optional<T>& value, WriteValue writeValue) {
    if (!write_u8(output, value.has_value() ? 1U : 0U)) return false;
    return !value.has_value() || writeValue(output, *value);
}

template <class T, class ReadValue>
[[nodiscard]] bool read_optional(
    std::istream& input, std::optional<T>& value, ReadValue readValue) {
    std::uint8_t present{};
    if (!read_u8(input, present) || present > 1U) return false;
    if (present == 0U) {
        value.reset();
        return true;
    }
    T decoded{};
    if (!readValue(input, decoded)) return false;
    value = decoded;
    return true;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("Unable to inspect preset file: " + ec.message());
    if (size > kMaximumPresetBytes) throw std::runtime_error("Preset JSON exceeds the 8 MiB import limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open preset JSON");
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input && !text.empty()) throw std::runtime_error("Unable to read preset JSON");
    return text;
}

[[nodiscard]] const ai::JsonValue* object_field(
    const ai::JsonValue* object, std::string_view key) noexcept {
    return object != nullptr && object->is_object() ? object->find(key) : nullptr;
}

[[nodiscard]] float json_float(const ai::JsonValue* value, float fallback) noexcept {
    if (value == nullptr || !value->is_number()) return fallback;
    const double number = value->as_number();
    if (!std::isfinite(number) ||
        number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max())) return fallback;
    return static_cast<float>(number);
}

[[nodiscard]] std::int32_t json_i32(const ai::JsonValue* value, std::int32_t fallback) noexcept {
    if (value == nullptr || !value->is_number()) return fallback;
    const double number = value->as_number();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        number > static_cast<double>(std::numeric_limits<std::int32_t>::max())) return fallback;
    return static_cast<std::int32_t>(number);
}

[[nodiscard]] std::uint32_t json_u32(const ai::JsonValue* value, std::uint32_t fallback) noexcept {
    const std::int32_t integer = json_i32(value, static_cast<std::int32_t>(fallback));
    return integer < 0 ? fallback : static_cast<std::uint32_t>(integer);
}

[[nodiscard]] bool json_bool(const ai::JsonValue* value, bool fallback) noexcept {
    return value != nullptr && value->is_bool() ? value->as_bool() : fallback;
}

[[nodiscard]] std::optional<float> optional_json_float(const ai::JsonValue* value) noexcept {
    if (value == nullptr || !value->is_number()) return std::nullopt;
    const float decoded = json_float(value, std::numeric_limits<float>::quiet_NaN());
    return std::isfinite(decoded) ? std::optional<float>(decoded) : std::nullopt;
}

[[nodiscard]] std::optional<std::int32_t> optional_json_i32(const ai::JsonValue* value) noexcept {
    if (value == nullptr || !value->is_number()) return std::nullopt;
    const std::int32_t sentinel = std::numeric_limits<std::int32_t>::min();
    const std::int32_t decoded = json_i32(value, sentinel);
    return decoded == sentinel ? std::nullopt : std::optional<std::int32_t>(decoded);
}

[[nodiscard]] std::optional<bool> optional_json_bool(const ai::JsonValue* value) noexcept {
    if (value == nullptr || !value->is_bool()) return std::nullopt;
    return value->as_bool();
}

[[nodiscard]] float sweep_value(
    const ai::JsonValue* sweeps,
    std::string_view axis,
    std::string_view parameter) noexcept {
    return json_float(object_field(object_field(sweeps, axis), parameter), 0.0F);
}

[[nodiscard]] std::uint32_t pcg_hash(std::uint32_t seed) noexcept {
    const std::uint32_t state = seed * 747796405U + 2891336453U;
    const std::uint32_t word =
        ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

[[nodiscard]] float source_hash(float x, float y) noexcept {
    const std::uint32_t ux = std::bit_cast<std::uint32_t>(x);
    const std::uint32_t uy = std::bit_cast<std::uint32_t>(y);
    const std::uint32_t result = pcg_hash(ux ^ pcg_hash(uy));
    return static_cast<float>(result) /
           static_cast<float>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] std::array<float, 4> source_hash4(float x, float y) noexcept {
    return {
        source_hash(x, y),
        source_hash(-x + 5.0F, -y + 5.0F),
        source_hash(y - 100.0F, x - 100.0F),
        source_hash(-y + 25.0F, -x + 25.0F),
    };
}

[[nodiscard]] float mix(float a, float b, float amount) noexcept {
    return a + (b - a) * amount;
}

[[nodiscard]] bool write_parameter(
    std::ostream& output, const FluoddityParameterSetting& setting) {
    return write_float(output, setting.value) &&
           write_float(output, setting.minimum) &&
           write_float(output, setting.maximum) &&
           write_float(output, setting.defaultMinimum) &&
           write_float(output, setting.defaultMaximum) &&
           write_float(output, setting.xSweep) &&
           write_float(output, setting.ySweep) &&
           write_float(output, setting.cohortSweep) &&
           write_float(output, setting.jitter);
}

[[nodiscard]] bool read_parameter(
    std::istream& input, FluoddityParameterSetting& setting) {
    return read_float(input, setting.value) &&
           read_float(input, setting.minimum) &&
           read_float(input, setting.maximum) &&
           read_float(input, setting.defaultMinimum) &&
           read_float(input, setting.defaultMaximum) &&
           read_float(input, setting.xSweep) &&
           read_float(input, setting.ySweep) &&
           read_float(input, setting.cohortSweep) &&
           read_float(input, setting.jitter);
}

[[nodiscard]] bool write_center(std::ostream& output, const FluoddityFourierCenter& center) {
    for (float value : center.frequency) if (!write_float(output, value)) return false;
    for (float value : center.amplitude) if (!write_float(output, value)) return false;
    for (float value : center.frequencyExtension) if (!write_float(output, value)) return false;
    for (float value : center.amplitudeExtension) if (!write_float(output, value)) return false;
    return true;
}

[[nodiscard]] bool read_center(std::istream& input, FluoddityFourierCenter& center) {
    for (float& value : center.frequency) if (!read_float(input, value)) return false;
    for (float& value : center.amplitude) if (!read_float(input, value)) return false;
    for (float& value : center.frequencyExtension) if (!read_float(input, value)) return false;
    for (float& value : center.amplitudeExtension) if (!read_float(input, value)) return false;
    return true;
}

} // namespace

bool FluoddityRuleAsset::validate(std::string* error) const {
    if (name.empty() || name.size() > kMaximumStringBytes) {
        set_error(error, "Fluoddity rule name is empty or too large");
        return false;
    }
    if (sourcePreset.size() > kMaximumStringBytes || notes.size() > kMaximumStringBytes) {
        set_error(error, "Fluoddity source metadata exceeds the format limit");
        return false;
    }
    if (cohortCount == 0U || cohortCount > kFluoddityMaximumCohorts) {
        set_error(error, "Fluoddity cohort count must be between 1 and 16384");
        return false;
    }
    if (sourceCanvasResolution == 0U || sourceCanvasResolution > 2048U) {
        set_error(error, "Fluoddity source canvas resolution is out of range");
        return false;
    }
    if (!finite(initialSpacing) || initialSpacing < 0.0F || initialSpacing > 4.0F ||
        !finite(importedRuleSeed) || !finite(gravityForce) || !finite(gravityStrafe) ||
        !finite(hueSensitivity)) {
        set_error(error, "Fluoddity scalar metadata contains a non-finite or invalid value");
        return false;
    }
    if (static_cast<std::uint8_t>(boundaryMode) >
            static_cast<std::uint8_t>(FluoddityBoundaryMode::Wrap) ||
        static_cast<std::uint8_t>(initialCondition) >
            static_cast<std::uint8_t>(FluoddityInitialCondition::Grid3D) ||
        static_cast<std::uint8_t>(seedAlgorithm) >
            static_cast<std::uint8_t>(FluodditySeedAlgorithm::NativeIntegerPcg) ||
        static_cast<std::uint8_t>(trailMode) >
            static_cast<std::uint8_t>(FluoddityTrailMode::VelocityRgbDensityA)) {
        set_error(error, "Fluoddity enum value is invalid");
        return false;
    }
    for (const FluoddityParameterSetting& setting : parameters) {
        if (!finite(setting.value) || !finite(setting.minimum) || !finite(setting.maximum) ||
            !finite(setting.defaultMinimum) || !finite(setting.defaultMaximum) ||
            !finite(setting.xSweep) || !finite(setting.ySweep) ||
            !finite(setting.cohortSweep) || !finite(setting.jitter) ||
            setting.minimum > setting.maximum ||
            setting.defaultMinimum > setting.defaultMaximum) {
            set_error(error, "Fluoddity parameter settings are invalid");
            return false;
        }
    }
    for (const FluoddityFourierCenter& center : rule) {
        for (float value : center.frequency) if (!finite(value)) {
            set_error(error, "Fluoddity rule contains a non-finite frequency"); return false;
        }
        for (float value : center.amplitude) if (!finite(value)) {
            set_error(error, "Fluoddity rule contains a non-finite amplitude"); return false;
        }
        for (float value : center.frequencyExtension) if (!finite(value)) {
            set_error(error, "Fluoddity rule contains a non-finite extension frequency"); return false;
        }
        for (float value : center.amplitudeExtension) if (!finite(value)) {
            set_error(error, "Fluoddity rule contains a non-finite extension amplitude"); return false;
        }
    }
    return true;
}

void FluoddityRuleAsset::recompute_hash() {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, name);
    hash_string(hash, sourcePreset);
    hash_u32(hash, sourceVersion);
    for (const FluoddityParameterSetting& setting : parameters) {
        hash_float(hash, setting.value);
        hash_float(hash, setting.minimum);
        hash_float(hash, setting.maximum);
        hash_float(hash, setting.defaultMinimum);
        hash_float(hash, setting.defaultMaximum);
        hash_float(hash, setting.xSweep);
        hash_float(hash, setting.ySweep);
        hash_float(hash, setting.cohortSweep);
        hash_float(hash, setting.jitter);
    }
    hash_u8(hash, parameterSweepsEnabled ? 1U : 0U);
    hash_u8(hash, disableSymmetry ? 1U : 0U);
    hash_float(hash, gravityForce);
    hash_float(hash, gravityStrafe);
    hash_u8(hash, static_cast<std::uint8_t>(boundaryMode));
    hash_u8(hash, static_cast<std::uint8_t>(initialCondition));
    hash_float(hash, initialSpacing);
    hash_u32(hash, cohortCount);
    hash_float(hash, importedRuleSeed);
    hash_u64(hash, nativeSeed);
    hash_u8(hash, static_cast<std::uint8_t>(seedAlgorithm));
    hash_float(hash, hueSensitivity);
    hash_u8(hash, colorByCohort ? 1U : 0U);
    hash_u32(hash, sourceCanvasResolution);
    hash_u8(hash, static_cast<std::uint8_t>(trailMode));
    for (const FluoddityFourierCenter& center : rule) {
        for (float value : center.frequency) hash_float(hash, value);
        for (float value : center.amplitude) hash_float(hash, value);
        for (float value : center.frequencyExtension) hash_float(hash, value);
        for (float value : center.amplitudeExtension) hash_float(hash, value);
    }
    hash_string(hash, notes);
    hash_u8(hash, compatibility.hadFieldStrengths ? 1U : 0U);
    hash_optional(hash, compatibility.forceFieldStrength,
                  [](std::uint64_t& h, float value) { hash_float(h, value); });
    hash_optional(hash, compatibility.strafeFieldStrength,
                  [](std::uint64_t& h, float value) { hash_float(h, value); });
    hash_optional(hash, compatibility.absoluteOrientation,
                  [](std::uint64_t& h, std::int32_t value) {
                      hash_u32(h, std::bit_cast<std::uint32_t>(value));
                  });
    hash_optional(hash, compatibility.orientationMix,
                  [](std::uint64_t& h, float value) { hash_float(h, value); });
    hash_optional(hash, compatibility.inkWeight,
                  [](std::uint64_t& h, float value) { hash_float(h, value); });
    hash_optional(hash, compatibility.watercolorMode,
                  [](std::uint64_t& h, bool value) { hash_u8(h, value ? 1U : 0U); });
    hash_optional(hash, compatibility.planeSamples,
                  [](std::uint64_t& h, std::int32_t value) {
                      hash_u32(h, std::bit_cast<std::uint32_t>(value));
                  });
    hash_optional(hash, compatibility.testingMode,
                  [](std::uint64_t& h, bool value) { hash_u8(h, value ? 1U : 0U); });
    contentHash = hash;
}

FluoddityImportResult import_fluoddity_preset_json(const std::filesystem::path& path) {
    FluoddityImportResult result;
    try {
        const ai::JsonParseResult parsed = ai::parse_json(read_text_file(path));
        if (!parsed.value.has_value()) {
            result.error = "Invalid Fluoddity preset JSON at byte " +
                           std::to_string(parsed.errorOffset) + ": " + parsed.error;
            return result;
        }
        const ai::JsonValue& root = *parsed.value;
        if (!root.is_object()) {
            result.error = "Fluoddity preset root must be a JSON object";
            return result;
        }

        FluoddityRuleAsset asset;
        asset.name = path.stem().string();
        asset.sourcePreset = path.filename().generic_string();
        asset.sourceVersion = json_u32(root.find("version"), 0U);
        if (asset.sourceVersion != 7U) {
            result.warnings.push_back("Preset version is not the tested Fluoddity version 7");
        }

        const ai::JsonValue* physics = root.find("physics");
        const ai::JsonValue* sliderRanges = root.find("slider_ranges");
        const ai::JsonValue* sweeps = root.find("sweeps");
        const ai::JsonValue* jitters = root.find("jitters");
        for (const ParameterSpec& spec : kParameterSpecs) {
            FluoddityParameterSetting& setting = asset.parameters[parameter_index(spec.parameter)];
            setting.value = json_float(object_field(physics, spec.physicsKey), spec.defaultValue);
            setting.minimum = spec.defaultMinimum;
            setting.maximum = spec.defaultMaximum;
            setting.defaultMinimum = spec.defaultMinimum;
            setting.defaultMaximum = spec.defaultMaximum;
            const ai::JsonValue* range = object_field(sliderRanges, spec.displayName);
            if (range == nullptr && spec.parameter == FluoddityParameter::AxialForce) {
                range = object_field(sliderRanges, "Axial/Sagittal Force");
            }
            if (range != nullptr && range->is_array()) {
                const auto& values = range->as_array();
                if (values.size() >= 2U) {
                    setting.minimum = json_float(&values[0], setting.minimum);
                    setting.maximum = json_float(&values[1], setting.maximum);
                }
                if (values.size() >= 4U) {
                    setting.defaultMinimum = json_float(&values[2], setting.defaultMinimum);
                    setting.defaultMaximum = json_float(&values[3], setting.defaultMaximum);
                }
            }
            setting.xSweep = sweep_value(sweeps, "x", spec.sweepKey);
            setting.ySweep = sweep_value(sweeps, "y", spec.sweepKey);
            setting.cohortSweep = sweep_value(sweeps, "cohort", spec.sweepKey);
            setting.jitter = json_float(object_field(jitters, spec.sweepKey), 0.0F);
        }
        asset.parameterSweepsEnabled = json_bool(root.find("parameter_sweeps_enabled"), false);

        const ai::JsonValue* settings = root.find("settings");
        asset.disableSymmetry = json_bool(object_field(settings, "disable_symmetry"), false);
        asset.gravityForce = json_float(object_field(settings, "gravity_force"), 0.0F);
        asset.gravityStrafe = json_float(object_field(settings, "gravity_strafe"), 0.0F);
        const std::int32_t boundary = json_i32(object_field(settings, "boundary_conditions"), 0);
        const std::int32_t initial = json_i32(object_field(settings, "initial_conditions"), 0);
        if (boundary < 0 || boundary > 2 || initial < 0 || initial > 3) {
            result.error = "Fluoddity preset contains an unsupported boundary or initial-condition mode";
            return result;
        }
        asset.boundaryMode = static_cast<FluoddityBoundaryMode>(boundary);
        asset.initialCondition = static_cast<FluoddityInitialCondition>(initial);
        asset.initialSpacing = json_float(object_field(settings, "init_spacing"), 1.0F);
        asset.cohortCount = json_u32(object_field(settings, "num_cohorts"), 64U);
        asset.importedRuleSeed = json_float(object_field(settings, "rule_seed"), 0.0F);
        asset.seedAlgorithm = FluodditySeedAlgorithm::ImportedFloatPcg;
        asset.nativeSeed = static_cast<std::uint64_t>(
            std::bit_cast<std::uint32_t>(asset.importedRuleSeed));

        const ai::JsonValue* appearance = root.find("appearance");
        asset.hueSensitivity = json_float(object_field(appearance, "hue_sensitivity"), 0.5F);
        asset.colorByCohort = json_bool(object_field(appearance, "color_by_cohort"), true);
        const ai::JsonValue* sim3d = root.find("sim_3d");
        asset.sourceCanvasResolution = json_u32(object_field(sim3d, "canvas_3d_depth"), 256U);
        asset.trailMode = FluoddityTrailMode::VelocityRgb;
        if (const ai::JsonValue* notes = root.find("notes"); notes != nullptr && notes->is_string()) {
            asset.notes = std::string(notes->as_string());
        }

        const ai::JsonValue* rule = root.find("rule");
        if (rule == nullptr || !rule->is_array()) {
            result.error = "Fluoddity preset is missing its rule array";
            return result;
        }
        std::vector<float> flattened;
        flattened.reserve(kFluoddityRuleFloatCount);
        for (const ai::JsonValue& value : rule->as_array()) {
            if (!value.is_number()) {
                result.error = "Fluoddity rule array contains a non-number";
                return result;
            }
            flattened.push_back(json_float(&value, std::numeric_limits<float>::quiet_NaN()));
        }
        if (flattened.size() == 80U) {
            result.warnings.push_back("Imported a legacy 80-float rule and zero-filled dimensions 5-6");
            std::vector<float> expanded;
            expanded.reserve(kFluoddityRuleFloatCount);
            for (std::size_t center = 0U; center < kFluoddityFourierCenterCount; ++center) {
                expanded.insert(expanded.end(), flattened.begin() + static_cast<std::ptrdiff_t>(center * 8U),
                                flattened.begin() + static_cast<std::ptrdiff_t>(center * 8U + 8U));
                expanded.insert(expanded.end(), 4U, 0.0F);
            }
            flattened = std::move(expanded);
        }
        if (flattened.size() != kFluoddityRuleFloatCount) {
            result.error = "Fluoddity rule must contain exactly 120 floats";
            return result;
        }
        std::size_t cursor = 0U;
        for (FluoddityFourierCenter& center : asset.rule) {
            for (float& value : center.frequency) value = flattened[cursor++];
            for (float& value : center.amplitude) value = flattened[cursor++];
            for (float& value : center.frequencyExtension) value = flattened[cursor++];
            for (float& value : center.amplitudeExtension) value = flattened[cursor++];
        }

        asset.compatibility.absoluteOrientation =
            optional_json_i32(object_field(settings, "absolute_orientation"));
        asset.compatibility.orientationMix =
            optional_json_float(object_field(settings, "orientation_mix"));
        asset.compatibility.inkWeight = optional_json_float(object_field(appearance, "ink_weight"));
        asset.compatibility.watercolorMode =
            optional_json_bool(object_field(appearance, "watercolor_mode"));
        asset.compatibility.planeSamples = optional_json_i32(object_field(sim3d, "plane_samples"));
        asset.compatibility.testingMode = optional_json_bool(object_field(sim3d, "testing_mode"));
        if (const ai::JsonValue* fieldStrengths = root.find("field_strengths");
            fieldStrengths != nullptr && fieldStrengths->is_object()) {
            asset.compatibility.hadFieldStrengths = true;
            asset.compatibility.forceFieldStrength =
                optional_json_float(fieldStrengths->find("force"));
            asset.compatibility.strafeFieldStrength =
                optional_json_float(fieldStrengths->find("strafe"));
            const float force = asset.compatibility.forceFieldStrength.value_or(0.0F);
            const float strafe = asset.compatibility.strafeFieldStrength.value_or(0.0F);
            if (force != 0.0F || strafe != 0.0F) {
                result.warnings.push_back(
                    "Legacy field strengths were retained as metadata but are not active runtime forces");
            }
        }
        if (asset.compatibility.absoluteOrientation.value_or(0) != 0 ||
            asset.compatibility.orientationMix.value_or(1.0F) != 1.0F) {
            result.warnings.push_back(
                "Legacy orientation controls were retained as metadata but are not active in the current source simulation");
        }
        if (asset.compatibility.watercolorMode.value_or(false) ||
            asset.compatibility.inkWeight.value_or(1.0F) != 1.0F) {
            result.warnings.push_back(
                "Legacy watercolor/ink appearance fields were retained as metadata only");
        }
        if (asset.compatibility.testingMode.value_or(false) ||
            asset.compatibility.planeSamples.value_or(1) != 1) {
            result.warnings.push_back(
                "Legacy 2D testing fields were retained as metadata; the native module is 3D-only");
        }

        std::string validationError;
        if (!asset.validate(&validationError)) {
            result.error = validationError;
            return result;
        }
        asset.recompute_hash();
        result.asset = std::move(asset);
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

FluoddityPresetBatchResult import_fluoddity_preset_directory(
    const std::filesystem::path& directory) {
    FluoddityPresetBatchResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        result.errors.push_back("Preset path is not a directory: " + directory.generic_string());
        return result;
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator iterator(directory, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (iterator->is_regular_file() && iterator->path().extension() == ".json") {
            files.push_back(iterator->path());
        }
    }
    if (ec) {
        result.errors.push_back("Unable to enumerate preset directory: " + ec.message());
        return result;
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        FluoddityImportResult imported = import_fluoddity_preset_json(file);
        if (!imported) {
            result.errors.push_back(file.generic_string() + ": " + imported.error);
            continue;
        }
        for (std::string& warning : imported.warnings) {
            result.warnings.push_back(file.generic_string() + ": " + warning);
        }
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(file, directory, relativeError);
        imported.asset->sourcePreset =
            relativeError || relative.empty() || relative.generic_string().starts_with("..")
                ? file.filename().generic_string()
                : relative.generic_string();
        imported.asset->recompute_hash();
        result.assets.push_back(std::move(*imported.asset));
    }
    return result;
}

bool write_dfluoddity(
    const std::filesystem::path& path,
    const FluoddityRuleAsset& inputAsset,
    std::string* error) {
    FluoddityRuleAsset asset = inputAsset;
    std::string validationError;
    if (!asset.validate(&validationError)) {
        set_error(error, validationError);
        return false;
    }
    asset.recompute_hash();
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError) {
            set_error(error, "Unable to create .dfluoddity output directory: " +
                                 filesystemError.message());
            return false;
        }
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        set_error(error, "Unable to open temporary .dfluoddity output");
        return false;
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    bool ok = static_cast<bool>(output) && write_u32(output, kFormatVersion) &&
              write_u64(output, asset.contentHash) && write_string(output, asset.name) &&
              write_string(output, asset.sourcePreset) && write_u32(output, asset.sourceVersion);
    for (const FluoddityParameterSetting& setting : asset.parameters) {
        ok = ok && write_parameter(output, setting);
    }
    ok = ok && write_u8(output, asset.parameterSweepsEnabled ? 1U : 0U) &&
         write_u8(output, asset.disableSymmetry ? 1U : 0U) &&
         write_float(output, asset.gravityForce) && write_float(output, asset.gravityStrafe) &&
         write_u8(output, static_cast<std::uint8_t>(asset.boundaryMode)) &&
         write_u8(output, static_cast<std::uint8_t>(asset.initialCondition)) &&
         write_float(output, asset.initialSpacing) && write_u32(output, asset.cohortCount) &&
         write_float(output, asset.importedRuleSeed) && write_u64(output, asset.nativeSeed) &&
         write_u8(output, static_cast<std::uint8_t>(asset.seedAlgorithm)) &&
         write_float(output, asset.hueSensitivity) &&
         write_u8(output, asset.colorByCohort ? 1U : 0U) &&
         write_u32(output, asset.sourceCanvasResolution) &&
         write_u8(output, static_cast<std::uint8_t>(asset.trailMode));
    for (const FluoddityFourierCenter& center : asset.rule) ok = ok && write_center(output, center);
    ok = ok && write_string(output, asset.notes) &&
         write_u8(output, asset.compatibility.hadFieldStrengths ? 1U : 0U) &&
         write_optional(output, asset.compatibility.forceFieldStrength, write_float) &&
         write_optional(output, asset.compatibility.strafeFieldStrength, write_float) &&
         write_optional(output, asset.compatibility.absoluteOrientation, write_i32) &&
         write_optional(output, asset.compatibility.orientationMix, write_float) &&
         write_optional(output, asset.compatibility.inkWeight, write_float) &&
         write_optional(output, asset.compatibility.watercolorMode,
                        [](std::ostream& stream, bool value) {
                            return write_u8(stream, value ? 1U : 0U);
                        }) &&
         write_optional(output, asset.compatibility.planeSamples, write_i32) &&
         write_optional(output, asset.compatibility.testingMode,
                        [](std::ostream& stream, bool value) {
                            return write_u8(stream, value ? 1U : 0U);
                        });
    output.flush();
    ok = ok && static_cast<bool>(output);
    output.close();
    if (!ok) {
        std::filesystem::remove(temporary, filesystemError);
        set_error(error, "Unable to write complete .dfluoddity asset");
        return false;
    }
    std::filesystem::remove(path, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        set_error(error, "Unable to commit .dfluoddity asset");
        return false;
    }
    return true;
}

FluoddityImportResult read_dfluoddity(const std::filesystem::path& path) {
    FluoddityImportResult result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "Unable to open .dfluoddity asset";
        return result;
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t version{};
    std::uint64_t storedHash{};
    if (!input || magic != kMagic || !read_u32(input, version) || version != kFormatVersion ||
        !read_u64(input, storedHash)) {
        result.error = "Unsupported or truncated .dfluoddity header";
        return result;
    }
    FluoddityRuleAsset asset;
    if (!read_string(input, asset.name) || !read_string(input, asset.sourcePreset) ||
        !read_u32(input, asset.sourceVersion)) {
        result.error = "Truncated .dfluoddity metadata";
        return result;
    }
    for (FluoddityParameterSetting& setting : asset.parameters) {
        if (!read_parameter(input, setting)) {
            result.error = "Truncated .dfluoddity parameter block";
            return result;
        }
    }
    std::uint8_t parameterSweeps{}, disableSymmetry{}, boundary{}, initial{}, seedAlgorithm{};
    std::uint8_t colorByCohort{}, trailMode{};
    if (!read_u8(input, parameterSweeps) || parameterSweeps > 1U ||
        !read_u8(input, disableSymmetry) || disableSymmetry > 1U ||
        !read_float(input, asset.gravityForce) || !read_float(input, asset.gravityStrafe) ||
        !read_u8(input, boundary) || !read_u8(input, initial) ||
        !read_float(input, asset.initialSpacing) || !read_u32(input, asset.cohortCount) ||
        !read_float(input, asset.importedRuleSeed) || !read_u64(input, asset.nativeSeed) ||
        !read_u8(input, seedAlgorithm) || !read_float(input, asset.hueSensitivity) ||
        !read_u8(input, colorByCohort) || colorByCohort > 1U ||
        !read_u32(input, asset.sourceCanvasResolution) || !read_u8(input, trailMode)) {
        result.error = "Truncated .dfluoddity simulation metadata";
        return result;
    }
    asset.parameterSweepsEnabled = parameterSweeps != 0U;
    asset.disableSymmetry = disableSymmetry != 0U;
    asset.boundaryMode = static_cast<FluoddityBoundaryMode>(boundary);
    asset.initialCondition = static_cast<FluoddityInitialCondition>(initial);
    asset.seedAlgorithm = static_cast<FluodditySeedAlgorithm>(seedAlgorithm);
    asset.colorByCohort = colorByCohort != 0U;
    asset.trailMode = static_cast<FluoddityTrailMode>(trailMode);
    for (FluoddityFourierCenter& center : asset.rule) {
        if (!read_center(input, center)) {
            result.error = "Truncated .dfluoddity Fourier rule";
            return result;
        }
    }
    std::uint8_t hadFieldStrengths{};
    if (!read_string(input, asset.notes) || !read_u8(input, hadFieldStrengths) ||
        hadFieldStrengths > 1U ||
        !read_optional(input, asset.compatibility.forceFieldStrength, read_float) ||
        !read_optional(input, asset.compatibility.strafeFieldStrength, read_float) ||
        !read_optional(input, asset.compatibility.absoluteOrientation, read_i32) ||
        !read_optional(input, asset.compatibility.orientationMix, read_float) ||
        !read_optional(input, asset.compatibility.inkWeight, read_float) ||
        !read_optional(input, asset.compatibility.watercolorMode,
                       [](std::istream& stream, bool& value) {
                           std::uint8_t decoded{};
                           if (!read_u8(stream, decoded) || decoded > 1U) return false;
                           value = decoded != 0U;
                           return true;
                       }) ||
        !read_optional(input, asset.compatibility.planeSamples, read_i32) ||
        !read_optional(input, asset.compatibility.testingMode,
                       [](std::istream& stream, bool& value) {
                           std::uint8_t decoded{};
                           if (!read_u8(stream, decoded) || decoded > 1U) return false;
                           value = decoded != 0U;
                           return true;
                       })) {
        result.error = "Truncated .dfluoddity compatibility metadata";
        return result;
    }
    asset.compatibility.hadFieldStrengths = hadFieldStrengths != 0U;
    if (input.peek() != std::char_traits<char>::eof()) {
        result.error = "Trailing bytes found after .dfluoddity payload";
        return result;
    }
    std::string validationError;
    if (!asset.validate(&validationError)) {
        result.error = validationError;
        return result;
    }
    asset.recompute_hash();
    if (asset.contentHash != storedHash) {
        result.error = ".dfluoddity content hash mismatch";
        return result;
    }
    result.asset = std::move(asset);
    return result;
}

std::array<float, 6> evaluate_fluoddity_rule(
    const FluoddityRule& rule,
    const std::array<float, 6>& input) noexcept {
    std::array<float, 6> result{};
    for (std::size_t index = 0U; index < rule.size(); ++index) {
        const FluoddityFourierCenter& center = rule[index];
        float phase{};
        for (std::size_t dimension = 0U; dimension < 4U; ++dimension) {
            phase += input[dimension] * center.frequency[dimension];
        }
        phase += input[4] * center.frequencyExtension[0] +
                 input[5] * center.frequencyExtension[1];
        const float phaseOffset = 2.0F * static_cast<float>(index) * 0.6283F +
                                  center.amplitude[3] * 3.14159F;
        result[0] += center.amplitude[0] * std::sin(phase + phaseOffset);
        result[1] += center.amplitude[1] * std::cos(phase + phaseOffset * 0.7F);
        result[2] += center.amplitude[2] * std::sin(phase * 2.0F + phaseOffset * 1.3F);
        result[3] += center.amplitude[3] * std::cos(phase * 2.0F + phaseOffset * 0.5F);
        result[4] += center.amplitudeExtension[0] *
                     std::sin(phase * 3.0F + phaseOffset * 1.7F);
        result[5] += center.amplitudeExtension[1] *
                     std::cos(phase * 3.0F + phaseOffset * 0.3F);
    }
    return result;
}

FluoddityRule generate_fluoddity_rule_from_imported_seed(float seed) noexcept {
    FluoddityRule rule{};
    for (std::size_t index = 0U; index < rule.size(); ++index) {
        FluoddityFourierCenter& center = rule[index];
        const float base = static_cast<float>(index * 8U);
        const float seedFrequency = source_hash(seed, base);
        const float frequencyScale = 1.0F + 2.0F * seedFrequency * seedFrequency;
        for (std::size_t component = 0U; component < 4U; ++component) {
            center.frequency[component] =
                (source_hash(seed, base + static_cast<float>(component)) * 2.0F - 1.0F) *
                frequencyScale;
            center.amplitude[component] =
                source_hash(seed, base + static_cast<float>(component + 4U)) * 2.0F - 1.0F;
        }
        const float extensionBase = 100.0F + static_cast<float>(index * 4U);
        center.frequencyExtension[0] =
            (source_hash(seed, extensionBase) * 2.0F - 1.0F) * frequencyScale;
        center.frequencyExtension[1] =
            (source_hash(seed, extensionBase + 1.0F) * 2.0F - 1.0F) * frequencyScale;
        center.amplitudeExtension[0] = source_hash(seed, extensionBase + 2.0F) * 2.0F - 1.0F;
        center.amplitudeExtension[1] = source_hash(seed, extensionBase + 3.0F) * 2.0F - 1.0F;
    }
    return rule;
}

FluoddityRule mutate_fluoddity_rule(
    const FluoddityRule& inputRule,
    float amount,
    float cohort) noexcept {
    FluoddityRule rule = inputRule;
    const float seed = source_hash(
                           inputRule[4].frequency[0] + inputRule[7].amplitude[1] +
                               inputRule[1].frequency[2],
                           inputRule[4].frequency[1] + inputRule[7].amplitude[0] +
                               inputRule[1].frequency[3]) +
                       cohort;
    for (std::size_t index = 0U; index < rule.size(); ++index) {
        FluoddityFourierCenter& center = rule[index];
        const std::array<float, 4> amplitudeHash = source_hash4(
            -0.5F - static_cast<float>(index) + seed,
            -0.5F + static_cast<float>(index));
        for (std::size_t component = 0U; component < 4U; ++component) {
            center.amplitude[component] += amount * (-1.0F + 2.0F * amplitudeHash[component]);
        }
        const float frequencyScale = 1.0F + amount * 0.5F *
            (source_hash(seed, static_cast<float>(index)) - 0.5F);
        for (float& frequency : center.frequency) frequency *= frequencyScale;
        center.amplitudeExtension[0] += amount * (-1.0F + 2.0F *
            source_hash(seed + 100.0F, static_cast<float>(index)));
        center.amplitudeExtension[1] += amount * (-1.0F + 2.0F *
            source_hash(seed + 200.0F, static_cast<float>(index)));
        const float extensionScale = 1.0F + amount * 0.5F *
            (source_hash(seed + 300.0F, static_cast<float>(index)) - 0.5F);
        for (float& frequency : center.frequencyExtension) frequency *= extensionScale;
    }
    return rule;
}

float evaluate_fluoddity_parameter(
    const FluoddityParameterSetting& setting,
    float positionX,
    float positionY,
    float cohort,
    std::uint32_t cohortCount,
    std::uint32_t frameNumber) noexcept {
    if (setting.xSweep == 0.0F && setting.ySweep == 0.0F &&
        setting.cohortSweep == 0.0F && setting.jitter == 0.0F) {
        return setting.value;
    }
    const float x = (positionX + 1.0F) * 0.5F;
    const float y = (positionY + 1.0F) * 0.5F;
    const float cohortNormalized = cohortCount == 0U ? 0.0F :
        std::floor(cohort) / static_cast<float>(cohortCount);
    float result{};
    std::uint32_t activeSweeps{};
    const auto accumulate = [&](float sweep, float position, float& total, std::uint32_t& count) {
        if (sweep == 0.0F) return;
        total += sweep > 0.0F ? mix(setting.minimum, setting.maximum, position)
                              : mix(setting.maximum, setting.minimum, position);
        ++count;
    };
    accumulate(setting.xSweep, x, result, activeSweeps);
    accumulate(setting.ySweep, y, result, activeSweeps);
    accumulate(setting.cohortSweep, cohortNormalized, result, activeSweeps);
    if (activeSweeps == 0U) result = setting.value;
    else result /= static_cast<float>(activeSweeps);
    if (setting.jitter != 0.0F) {
        const float random = source_hash(static_cast<float>(frameNumber) + result,
                                         x + y * 1000.0F) * 2.0F - 1.0F;
        result += setting.jitter * result * random;
    }
    return result;
}

FluoddityQualityProfile fluoddity_quality_profile(FluoddityQuality quality) noexcept {
    switch (quality) {
    case FluoddityQuality::Low:
        return {quality, 250'000U, 128U, FluoddityAccumulationMode::FixedPointSigned};
    case FluoddityQuality::Medium:
        return {quality, 1'000'000U, 192U, FluoddityAccumulationMode::FixedPointSigned};
    case FluoddityQuality::High:
        return {quality, 4'000'000U, 256U, FluoddityAccumulationMode::FixedPointSigned};
    case FluoddityQuality::Custom:
        return {quality, 1'000'000U, 192U, FluoddityAccumulationMode::FixedPointSigned};
    }
    return {};
}

FluoddityMemoryEstimate estimate_fluoddity_memory(
    const FluoddityQualityProfile& profile,
    std::uint32_t cohortCount) noexcept {
    FluoddityMemoryEstimate estimate;
    const std::uint64_t resolution = profile.trailResolution;
    const std::uint64_t voxels = resolution * resolution * resolution;
    estimate.particleBytes = static_cast<std::uint64_t>(profile.particleCapacity) * 32U;
    estimate.trailBytes = voxels * 8U * 2U;
    estimate.accumulationBytes = profile.accumulationMode ==
                                         FluoddityAccumulationMode::FixedPointSigned
                                     ? voxels * 16U
                                     : 0U;
    estimate.ruleBytes = static_cast<std::uint64_t>(
                             std::clamp(cohortCount, 1U, kFluoddityMaximumCohorts)) *
                         480U;
    estimate.supportingBytes = 16U * 1024U * 1024U;
    estimate.totalBytes = estimate.particleBytes + estimate.trailBytes +
                          estimate.accumulationBytes + estimate.ruleBytes +
                          estimate.supportingBytes;
    return estimate;
}

std::string fluoddity_parameter_name(FluoddityParameter parameter) {
    const std::size_t index = parameter_index(parameter);
    return index < kParameterSpecs.size() ? std::string(kParameterSpecs[index].displayName)
                                          : std::string("Unknown");
}

std::string fluoddity_quality_name(FluoddityQuality quality) {
    switch (quality) {
    case FluoddityQuality::Low: return "Low";
    case FluoddityQuality::Medium: return "Medium";
    case FluoddityQuality::High: return "High";
    case FluoddityQuality::Custom: return "Custom";
    }
    return "Unknown";
}

} // namespace dve
