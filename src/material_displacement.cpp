#include "dve/material_displacement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace dve {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] float saturate(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }
[[nodiscard]] Float3 add3(Float3 a, Float3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
[[nodiscard]] Float3 scale3(Float3 value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}
[[nodiscard]] float dot3(Float3 a, Float3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
[[nodiscard]] Float3 normalize3(Float3 value, Float3 fallback = {0.0F, 0.0F, 1.0F}) noexcept {
    const float squared = dot3(value, value);
    if (!(squared > 1.0e-20F) || !finite(squared)) return fallback;
    return scale3(value, 1.0F/std::sqrt(squared));
}
[[nodiscard]] Float2 average(Float2 a, Float2 b) noexcept {
    return {(a.x+b.x)*0.5F,(a.y+b.y)*0.5F};
}
[[nodiscard]] Float3 average(Float3 a, Float3 b) noexcept { return scale3(add3(a,b),0.5F); }
[[nodiscard]] Float4 average(Float4 a, Float4 b) noexcept {
    return {(a.x+b.x)*0.5F,(a.y+b.y)*0.5F,(a.z+b.z)*0.5F,(a.w+b.w)*0.5F};
}

[[nodiscard]] float wrap(float value, ImportedWrapMode mode) noexcept {
    if (mode == ImportedWrapMode::ClampToEdge) return std::clamp(value, 0.0F, 1.0F);
    const float whole = std::floor(value);
    const float local = value - whole;
    if (mode == ImportedWrapMode::MirroredRepeat &&
        (static_cast<long long>(whole) & 1LL) != 0LL) return 1.0F - local;
    return local;
}

[[nodiscard]] float sample_image_red(
    const CookedPolygonAsset& asset,
    const PolygonTextureBinding& binding,
    Float2 uv) noexcept {
    if (!binding.texture || *binding.texture >= asset.textures.size()) return 0.5F;
    const PolygonTexture& texture = asset.textures[*binding.texture];
    if (texture.imageIndex >= asset.images.size()) return 0.5F;
    const PolygonImage& image = asset.images[texture.imageIndex];
    if (image.width == 0U || image.height == 0U ||
        image.rgba8.size() != static_cast<std::size_t>(image.width) * image.height * 4U)
        return 0.5F;
    const PolygonSampler fallback{};
    const PolygonSampler& sampler = texture.samplerIndex && *texture.samplerIndex < asset.samplers.size()
        ? asset.samplers[*texture.samplerIndex] : fallback;
    const float u = wrap(uv.x, sampler.wrapS);
    const float v = wrap(uv.y, sampler.wrapT);
    const float x = u * static_cast<float>(image.width - 1U);
    const float y = v * static_cast<float>(image.height - 1U);
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(x));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(y));
    const std::uint32_t x1 = std::min(x0 + 1U, image.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, image.height - 1U);
    const auto red = [&](std::uint32_t sx, std::uint32_t sy) {
        return static_cast<float>(image.rgba8[
            (static_cast<std::size_t>(sy) * image.width + sx) * 4U]) / 255.0F;
    };
    if (sampler.magFilter == ImportedTextureFilter::Nearest)
        return red(static_cast<std::uint32_t>(std::round(x)),
                   static_cast<std::uint32_t>(std::round(y)));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float top = red(x0,y0) + (red(x1,y0)-red(x0,y0))*tx;
    const float bottom = red(x0,y1) + (red(x1,y1)-red(x0,y1))*tx;
    return top + (bottom-top)*ty;
}

[[nodiscard]] PolygonVertex midpoint(const PolygonVertex& a, const PolygonVertex& b) noexcept {
    PolygonVertex result;
    result.position = average(a.position,b.position);
    result.normal = normalize3(average(a.normal,b.normal));
    const Float3 tangent = normalize3(average(
        Float3{a.tangent.x,a.tangent.y,a.tangent.z},
        Float3{b.tangent.x,b.tangent.y,b.tangent.z}), {1.0F,0.0F,0.0F});
    result.tangent = {tangent.x,tangent.y,tangent.z,(a.tangent.w+b.tangent.w)<0.0F?-1.0F:1.0F};
    result.texcoord = average(a.texcoord,b.texcoord);
    result.texcoord1 = average(a.texcoord1,b.texcoord1);
    result.color = average(a.color,b.color);
    return result;
}

[[nodiscard]] bool subdivide_once(CookedPolygonAsset& asset,
                                  std::uint64_t maximumVertices,
                                  std::string* error) {
    std::vector<std::uint32_t> newIndices;
    newIndices.reserve(asset.indices.size()*4U);
    std::vector<PolygonSubmesh> newSubmeshes;
    newSubmeshes.reserve(asset.submeshes.size());

    for (const PolygonSubmesh& submesh : asset.submeshes) {
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> midpointCache;
        const std::uint32_t first = static_cast<std::uint32_t>(newIndices.size());
        auto midpoint_index = [&](std::uint32_t a, std::uint32_t b) -> std::optional<std::uint32_t> {
            const std::pair<std::uint32_t,std::uint32_t> key{std::min(a,b),std::max(a,b)};
            if (const auto found = midpointCache.find(key); found != midpointCache.end()) return found->second;
            if (asset.vertices.size() >= maximumVertices) return std::nullopt;
            const std::uint32_t index = static_cast<std::uint32_t>(asset.vertices.size());
            asset.vertices.push_back(midpoint(asset.vertices[a],asset.vertices[b]));
            midpointCache.emplace(key,index);
            return index;
        };
        for (std::uint32_t offset=0U; offset<submesh.indexCount; offset+=3U) {
            const std::uint32_t a=asset.indices[submesh.firstIndex+offset];
            const std::uint32_t b=asset.indices[submesh.firstIndex+offset+1U];
            const std::uint32_t c=asset.indices[submesh.firstIndex+offset+2U];
            const auto ab=midpoint_index(a,b),bc=midpoint_index(b,c),ca=midpoint_index(c,a);
            if (!ab || !bc || !ca) { if(error)*error="vertex displacement subdivision exceeded the vertex limit"; return false; }
            const std::array<std::uint32_t,12> split{{
                a,*ab,*ca, *ab,b,*bc, *ca,*bc,c, *ab,*bc,*ca}};
            newIndices.insert(newIndices.end(),split.begin(),split.end());
        }
        PolygonSubmesh replacement=submesh;
        replacement.firstIndex=first;
        replacement.indexCount=static_cast<std::uint32_t>(newIndices.size())-first;
        newSubmeshes.push_back(std::move(replacement));
    }
    asset.indices=std::move(newIndices);
    asset.submeshes=std::move(newSubmeshes);
    return true;
}

void recompute_bounds(CookedPolygonAsset& asset) noexcept {
    if (asset.vertices.empty()) { asset.bounds={}; return; }
    Float3 minimum=asset.vertices.front().position;
    Float3 maximum=minimum;
    for (const PolygonVertex& vertex:asset.vertices) {
        minimum.x=std::min(minimum.x,vertex.position.x);minimum.y=std::min(minimum.y,vertex.position.y);minimum.z=std::min(minimum.z,vertex.position.z);
        maximum.x=std::max(maximum.x,vertex.position.x);maximum.y=std::max(maximum.y,vertex.position.y);maximum.z=std::max(maximum.z,vertex.position.z);
    }
    asset.bounds={minimum,maximum};
}

[[nodiscard]] std::vector<std::uint32_t> vertex_materials(
    const CookedPolygonAsset& asset,
    std::uint64_t& sharedMaterialVertices) {
    const std::uint32_t unset=std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> result(asset.vertices.size(),unset);
    std::vector<bool> reported(asset.vertices.size(),false);
    for (const PolygonSubmesh& submesh:asset.submeshes) {
        for (std::uint32_t offset=0U;offset<submesh.indexCount;++offset) {
            const std::uint32_t vertex=asset.indices[submesh.firstIndex+offset];
            if (result[vertex]==unset) result[vertex]=submesh.materialIndex;
            else if (result[vertex]!=submesh.materialIndex && !reported[vertex]) {
                reported[vertex]=true;++sharedMaterialVertices;
            }
        }
    }
    return result;
}

void apply_displacement(CookedPolygonAsset& asset,
                        std::span<const MaterialVertexDisplacementSettings> settings,
                        float distance,
                        bool collisionOnly,
                        PolygonDisplacementStats& stats) {
    std::uint64_t shared{};
    const auto materials=vertex_materials(asset,shared);
    stats.sharedMaterialVertices=std::max(stats.sharedMaterialVertices,shared);
    for (std::size_t index=0U;index<asset.vertices.size();++index) {
        const std::uint32_t materialIndex=materials[index];
        if (materialIndex>=settings.size()||materialIndex>=asset.materialBindings.size()) continue;
        const auto& setting=settings[materialIndex];
        const bool active=collisionOnly
            ? setting.policy==MaterialDisplacementPolicy::CollisionAffecting
            : setting.policy!=MaterialDisplacementPolicy::Disabled;
        if (!active || !(std::abs(setting.scaleMeters)>0.0F)) continue;
        const auto& binding=asset.materialBindings[materialIndex];
        if (!binding.height.texture) { ++stats.missingHeightBindings; continue; }
        PolygonVertex& vertex=asset.vertices[index];
        Float2 uv=binding.height.texcoord==1U?vertex.texcoord1:vertex.texcoord;
        uv=transform_texture_coordinates(uv,binding.mapping.baseTransform);
        const float height=sample_image_red(asset,binding.height,uv);
        const float displacement=(height-setting.referencePlane)*setting.scaleMeters*
                                 material_displacement_lod_fade(distance,setting);
        vertex.position=add3(vertex.position,scale3(normalize3(vertex.normal),displacement));
        stats.maximumAbsoluteDisplacement=std::max(stats.maximumAbsoluteDisplacement,std::abs(displacement));
        if (collisionOnly) ++stats.displacedCollisionVertices;
        else ++stats.displacedVisualVertices;
    }
    recompute_bounds(asset);
    asset.contentHash=polygon_asset_content_hash(asset);
}

} // namespace

bool validate_material_vertex_displacement_settings(
    const MaterialVertexDisplacementSettings& settings,
    GeometryKind geometry,
    std::string* error) noexcept {
    auto fail=[&](const char* message){if(error)*error=message;return false;};
    if (static_cast<unsigned>(settings.policy) >
        static_cast<unsigned>(MaterialDisplacementPolicy::CollisionAffecting))
        return fail("material displacement policy is invalid");
    if (!finite(settings.scaleMeters)||!finite(settings.referencePlane)||
        !finite(settings.fadeStartMeters)||!finite(settings.fadeEndMeters))
        return fail("material displacement settings contain a non-finite value");
    if (settings.referencePlane<0.0F||settings.referencePlane>1.0F||
        settings.fadeStartMeters<0.0F||settings.fadeEndMeters<settings.fadeStartMeters||
        settings.offlineSubdivisionLevels>4U)
        return fail("material displacement settings are outside supported bounds");
    if (geometry==GeometryKind::Voxel&&settings.policy==MaterialDisplacementPolicy::CollisionAffecting)
        return fail("voxel-derived displacement is visual-only; revoxelization is required for collision changes");
    if (settings.policy!=MaterialDisplacementPolicy::Disabled&&
        (!settings.affectDepth||!settings.affectShadows))
        return fail("geometric displacement must affect both depth and shadow geometry");
    return true;
}

float material_displacement_lod_fade(
    float cameraDistanceMeters,
    const MaterialVertexDisplacementSettings& settings) noexcept {
    if (settings.policy==MaterialDisplacementPolicy::Disabled) return 0.0F;
    if (cameraDistanceMeters<=settings.fadeStartMeters) return 1.0F;
    if (cameraDistanceMeters>=settings.fadeEndMeters) return 0.0F;
    const float range=std::max(1.0e-5F,settings.fadeEndMeters-settings.fadeStartMeters);
    return saturate(1.0F-(cameraDistanceMeters-settings.fadeStartMeters)/range);
}

std::optional<PolygonDisplacementResult> build_displaced_polygon_asset(
    const CookedPolygonAsset& source,
    std::span<const MaterialVertexDisplacementSettings> materialSettings,
    float cameraDistanceMeters,
    std::uint64_t maximumVertices,
    std::string* error) {
    const auto valid=validate_polygon_asset(source);
    if (!valid) { if(error)*error=valid.message; return std::nullopt; }
    if (!finite(cameraDistanceMeters)||cameraDistanceMeters<0.0F||maximumVertices<source.vertices.size()) {
        if(error)*error="invalid displacement build limits";
        return std::nullopt;
    }
    if (materialSettings.size()<source.materials.size()) {
        if(error)*error="displacement settings must cover every material";
        return std::nullopt;
    }
    std::uint32_t subdivisions{};
    bool collisionRequested=false;
    for (std::size_t i=0U;i<source.materials.size();++i) {
        std::string validation;
        if (!validate_material_vertex_displacement_settings(materialSettings[i],GeometryKind::Polygon,&validation)) {
            if(error)*error=validation;
            return std::nullopt;
        }
        if (materialSettings[i].policy!=MaterialDisplacementPolicy::Disabled)
            subdivisions=std::max(subdivisions,materialSettings[i].offlineSubdivisionLevels);
        collisionRequested=collisionRequested||
            materialSettings[i].policy==MaterialDisplacementPolicy::CollisionAffecting;
    }

    PolygonDisplacementResult result;
    result.visualAsset=source;
    result.stats.sourceVertices=source.vertices.size();
    result.stats.sourceTriangles=source.indices.size()/3U;
    for (std::uint32_t level=0U;level<subdivisions;++level)
        if (!subdivide_once(result.visualAsset,maximumVertices,error)) return std::nullopt;
    if (collisionRequested) result.collisionAsset=result.visualAsset;

    apply_displacement(result.visualAsset,materialSettings,cameraDistanceMeters,false,result.stats);
    if (result.collisionAsset)
        apply_displacement(*result.collisionAsset,materialSettings,cameraDistanceMeters,true,result.stats);
    result.stats.outputVertices=result.visualAsset.vertices.size();
    result.stats.outputTriangles=result.visualAsset.indices.size()/3U;
    return result;
}

} // namespace dve
