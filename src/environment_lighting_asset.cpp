#include "dve/environment_lighting.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>

namespace dve {
namespace {
constexpr std::array<char, 8> kMagic{'D','V','E','I','B','L','1','\0'};
constexpr std::uint16_t kMajor = 1U;
constexpr std::uint16_t kMinor = 0U;
constexpr std::uint32_t kMaximumResolution = 8192U;
constexpr std::uint64_t kMaximumTexels = 512ULL * 1024ULL * 1024ULL;

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

class Writer {
public:
    void u8(std::uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
    void u16(std::uint16_t value) { u8(static_cast<std::uint8_t>(value)); u8(static_cast<std::uint8_t>(value >> 8U)); }
    void u32(std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift)); }
    void u64(std::uint64_t value) { for (unsigned shift = 0; shift < 64; shift += 8) u8(static_cast<std::uint8_t>(value >> shift)); }
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void bytes(const void* source, std::size_t count) {
        const auto* first = static_cast<const std::byte*>(source);
        data.insert(data.end(), first, first + static_cast<std::ptrdiff_t>(count));
    }
    void text(std::string_view value) {
        if (value.size() > (1U << 24U)) throw std::runtime_error("DVEIBL string exceeds limit");
        u32(static_cast<std::uint32_t>(value.size())); bytes(value.data(), value.size());
    }
    std::vector<std::byte> data;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) throw std::runtime_error("truncated DVEIBL file");
    }
    std::uint8_t u8() { require(1); return std::to_integer<std::uint8_t>(bytes_[offset_++]); }
    std::uint16_t u16() { const auto a=u8(), b=u8(); return static_cast<std::uint16_t>(a | (b << 8U)); }
    std::uint32_t u32() { std::uint32_t value{}; for (unsigned shift=0; shift<32; shift+=8) value |= static_cast<std::uint32_t>(u8()) << shift; return value; }
    std::uint64_t u64() { std::uint64_t value{}; for (unsigned shift=0; shift<64; shift+=8) value |= static_cast<std::uint64_t>(u8()) << shift; return value; }
    float f32() { return std::bit_cast<float>(u32()); }
    std::string text() { const auto count=u32(); require(count); std::string out(count, '\0'); if(count) std::memcpy(out.data(), bytes_.data()+offset_, count); offset_ += count; return out; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

void hash_bytes(std::uint64_t& hash, const void* bytes, std::size_t count) noexcept {
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i=0; i<count; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
}
template<class T> void hash_value(std::uint64_t& hash, const T& value) noexcept { hash_bytes(hash, &value, sizeof(value)); }
void hash_cube(std::uint64_t& hash, const EnvironmentCube& cube) noexcept {
    hash_value(hash, cube.resolution);
    for (const auto pixel : cube.texels) { hash_value(hash, pixel.x); hash_value(hash, pixel.y); hash_value(hash, pixel.z); }
}
std::uint64_t calculate_hash(const EnvironmentLightingAsset& asset) noexcept {
    std::uint64_t hash=1469598103934665603ULL;
    hash_bytes(hash, asset.name.data(), asset.name.size());
    hash_value(hash, asset.rotationRadians); hash_value(hash, asset.intensity); hash_value(hash, asset.skyboxExposure);
    hash_cube(hash, asset.sourceRadiance); hash_cube(hash, asset.baked.diffuseIrradiance);
    const auto levels=static_cast<std::uint32_t>(asset.baked.specularPrefilter.levels.size()); hash_value(hash, levels);
    for(const auto& level:asset.baked.specularPrefilter.levels) hash_cube(hash,level);
    hash_value(hash, asset.baked.brdfResolution);
    for(const auto sample:asset.baked.brdfLut){hash_value(hash,sample.scale);hash_value(hash,sample.bias);}
    return hash;
}
void write_cube(Writer& writer, const EnvironmentCube& cube) {
    writer.u32(cube.resolution); writer.u64(static_cast<std::uint64_t>(cube.texels.size()));
    for(const auto pixel:cube.texels){writer.f32(pixel.x);writer.f32(pixel.y);writer.f32(pixel.z);}
}
EnvironmentCube read_cube(Reader& reader) {
    EnvironmentCube cube; cube.resolution=reader.u32(); const auto count=reader.u64();
    if(cube.resolution==0 || cube.resolution>kMaximumResolution || count>kMaximumTexels || count!=6ULL*cube.resolution*cube.resolution)
        throw std::runtime_error("invalid DVEIBL cube dimensions");
    cube.texels.resize(static_cast<std::size_t>(count));
    for(auto& pixel:cube.texels) pixel={reader.f32(),reader.f32(),reader.f32()};
    return cube;
}
Float3 add3(Float3 a,Float3 b) noexcept{return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 mul3(Float3 a,float s) noexcept{return{a.x*s,a.y*s,a.z*s};}
Float3 cross3(Float3 a,Float3 b) noexcept{return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float dot3(Float3 a,Float3 b) noexcept{return a.x*b.x+a.y*b.y+a.z*b.z;}
Float3 normalize3(Float3 a,Float3 fallback) noexcept{const float l=std::sqrt(std::max(0.0F,dot3(a,a)));return l>1.0e-8F?mul3(a,1.0F/l):fallback;}
}

bool EnvironmentLightingAsset::validate(std::string* error) const noexcept {
    auto fail=[&](const char* message){set_error(error,message);return false;};
    if(name.empty() || name.size()>(1U<<20U)) return fail("environment-lighting name is empty or too long");
    if(!std::isfinite(rotationRadians)||!std::isfinite(intensity)||intensity<0.0F||!std::isfinite(skyboxExposure)||skyboxExposure<-32.0F||skyboxExposure>32.0F)
        return fail("environment-lighting exposure, intensity, or rotation is invalid");
    if(!sourceRadiance.validate(error)||!baked.validate(error)) return false;
    if(contentHash!=0U && contentHash!=calculate_hash(*this)) return fail("environment-lighting content hash mismatch");
    return true;
}
void EnvironmentLightingAsset::recompute_content_hash() noexcept { contentHash=calculate_hash(*this); }

EnvironmentLightingAsset make_default_environment_lighting_asset(const IblBakeSettings& settings) {
    std::string error; if(!settings.validate(&error)) throw std::invalid_argument(error);
    EnvironmentLightingAsset asset; asset.name="Default Studio Environment";
    asset.sourceRadiance={16U,std::vector<Float3>(6ULL*16U*16U)};
    for(std::uint32_t face=0;face<6U;++face) for(std::uint32_t y=0;y<16U;++y) for(std::uint32_t x=0;x<16U;++x){
        const Float3 d=cube_lookup_to_direction(static_cast<CubeFace>(face),{(x+0.5F)/16.0F,(y+0.5F)/16.0F});
        const float sky=std::clamp(0.5F+0.5F*d.y,0.0F,1.0F); const Float3 ground{0.035F,0.03F,0.025F}, zenith{0.35F,0.50F,0.82F};
        asset.sourceRadiance.texels[asset.sourceRadiance.face_offset(static_cast<CubeFace>(face))+static_cast<std::size_t>(y)*16U+x]={ground.x+(zenith.x-ground.x)*sky,ground.y+(zenith.y-ground.y)*sky,ground.z+(zenith.z-ground.z)*sky};
    }
    asset.baked=bake_image_based_lighting(asset.sourceRadiance,settings); asset.recompute_content_hash(); return asset;
}

bool write_dveibl(const std::filesystem::path& path,const EnvironmentLightingAsset& input,std::string* error) {
    try {
        EnvironmentLightingAsset asset=input; if(asset.contentHash==0U)asset.recompute_content_hash(); std::string validation;if(!asset.validate(&validation))throw std::runtime_error(validation);
        Writer writer;writer.bytes(kMagic.data(),kMagic.size());writer.u16(kMajor);writer.u16(kMinor);writer.u64(asset.contentHash);writer.f32(asset.rotationRadians);writer.f32(asset.intensity);writer.f32(asset.skyboxExposure);writer.f32(0.0F);writer.text(asset.name);
        write_cube(writer,asset.sourceRadiance);write_cube(writer,asset.baked.diffuseIrradiance);writer.u32(static_cast<std::uint32_t>(asset.baked.specularPrefilter.levels.size()));for(const auto& level:asset.baked.specularPrefilter.levels)write_cube(writer,level);writer.u32(asset.baked.brdfResolution);writer.u64(static_cast<std::uint64_t>(asset.baked.brdfLut.size()));for(const auto sample:asset.baked.brdfLut){writer.f32(sample.scale);writer.f32(sample.bias);}
        std::filesystem::create_directories(path.parent_path().empty()?std::filesystem::path{"."}:path.parent_path());const auto temporary=path.string()+".tmp";std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);if(!stream)throw std::runtime_error("failed to open temporary DVEIBL file");stream.write(reinterpret_cast<const char*>(writer.data.data()),static_cast<std::streamsize>(writer.data.size()));stream.close();if(!stream)throw std::runtime_error("failed to write DVEIBL file");std::error_code ec;std::filesystem::rename(temporary,path,ec);if(ec){std::filesystem::remove(path,ec);ec.clear();std::filesystem::rename(temporary,path,ec);}if(ec)throw std::runtime_error("failed to commit DVEIBL file: "+ec.message());return true;
    } catch(const std::exception& exception){set_error(error,exception.what());return false;}
}
EnvironmentLightingReadResult read_dveibl(const std::filesystem::path& path,std::uint64_t maximumBytes) {
    EnvironmentLightingReadResult result;try{std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("failed to open DVEIBL file");stream.seekg(0,std::ios::end);const auto end=stream.tellg();if(end<0||static_cast<std::uint64_t>(end)>maximumBytes)throw std::runtime_error("DVEIBL file exceeds size limit");stream.seekg(0);std::vector<std::byte> bytes(static_cast<std::size_t>(end));if(!bytes.empty())stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!stream&&!bytes.empty())throw std::runtime_error("failed to read DVEIBL file");Reader reader(bytes);for(const char expected:kMagic)if(reader.u8()!=static_cast<std::uint8_t>(expected))throw std::runtime_error("DVEIBL magic mismatch");const auto major=reader.u16(),minor=reader.u16();if(major!=kMajor||minor>kMinor)throw std::runtime_error("unsupported DVEIBL version");EnvironmentLightingAsset asset;asset.contentHash=reader.u64();asset.rotationRadians=reader.f32();asset.intensity=reader.f32();asset.skyboxExposure=reader.f32();(void)reader.f32();asset.name=reader.text();asset.sourceRadiance=read_cube(reader);asset.baked.diffuseIrradiance=read_cube(reader);const auto levels=reader.u32();if(levels==0U||levels>32U)throw std::runtime_error("invalid DVEIBL mip count");for(std::uint32_t i=0;i<levels;++i)asset.baked.specularPrefilter.levels.push_back(read_cube(reader));asset.baked.brdfResolution=reader.u32();const auto count=reader.u64();if(asset.baked.brdfResolution==0U||asset.baked.brdfResolution>kMaximumResolution||count!=static_cast<std::uint64_t>(asset.baked.brdfResolution)*asset.baked.brdfResolution||count>kMaximumTexels)throw std::runtime_error("invalid DVEIBL BRDF LUT dimensions");asset.baked.brdfLut.resize(static_cast<std::size_t>(count));for(auto& sample:asset.baked.brdfLut)sample={reader.f32(),reader.f32()};if(!reader.at_end())throw std::runtime_error("DVEIBL has trailing data");std::string validation;if(!asset.validate(&validation))throw std::runtime_error(validation);result.asset=std::move(asset);return result;}catch(const std::exception& exception){result.error=exception.what();return result;}}

bool EnvironmentSkyboxSettings::validate(std::string* error) const noexcept {auto fail=[&](const char* message){set_error(error,message);return false;};if(width==0U||height==0U||width>16384U||height>16384U)return fail("skybox dimensions are invalid");if(!std::isfinite(rotationRadians)||!std::isfinite(intensity)||intensity<0.0F||!std::isfinite(exposure)||exposure<-32.0F||exposure>32.0F)return fail("skybox rotation, intensity, or exposure is invalid");return true;}
Float3 rotate_environment_direction(Float3 direction,float yaw) noexcept {const float c=std::cos(yaw),s=std::sin(yaw);return normalize3({c*direction.x+s*direction.z,direction.y,-s*direction.x+c*direction.z},{0,0,1});}
Float3 sample_environment_sky(const EnvironmentCube& cube,Float3 direction,float rotation,float intensity,float exposure) noexcept {const float scale=std::max(0.0F,intensity)*std::exp2(std::clamp(exposure,-32.0F,32.0F));const auto sample=sample_environment_cube(cube,rotate_environment_direction(direction,rotation));return{std::max(0.0F,sample.x*scale),std::max(0.0F,sample.y*scale),std::max(0.0F,sample.z*scale)};}
std::vector<Float4> render_environment_skybox(const EnvironmentCube& cube,const camera::CameraPose& camera,const EnvironmentSkyboxSettings& settings){std::string error;if(!cube.validate(&error)||!camera.lens.validate(&error)||!settings.validate(&error))throw std::invalid_argument(error);const Float3 forward=normalize3({camera.target.x-camera.position.x,camera.target.y-camera.position.y,camera.target.z-camera.position.z},{0,0,-1});const Float3 right=normalize3(cross3(forward,camera.worldUp),{1,0,0});const Float3 up=normalize3(cross3(right,forward),{0,1,0});const float tanHalf=std::tan(camera.lens.effective_vertical_field_of_view_radians()*0.5F);std::vector<Float4> image(static_cast<std::size_t>(settings.width)*settings.height);for(std::uint32_t y=0;y<settings.height;++y)for(std::uint32_t x=0;x<settings.width;++x){const float nx=(2.0F*(static_cast<float>(x)+0.5F)/settings.width-1.0F)*camera.lens.aspectRatio*tanHalf;const float ny=(1.0F-2.0F*(static_cast<float>(y)+0.5F)/settings.height)*tanHalf;const Float3 direction=normalize3(add3(forward,add3(mul3(right,nx),mul3(up,ny))),forward);const Float3 radiance=sample_environment_sky(cube,direction,settings.rotationRadians,settings.intensity,settings.exposure);image[static_cast<std::size_t>(y)*settings.width+x]={radiance.x,radiance.y,radiance.z,1.0F};}return image;}
} // namespace dve
