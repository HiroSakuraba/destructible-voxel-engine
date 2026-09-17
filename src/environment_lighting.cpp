#include "dve/environment_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dve {
namespace {
constexpr float kPi = 3.14159265358979323846F;

Float3 vadd(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 vsub(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 vmul(Float3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
Float3 hadamard(Float3 a, Float3 b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length3(Float3 value) noexcept { return std::sqrt(std::max(0.0F, dot3(value, value))); }
Float3 normalize3(Float3 value, Float3 fallback = {0.0F, 1.0F, 0.0F}) noexcept {
    const float length = length3(value);
    return length > 1.0e-8F ? vmul(value, 1.0F / length) : fallback;
}
Float3 clamp_positive(Float3 value) noexcept {
    return {std::max(0.0F, value.x), std::max(0.0F, value.y), std::max(0.0F, value.z)};
}
bool finite3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
Float3 lerp3(Float3 a, Float3 b, float weight) noexcept {
    return vadd(vmul(a, 1.0F - weight), vmul(b, weight));
}

float radical_inverse(std::uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Float2 hammersley(std::uint32_t index, std::uint32_t count) noexcept {
    return {static_cast<float>(index) / static_cast<float>(count), radical_inverse(index)};
}

void orthonormal_basis(Float3 normal, Float3& tangent, Float3& bitangent) noexcept {
    const Float3 up = std::abs(normal.y) < 0.999F ? Float3{0.0F, 1.0F, 0.0F}
                                                     : Float3{1.0F, 0.0F, 0.0F};
    tangent = normalize3(cross3(up, normal), {1.0F, 0.0F, 0.0F});
    bitangent = cross3(normal, tangent);
}

Float3 cosine_sample(Float2 sample, Float3 normal) noexcept {
    const float radius = std::sqrt(sample.x);
    const float phi = 2.0F * kPi * sample.y;
    Float3 tangent{};
    Float3 bitangent{};
    orthonormal_basis(normal, tangent, bitangent);
    return normalize3(vadd(vadd(vmul(tangent, radius * std::cos(phi)),
                                 vmul(bitangent, radius * std::sin(phi))),
                           vmul(normal, std::sqrt(std::max(0.0F, 1.0F - sample.x)))));
}

Float3 importance_sample_ggx(Float2 sample, Float3 normal, float roughness) noexcept {
    const float alpha = std::max(0.001F, roughness * roughness);
    const float phi = 2.0F * kPi * sample.x;
    const float cosineTheta =
        std::sqrt((1.0F - sample.y) / (1.0F + (alpha * alpha - 1.0F) * sample.y));
    const float sineTheta = std::sqrt(std::max(0.0F, 1.0F - cosineTheta * cosineTheta));
    Float3 tangent{};
    Float3 bitangent{};
    orthonormal_basis(normal, tangent, bitangent);
    return normalize3(vadd(vadd(vmul(tangent, sineTheta * std::cos(phi)),
                                 vmul(bitangent, sineTheta * std::sin(phi))),
                           vmul(normal, cosineTheta)));
}

Float3 reflect3(Float3 incident, Float3 normal) noexcept {
    return vsub(incident, vmul(normal, 2.0F * dot3(incident, normal)));
}

// Split-sum IBL uses Schlick-GGX k = alpha / 2, with alpha = perceptualRoughness^2.
float geometry_schlick_ibl(float ndot, float roughness) noexcept {
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5F;
    return ndot / (ndot * (1.0F - k) + k);
}

float geometry_smith_ibl(float ndotv, float ndotl, float roughness) noexcept {
    return geometry_schlick_ibl(ndotv, roughness) * geometry_schlick_ibl(ndotl, roughness);
}

IblBrdfSample integrate_brdf(float ndotv, float roughness, std::uint32_t sampleCount) noexcept {
    const Float3 view{std::sqrt(std::max(0.0F, 1.0F - ndotv * ndotv)), 0.0F, ndotv};
    const Float3 normal{0.0F, 0.0F, 1.0F};
    float scale = 0.0F;
    float bias = 0.0F;
    for (std::uint32_t index = 0; index < sampleCount; ++index) {
        const Float3 halfVector =
            importance_sample_ggx(hammersley(index, sampleCount), normal, roughness);
        const Float3 light = normalize3(
            vsub(vmul(halfVector, 2.0F * dot3(view, halfVector)), view));
        const float ndotl = std::max(0.0F, light.z);
        if (ndotl <= 0.0F) continue;
        const float ndoth = std::max(0.0F, halfVector.z);
        const float vdoth = std::max(0.0F, dot3(view, halfVector));
        const float geometry = geometry_smith_ibl(ndotv, ndotl, roughness);
        const float visibility =
            (geometry * vdoth) / std::max(ndoth * ndotv, 1.0e-6F);
        const float fresnelComplement = std::pow(1.0F - vdoth, 5.0F);
        scale += (1.0F - fresnelComplement) * visibility;
        bias += fresnelComplement * visibility;
    }
    const float inverseCount = 1.0F / static_cast<float>(sampleCount);
    return {scale * inverseCount, bias * inverseCount};
}

Float3 texel(const EnvironmentCube& cube, CubeFace face, std::uint32_t x,
             std::uint32_t y) noexcept {
    return cube.texels[cube.face_offset(face) + static_cast<std::size_t>(y) * cube.resolution + x];
}

Float3 seamless_texel(const EnvironmentCube& cube, CubeFace face, int x, int y) noexcept {
    const int resolution = static_cast<int>(cube.resolution);
    if (x >= 0 && x < resolution && y >= 0 && y < resolution) {
        return texel(cube, face, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    }
    const float resolutionFloat = static_cast<float>(cube.resolution);
    const Float2 extendedUv{(static_cast<float>(x) + 0.5F) / resolutionFloat,
                            (static_cast<float>(y) + 0.5F) / resolutionFloat};
    const CubeLookup adjacent =
        direction_to_cube_lookup(cube_lookup_to_direction(face, extendedUv));
    const int adjacentX = std::clamp(
        static_cast<int>(std::lround(adjacent.uv.x * resolutionFloat - 0.5F)), 0,
        resolution - 1);
    const int adjacentY = std::clamp(
        static_cast<int>(std::lround(adjacent.uv.y * resolutionFloat - 0.5F)), 0,
        resolution - 1);
    return texel(cube, adjacent.face, static_cast<std::uint32_t>(adjacentX),
                 static_cast<std::uint32_t>(adjacentY));
}

float distribution_ggx(float ndoth, float roughness) noexcept {
    const float alpha = std::max(0.001F, roughness * roughness);
    const float alphaSquared = alpha * alpha;
    const float denominator = ndoth * ndoth * (alphaSquared - 1.0F) + 1.0F;
    return alphaSquared / std::max(kPi * denominator * denominator, 1.0e-9F);
}

float ggx_source_lod(Float3 normal, Float3 view, Float3 halfVector, float roughness,
                     std::uint32_t sampleCount, std::uint32_t sourceResolution,
                     std::size_t maximumLod) noexcept {
    if (roughness <= 0.0F || sourceResolution == 0U || maximumLod == 0U) return 0.0F;
    const float ndoth = std::max(0.0F, dot3(normal, halfVector));
    const float vdoth = std::max(1.0e-6F, dot3(view, halfVector));
    const float pdf =
        std::max(distribution_ggx(ndoth, roughness) * ndoth / (4.0F * vdoth), 1.0e-6F);
    const float sampleSolidAngle = 1.0F / (static_cast<float>(sampleCount) * pdf);
    const float resolution = static_cast<float>(sourceResolution);
    const float texelSolidAngle = 4.0F * kPi / (6.0F * resolution * resolution);
    return std::clamp(0.5F * std::log2(sampleSolidAngle / texelSolidAngle), 0.0F,
                      static_cast<float>(maximumLod));
}

float specular_occlusion(float ndotv, float ambientOcclusion, float roughness) noexcept {
    const float ao = std::clamp(ambientOcclusion, 0.0F, 1.0F);
    const float exponent = std::exp2(-16.0F * std::clamp(roughness, 0.0F, 1.0F) - 1.0F);
    return std::clamp(std::pow(std::max(0.0F, ndotv + ao), exponent) - 1.0F + ao,
                      0.0F, 1.0F);
}

IblBrdfSample sample_brdf(const IblBakeResult& ibl, float ndotv, float roughness) noexcept {
    if (ibl.brdfResolution == 0U || ibl.brdfLut.empty()) return {1.0F, 0.0F};
    const float maximumIndex = static_cast<float>(ibl.brdfResolution - 1U);
    const float fx = std::clamp(ndotv, 0.0F, 1.0F) * maximumIndex;
    const float fy = std::clamp(roughness, 0.0F, 1.0F) * maximumIndex;
    const auto x0 = static_cast<std::uint32_t>(fx);
    const auto y0 = static_cast<std::uint32_t>(fy);
    const auto x1 = std::min(x0 + 1U, ibl.brdfResolution - 1U);
    const auto y1 = std::min(y0 + 1U, ibl.brdfResolution - 1U);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return ibl.brdfLut[static_cast<std::size_t>(y) * ibl.brdfResolution + x];
    };
    const IblBrdfSample a = at(x0, y0);
    const IblBrdfSample b = at(x1, y0);
    const IblBrdfSample c = at(x0, y1);
    const IblBrdfSample d = at(x1, y1);
    return {(a.scale * (1.0F - tx) + b.scale * tx) * (1.0F - ty) +
                (c.scale * (1.0F - tx) + d.scale * tx) * ty,
            (a.bias * (1.0F - tx) + b.bias * tx) * (1.0F - ty) +
                (c.bias * (1.0F - tx) + d.bias * tx) * ty};
}
} // namespace

bool EnvironmentImage2D::validate(std::string* error) const noexcept {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (width == 0U || height == 0U) return fail("environment image dimensions must be nonzero");
    if (pixels.size() != static_cast<std::size_t>(width) * height)
        return fail("environment image pixel count mismatch");
    for (const Float3 pixel : pixels)
        if (!finite3(pixel)) return fail("environment image contains non-finite pixels");
    return true;
}

bool EnvironmentCube::validate(std::string* error) const noexcept {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (resolution == 0U) return fail("environment cube resolution must be nonzero");
    if (texels.size() != 6ULL * resolution * resolution)
        return fail("environment cube texel count mismatch");
    for (const Float3 value : texels)
        if (!finite3(value)) return fail("environment cube contains non-finite texels");
    return true;
}

std::size_t EnvironmentCube::face_offset(CubeFace face) const noexcept {
    return static_cast<std::size_t>(face) * resolution * resolution;
}

bool EnvironmentCubeMipChain::validate(std::string* error) const noexcept {
    if (levels.empty()) {
        if (error) *error = "environment mip chain is empty";
        return false;
    }
    std::uint32_t expectedResolution = levels.front().resolution;
    for (const auto& level : levels) {
        if (!level.validate(error) || level.resolution != expectedResolution) return false;
        expectedResolution = std::max(1U, expectedResolution / 2U);
    }
    return true;
}

CubeLookup direction_to_cube_lookup(Float3 direction) noexcept {
    direction = normalize3(direction, {0.0F, 0.0F, 1.0F});
    const float absoluteX = std::abs(direction.x);
    const float absoluteY = std::abs(direction.y);
    const float absoluteZ = std::abs(direction.z);
    CubeLookup output{};
    float u = 0.0F;
    float v = 0.0F;
    float major = 1.0F;
    if (absoluteX >= absoluteY && absoluteX >= absoluteZ) {
        major = absoluteX;
        if (direction.x >= 0.0F) {
            output.face = CubeFace::PositiveX;
            u = -direction.z;
            v = -direction.y;
        } else {
            output.face = CubeFace::NegativeX;
            u = direction.z;
            v = -direction.y;
        }
    } else if (absoluteY >= absoluteZ) {
        major = absoluteY;
        if (direction.y >= 0.0F) {
            output.face = CubeFace::PositiveY;
            u = direction.x;
            v = direction.z;
        } else {
            output.face = CubeFace::NegativeY;
            u = direction.x;
            v = -direction.z;
        }
    } else {
        major = absoluteZ;
        if (direction.z >= 0.0F) {
            output.face = CubeFace::PositiveZ;
            u = direction.x;
            v = -direction.y;
        } else {
            output.face = CubeFace::NegativeZ;
            u = -direction.x;
            v = -direction.y;
        }
    }
    output.uv = {0.5F * (u / major + 1.0F), 0.5F * (v / major + 1.0F)};
    return output;
}

Float3 cube_lookup_to_direction(CubeFace face, Float2 uv) noexcept {
    const float u = 2.0F * uv.x - 1.0F;
    const float v = 2.0F * uv.y - 1.0F;
    switch (face) {
        case CubeFace::PositiveX: return normalize3({1.0F, -v, -u});
        case CubeFace::NegativeX: return normalize3({-1.0F, -v, u});
        case CubeFace::PositiveY: return normalize3({u, 1.0F, v});
        case CubeFace::NegativeY: return normalize3({u, -1.0F, -v});
        case CubeFace::PositiveZ: return normalize3({u, -v, 1.0F});
        case CubeFace::NegativeZ: return normalize3({-u, -v, -1.0F});
    }
    return {0.0F, 0.0F, 1.0F};
}

Float3 sample_environment_image(const EnvironmentImage2D& image, Float2 uv) noexcept {
    if (image.width == 0U || image.height == 0U || image.pixels.empty()) return {};
    uv.x -= std::floor(uv.x);
    uv.y = std::clamp(uv.y, 0.0F, 1.0F);
    const float fx = uv.x * static_cast<float>(image.width) - 0.5F;
    const float fy = uv.y * static_cast<float>(image.height) - 0.5F;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto at = [&](int x, int y) {
        const int width = static_cast<int>(image.width);
        x %= width;
        if (x < 0) x += width;
        y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
        return image.pixels[static_cast<std::size_t>(y) * image.width +
                            static_cast<std::uint32_t>(x)];
    };
    return lerp3(lerp3(at(x0, y0), at(x0 + 1, y0), tx),
                 lerp3(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

Float3 sample_environment_cube(const EnvironmentCube& cube, Float3 direction) noexcept {
    if (cube.resolution == 0U || cube.texels.empty()) return {};
    const CubeLookup lookup = direction_to_cube_lookup(direction);
    const float resolution = static_cast<float>(cube.resolution);
    const float fx = lookup.uv.x * resolution - 0.5F;
    const float fy = lookup.uv.y * resolution - 0.5F;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    return lerp3(
        lerp3(seamless_texel(cube, lookup.face, x0, y0),
              seamless_texel(cube, lookup.face, x0 + 1, y0), tx),
        lerp3(seamless_texel(cube, lookup.face, x0, y0 + 1),
              seamless_texel(cube, lookup.face, x0 + 1, y0 + 1), tx),
        ty);
}

Float3 sample_environment_cube_lod(const EnvironmentCubeMipChain& chain, Float3 direction,
                                   float lod) noexcept {
    if (chain.levels.empty()) return {};
    lod = std::clamp(lod, 0.0F, static_cast<float>(chain.levels.size() - 1U));
    const auto first = static_cast<std::size_t>(lod);
    const auto second = std::min(first + 1U, chain.levels.size() - 1U);
    return lerp3(sample_environment_cube(chain.levels[first], direction),
                 sample_environment_cube(chain.levels[second], direction),
                 lod - static_cast<float>(first));
}

EnvironmentCubeMipChain build_environment_cube_mip_chain(const EnvironmentCube& source) {
    std::string error;
    if (!source.validate(&error)) throw std::invalid_argument(error);
    EnvironmentCubeMipChain chain;
    chain.levels.push_back(source);
    while (chain.levels.back().resolution > 1U) {
        const EnvironmentCube& previous = chain.levels.back();
        const std::uint32_t resolution = std::max(1U, previous.resolution / 2U);
        EnvironmentCube next{resolution, std::vector<Float3>(6ULL * resolution * resolution)};
        for (std::uint32_t face = 0; face < 6U; ++face) {
            for (std::uint32_t y = 0; y < resolution; ++y) {
                for (std::uint32_t x = 0; x < resolution; ++x) {
                    Float3 sum{};
                    for (std::uint32_t offsetY = 0; offsetY < 2U; ++offsetY) {
                        for (std::uint32_t offsetX = 0; offsetX < 2U; ++offsetX) {
                            const Float2 uv{
                                (static_cast<float>(2U * x + offsetX) + 0.5F) /
                                    static_cast<float>(previous.resolution),
                                (static_cast<float>(2U * y + offsetY) + 0.5F) /
                                    static_cast<float>(previous.resolution)};
                            sum = vadd(sum, sample_environment_cube(
                                                previous,
                                                cube_lookup_to_direction(
                                                    static_cast<CubeFace>(face), uv)));
                        }
                    }
                    next.texels[next.face_offset(static_cast<CubeFace>(face)) +
                                static_cast<std::size_t>(y) * resolution + x] = vmul(sum, 0.25F);
                }
            }
        }
        chain.levels.push_back(std::move(next));
    }
    return chain;
}

EnvironmentCube convert_equirectangular_to_cube(const EnvironmentImage2D& image,
                                                 std::uint32_t faceResolution) {
    std::string error;
    if (!image.validate(&error) || faceResolution == 0U)
        throw std::invalid_argument(error.empty() ? "invalid cube resolution" : error);
    EnvironmentCube cube{faceResolution,
                         std::vector<Float3>(6ULL * faceResolution * faceResolution)};
    const float resolution = static_cast<float>(faceResolution);
    for (std::uint32_t face = 0; face < 6U; ++face) {
        for (std::uint32_t y = 0; y < faceResolution; ++y) {
            for (std::uint32_t x = 0; x < faceResolution; ++x) {
                const Float3 direction = cube_lookup_to_direction(
                    static_cast<CubeFace>(face),
                    {(static_cast<float>(x) + 0.5F) / resolution,
                     (static_cast<float>(y) + 0.5F) / resolution});
                const float u = std::atan2(direction.z, direction.x) / (2.0F * kPi) + 0.5F;
                const float v = std::acos(std::clamp(direction.y, -1.0F, 1.0F)) / kPi;
                cube.texels[cube.face_offset(static_cast<CubeFace>(face)) +
                            static_cast<std::size_t>(y) * faceResolution + x] =
                    sample_environment_image(image, {u, v});
            }
        }
    }
    return cube;
}

IblBrdfSample integrate_environment_brdf(float ndotv, float roughness,
                                               std::uint32_t sampleCount) noexcept {
    if (sampleCount == 0U) return {};
    return integrate_brdf(std::clamp(ndotv, 0.0F, 1.0F),
                          std::clamp(roughness, 0.0F, 1.0F), sampleCount);
}

bool IblBakeSettings::validate(std::string* error) const noexcept {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (irradianceResolution == 0U || specularResolution == 0U || specularMipLevels == 0U ||
        sampleCount < 4U || sampleCount > 4096U || brdfResolution == 0U)
        return fail("invalid IBL bake settings");
    return true;
}

bool IblBakeResult::validate(std::string* error) const noexcept {
    if (!diffuseIrradiance.validate(error) || !specularPrefilter.validate(error)) return false;
    if (brdfResolution == 0U ||
        brdfLut.size() != static_cast<std::size_t>(brdfResolution) * brdfResolution) {
        if (error) *error = "BRDF LUT size mismatch";
        return false;
    }
    for (const auto value : brdfLut) {
        if (!std::isfinite(value.scale) || !std::isfinite(value.bias)) {
            if (error) *error = "BRDF LUT non-finite";
            return false;
        }
    }
    return true;
}

IblBakeResult bake_image_based_lighting(const EnvironmentCube& source,
                                         const IblBakeSettings& settings) {
    std::string error;
    if (!source.validate(&error) || !settings.validate(&error)) throw std::invalid_argument(error);
    IblBakeResult output;
    output.diffuseIrradiance = {
        settings.irradianceResolution,
        std::vector<Float3>(6ULL * settings.irradianceResolution * settings.irradianceResolution)};
    const float irradianceResolution = static_cast<float>(settings.irradianceResolution);
    for (std::uint32_t face = 0; face < 6U; ++face) {
        for (std::uint32_t y = 0; y < settings.irradianceResolution; ++y) {
            for (std::uint32_t x = 0; x < settings.irradianceResolution; ++x) {
                const Float3 normal = cube_lookup_to_direction(
                    static_cast<CubeFace>(face),
                    {(static_cast<float>(x) + 0.5F) / irradianceResolution,
                     (static_cast<float>(y) + 0.5F) / irradianceResolution});
                Float3 sum{};
                for (std::uint32_t index = 0; index < settings.sampleCount; ++index) {
                    sum = vadd(sum, sample_environment_cube(
                                        source,
                                        cosine_sample(hammersley(index, settings.sampleCount),
                                                      normal)));
                }
                output.diffuseIrradiance.texels[
                    output.diffuseIrradiance.face_offset(static_cast<CubeFace>(face)) +
                    static_cast<std::size_t>(y) * settings.irradianceResolution + x] =
                    vmul(sum, kPi / static_cast<float>(settings.sampleCount));
            }
        }
    }

    const EnvironmentCubeMipChain sourceMips = build_environment_cube_mip_chain(source);
    std::uint32_t outputResolution = settings.specularResolution;
    for (std::uint32_t mip = 0; mip < settings.specularMipLevels; ++mip) {
        EnvironmentCube level{outputResolution,
                              std::vector<Float3>(6ULL * outputResolution * outputResolution)};
        const float roughness = settings.specularMipLevels == 1U
                                    ? 0.0F
                                    : static_cast<float>(mip) /
                                          static_cast<float>(settings.specularMipLevels - 1U);
        const float resolution = static_cast<float>(outputResolution);
        for (std::uint32_t face = 0; face < 6U; ++face) {
            for (std::uint32_t y = 0; y < outputResolution; ++y) {
                for (std::uint32_t x = 0; x < outputResolution; ++x) {
                    const Float3 normal = cube_lookup_to_direction(
                        static_cast<CubeFace>(face),
                        {(static_cast<float>(x) + 0.5F) / resolution,
                         (static_cast<float>(y) + 0.5F) / resolution});
                    const Float3 view = normal;
                    Float3 sum{};
                    float weight = 0.0F;
                    for (std::uint32_t index = 0; index < settings.sampleCount; ++index) {
                        const Float3 halfVector = importance_sample_ggx(
                            hammersley(index, settings.sampleCount), normal, roughness);
                        const Float3 light = normalize3(
                            vsub(vmul(halfVector, 2.0F * dot3(view, halfVector)), view));
                        const float ndotl = std::max(0.0F, dot3(normal, light));
                        if (ndotl <= 0.0F) continue;
                        const float sourceLod = ggx_source_lod(
                            normal, view, halfVector, roughness, settings.sampleCount,
                            source.resolution, sourceMips.levels.size() - 1U);
                        sum = vadd(sum, vmul(sample_environment_cube_lod(sourceMips, light,
                                                                        sourceLod),
                                              ndotl));
                        weight += ndotl;
                    }
                    level.texels[level.face_offset(static_cast<CubeFace>(face)) +
                                 static_cast<std::size_t>(y) * outputResolution + x] =
                        weight > 0.0F ? vmul(sum, 1.0F / weight)
                                      : sample_environment_cube(source, normal);
                }
            }
        }
        output.specularPrefilter.levels.push_back(std::move(level));
        outputResolution = std::max(1U, outputResolution / 2U);
    }

    output.brdfResolution = settings.brdfResolution;
    output.brdfLut.resize(static_cast<std::size_t>(settings.brdfResolution) *
                          settings.brdfResolution);
    const float brdfResolution = static_cast<float>(settings.brdfResolution);
    for (std::uint32_t y = 0; y < settings.brdfResolution; ++y) {
        for (std::uint32_t x = 0; x < settings.brdfResolution; ++x) {
            output.brdfLut[static_cast<std::size_t>(y) * settings.brdfResolution + x] =
                integrate_environment_brdf((static_cast<float>(x) + 0.5F) / brdfResolution,
                               (static_cast<float>(y) + 0.5F) / brdfResolution,
                               settings.sampleCount);
        }
    }
    return output;
}

Float3 evaluate_image_based_lighting(const IblBakeResult& ibl,
                                     const IblMaterialInput& material) noexcept {
    if (ibl.specularPrefilter.levels.empty()) return {};
    const Float3 normal = normalize3(material.normal);
    const Float3 view = normalize3(material.viewDirection, {0.0F, 0.0F, 1.0F});
    const Float3 reflection = reflect3(vmul(view, -1.0F), normal);
    const float metallic = std::clamp(material.metallic, 0.0F, 1.0F);
    const float roughness = std::clamp(material.roughness, 0.0F, 1.0F);
    const float specular = std::clamp(material.specular, 0.0F, 1.0F);
    const float ao = std::clamp(material.ambientOcclusion, 0.0F, 1.0F);
    const float ndotv = std::max(0.0F, dot3(normal, view));
    const Float3 dielectricF0{0.08F * specular, 0.08F * specular, 0.08F * specular};
    const Float3 f0 = lerp3(dielectricF0, clamp_positive(material.baseColor), metallic);
    const IblBrdfSample brdf = sample_brdf(ibl, ndotv, roughness);
    const Float3 diffuse = hadamard(
        sample_environment_cube(ibl.diffuseIrradiance, normal),
        vmul(clamp_positive(material.baseColor), (1.0F - metallic) / kPi));
    const float maximumLod = static_cast<float>(ibl.specularPrefilter.levels.size() - 1U);
    const Float3 prefiltered = sample_environment_cube_lod(
        ibl.specularPrefilter, reflection, roughness * maximumLod);
    const Float3 specularLighting = hadamard(
        prefiltered,
        vadd(vmul(f0, brdf.scale), {brdf.bias, brdf.bias, brdf.bias}));
    return vadd(vmul(diffuse, ao),
                vmul(specularLighting, specular_occlusion(ndotv, ao, roughness)));
}

bool ReflectionProbe::validate(std::string* error) const noexcept {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (id == 0U) return fail("reflection probe id must be nonzero");
    if (!finite3(position) || !finite3(halfExtents) || halfExtents.x <= 0.0F ||
        halfExtents.y <= 0.0F || halfExtents.z <= 0.0F || !std::isfinite(blendDistance) ||
        blendDistance < 0.0F || !std::isfinite(intensity) || intensity < 0.0F)
        return fail("invalid reflection probe");
    return true;
}

std::vector<ReflectionProbeWeight> select_reflection_probes(
    const std::vector<ReflectionProbe>& probes, Float3 worldPosition,
    std::uint32_t maximumProbeCount) {
    struct Candidate {
        ReflectionProbeId id{};
        float weight{};
        std::int32_t priority{};
    };
    std::vector<Candidate> candidates;
    for (const auto& probe : probes) {
        if (!probe.validate()) continue;
        const Float3 distance{std::abs(worldPosition.x - probe.position.x) - probe.halfExtents.x,
                              std::abs(worldPosition.y - probe.position.y) - probe.halfExtents.y,
                              std::abs(worldPosition.z - probe.position.z) - probe.halfExtents.z};
        const float outside = std::max({distance.x, distance.y, distance.z});
        if (outside > probe.blendDistance) continue;
        const float weight = probe.intensity *
            (probe.blendDistance > 0.0F
                 ? std::clamp(1.0F - outside / probe.blendDistance, 0.0F, 1.0F)
                 : 1.0F);
        if (weight > 0.0F) candidates.push_back({probe.id, weight, probe.priority});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.id < b.id;
    });
    if (candidates.size() > maximumProbeCount) candidates.resize(maximumProbeCount);
    float totalWeight = 0.0F;
    for (const auto& candidate : candidates) totalWeight += candidate.weight;
    std::vector<ReflectionProbeWeight> output;
    output.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        output.push_back({candidate.id, totalWeight > 0.0F ? candidate.weight / totalWeight : 0.0F});
    }
    return output;
}

Float3 box_project_reflection_direction(const ReflectionProbe& probe, Float3 worldPosition,
                                        Float3 reflectionDirection) noexcept {
    reflectionDirection = normalize3(reflectionDirection, {0.0F, 0.0F, 1.0F});
    if (!probe.boxProjection) return reflectionDirection;
    const Float3 minimum = vsub(probe.position, probe.halfExtents);
    const Float3 maximum = vadd(probe.position, probe.halfExtents);
    float distance = std::numeric_limits<float>::infinity();
    const auto test_axis = [&](float position, float direction, float low, float high) {
        if (std::abs(direction) < 1.0e-7F) return;
        const float candidate = (direction > 0.0F ? high - position : low - position) / direction;
        if (candidate > 0.0F) distance = std::min(distance, candidate);
    };
    test_axis(worldPosition.x, reflectionDirection.x, minimum.x, maximum.x);
    test_axis(worldPosition.y, reflectionDirection.y, minimum.y, maximum.y);
    test_axis(worldPosition.z, reflectionDirection.z, minimum.z, maximum.z);
    return std::isfinite(distance)
               ? normalize3(vsub(vadd(worldPosition, vmul(reflectionDirection, distance)),
                                  probe.position),
                            reflectionDirection)
               : reflectionDirection;
}
} // namespace dve
