#include "dve/heightmap_terrain.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>

#include <png.h>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-6F;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] float dot(Float3 a,Float3 b) noexcept{return a.x*b.x+a.y*b.y+a.z*b.z;}
[[nodiscard]] Float3 normalize(Float3 v) noexcept {
    const float l=std::sqrt(dot(v,v));return l>kEpsilon?Float3{v.x/l,v.y/l,v.z/l}:Float3{0.0F,1.0F,0.0F};
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path,std::string* error){
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in){fail(error,"could not open heightmap");return {};}
    const auto end=in.tellg();if(end<0){fail(error,"could not measure heightmap");return {};}
    in.seekg(0,std::ios::beg);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if(!bytes.empty()&&!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))){fail(error,"could not read heightmap");return {};}
    return bytes;
}

[[nodiscard]] std::optional<HeightmapImage> load_png(const std::filesystem::path& path,
                                                      const HeightmapImportSettings& settings,
                                                      std::string* error){
    FILE* file=std::fopen(path.string().c_str(),"rb");if(!file){fail(error,"could not open PNG heightmap");return std::nullopt;}
    png_structp png=png_create_read_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
    png_infop info=png?png_create_info_struct(png):nullptr;
    if(!png||!info){if(png)png_destroy_read_struct(&png,nullptr,nullptr);std::fclose(file);fail(error,"could not initialize PNG decoder");return std::nullopt;}
    if(setjmp(png_jmpbuf(png))!=0){png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);fail(error,"PNG heightmap decode failed");return std::nullopt;}
    png_init_io(png,file);png_read_info(png,info);
    const png_uint_32 width=png_get_image_width(png,info),height=png_get_image_height(png,info);
    int bitDepth=png_get_bit_depth(png,info),colorType=png_get_color_type(png,info);
    if(width<2U||height<2U||width>settings.maximumDimension||height>settings.maximumDimension||
       static_cast<std::uint64_t>(width)*height>settings.maximumSamples){png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);fail(error,"PNG heightmap dimensions exceed configured limits");return std::nullopt;}
    if(colorType==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);
    if(colorType==PNG_COLOR_TYPE_RGB||colorType==PNG_COLOR_TYPE_RGB_ALPHA||colorType==PNG_COLOR_TYPE_PALETTE)
        png_set_rgb_to_gray_fixed(png,1,-1,-1);
    if(colorType==PNG_COLOR_TYPE_GRAY_ALPHA||colorType==PNG_COLOR_TYPE_RGB_ALPHA)png_set_strip_alpha(png);
    if(bitDepth<8)png_set_expand_gray_1_2_4_to_8(png);
    if(bitDepth==16&&std::endian::native==std::endian::little)png_set_swap(png);
    png_read_update_info(png,info);bitDepth=png_get_bit_depth(png,info);
    if(png_get_channels(png,info)!=1||!(bitDepth==8||bitDepth==16)){png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);fail(error,"PNG heightmap must decode to 8-bit or 16-bit grayscale");return std::nullopt;}
    const png_size_t rowBytes=png_get_rowbytes(png,info);std::vector<std::uint8_t> storage(rowBytes*height);std::vector<png_bytep> rows(height);
    for(png_uint_32 y=0;y<height;++y)rows[y]=storage.data()+static_cast<std::size_t>(y)*rowBytes;
    png_read_image(png,rows.data());png_read_end(png,nullptr);png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);
    HeightmapImage out;out.width=width;out.height=height;out.sourceBitDepth=static_cast<std::uint16_t>(bitDepth);out.sourceFormat="PNG";out.samples.resize(static_cast<std::size_t>(width)*height);
    for(std::uint32_t y=0;y<out.height;++y)for(std::uint32_t x=0;x<out.width;++x){
        float value{};if(bitDepth==8)value=static_cast<float>(storage[static_cast<std::size_t>(y)*rowBytes+x])/255.0F;
        else{std::uint16_t sample{};std::memcpy(&sample,storage.data()+static_cast<std::size_t>(y)*rowBytes+static_cast<std::size_t>(x)*2U,2U);value=static_cast<float>(sample)/65535.0F;}
        out.samples[static_cast<std::size_t>(y)*out.width+x]=value;
    }
    return out;
}

[[nodiscard]] bool next_pgm_token(std::span<const std::uint8_t> bytes,std::size_t& offset,std::string& token){
    token.clear();
    while(offset<bytes.size()){
        const char c=static_cast<char>(bytes[offset]);
        if(c=='#'){while(offset<bytes.size()&&bytes[offset]!='\n')++offset;continue;}
        if(std::isspace(static_cast<unsigned char>(c))!=0){++offset;continue;}break;
    }
    while(offset<bytes.size()){
        const char c=static_cast<char>(bytes[offset]);if(c=='#'||std::isspace(static_cast<unsigned char>(c))!=0)break;token.push_back(c);++offset;
    }
    return !token.empty();
}

[[nodiscard]] std::optional<HeightmapImage> load_pgm(const std::filesystem::path& path,
                                                      const HeightmapImportSettings& settings,
                                                      std::string* error){
    auto bytes=read_file(path,error);if(bytes.empty())return std::nullopt;std::size_t offset=0U;std::string magic,w,h,maxv;
    if(!next_pgm_token(bytes,offset,magic)||!next_pgm_token(bytes,offset,w)||!next_pgm_token(bytes,offset,h)||!next_pgm_token(bytes,offset,maxv)||
       (magic!="P2"&&magic!="P5")){fail(error,"unsupported or corrupt PGM heightmap");return std::nullopt;}
    std::uint64_t width{},height{},maximum{};try{width=std::stoull(w);height=std::stoull(h);maximum=std::stoull(maxv);}catch(...){fail(error,"invalid PGM header");return std::nullopt;}
    if(width<2U||height<2U||width>settings.maximumDimension||height>settings.maximumDimension||width*height>settings.maximumSamples||maximum==0U||maximum>65535U){fail(error,"PGM dimensions or maximum value are invalid");return std::nullopt;}
    HeightmapImage out;out.width=static_cast<std::uint32_t>(width);out.height=static_cast<std::uint32_t>(height);out.sourceBitDepth=maximum>255U?16U:8U;out.sourceFormat="PGM";out.samples.reserve(static_cast<std::size_t>(width*height));
    if(magic=="P2"){
        std::string token;for(std::uint64_t i=0;i<width*height;++i){if(!next_pgm_token(bytes,offset,token)){fail(error,"truncated ASCII PGM heightmap");return std::nullopt;}std::uint64_t v{};try{v=std::stoull(token);}catch(...){fail(error,"invalid ASCII PGM sample");return std::nullopt;}if(v>maximum){fail(error,"PGM sample exceeds maximum value");return std::nullopt;}out.samples.push_back(static_cast<float>(v)/static_cast<float>(maximum));}
    }else{
        if(offset>=bytes.size()||std::isspace(bytes[offset])==0){fail(error,"binary PGM header lacks its sample separator");return std::nullopt;}
        ++offset;
        const std::size_t bytesPer=maximum>255U?2U:1U;
        if(bytes.size()-offset<static_cast<std::size_t>(width*height)*bytesPer){fail(error,"truncated binary PGM heightmap");return std::nullopt;}
        for(std::uint64_t i=0;i<width*height;++i){std::uint16_t v{};if(bytesPer==1U)v=bytes[offset++];else{v=static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset])<<8U)|bytes[offset+1U]);offset+=2U;}out.samples.push_back(static_cast<float>(v)/static_cast<float>(maximum));}
    }
    return out;
}

[[nodiscard]] std::optional<HeightmapImage> load_raw16(const std::filesystem::path& path,
                                                        const HeightmapImportSettings& settings,bool little,
                                                        std::string* error){
    if(settings.rawWidth<2U||settings.rawHeight<2U){fail(error,"RAW16 import requires rawWidth and rawHeight of at least 2");return std::nullopt;}
    const std::uint64_t count=static_cast<std::uint64_t>(settings.rawWidth)*settings.rawHeight;
    if(settings.rawWidth>settings.maximumDimension||settings.rawHeight>settings.maximumDimension||count>settings.maximumSamples){fail(error,"RAW16 dimensions exceed configured limits");return std::nullopt;}
    auto bytes=read_file(path,error);if(bytes.size()!=count*2ULL){fail(error,"RAW16 byte count does not match rawWidth*rawHeight*2");return std::nullopt;}
    HeightmapImage out;out.width=settings.rawWidth;out.height=settings.rawHeight;out.sourceBitDepth=16U;out.sourceFormat=little?"RAW16LE":"RAW16BE";out.samples.resize(static_cast<std::size_t>(count));
    for(std::size_t i=0;i<out.samples.size();++i){const std::uint8_t a=bytes[i*2U],b=bytes[i*2U+1U];const std::uint16_t value=little?static_cast<std::uint16_t>(a|(static_cast<std::uint16_t>(b)<<8U)):static_cast<std::uint16_t>((static_cast<std::uint16_t>(a)<<8U)|b);out.samples[i]=static_cast<float>(value)/65535.0F;}
    return out;
}

void transform_samples(HeightmapImage& image,const HeightmapImportSettings& settings){
    if(settings.flipVertical)for(std::uint32_t y=0;y<image.height/2U;++y){const std::uint32_t other=image.height-1U-y;for(std::uint32_t x=0;x<image.width;++x)std::swap(image.samples[static_cast<std::size_t>(y)*image.width+x],image.samples[static_cast<std::size_t>(other)*image.width+x]);}
    if(settings.invert)for(float& v:image.samples)v=1.0F-v;
}

[[nodiscard]] Float3 terrain_position(const HeightmapImage& image,const HeightmapTerrainSettings& settings,
                                      std::uint32_t x,std::uint32_t y) noexcept {
    const float h=settings.originMeters.y+settings.heightOffsetMeters+image.at(x,y)*settings.heightScaleMeters;
    return {settings.originMeters.x+static_cast<float>(x)*settings.spacingXMeters,h,
            settings.originMeters.z+static_cast<float>(y)*settings.spacingZMeters};
}

[[nodiscard]] Float3 terrain_normal(const HeightmapImage& image,const HeightmapTerrainSettings& settings,
                                    std::uint32_t x,std::uint32_t y) noexcept {
    const std::uint32_t xl=x==0U?0U:x-1U,xr=std::min(x+1U,image.width-1U),yu=y==0U?0U:y-1U,yd=std::min(y+1U,image.height-1U);
    const float hL=image.at(xl,y)*settings.heightScaleMeters,hR=image.at(xr,y)*settings.heightScaleMeters;
    const float hU=image.at(x,yu)*settings.heightScaleMeters,hD=image.at(x,yd)*settings.heightScaleMeters;
    const float dx=std::max(settings.spacingXMeters*static_cast<float>(xr-xl),kEpsilon),dz=std::max(settings.spacingZMeters*static_cast<float>(yd-yu),kEpsilon);
    return normalize({-(hR-hL)/dx,1.0F,-(hD-hU)/dz});
}

void add_quad(ImportedMesh& mesh,std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t d,std::uint32_t& source,bool reverse=false){
    if(!reverse){mesh.triangles.push_back({{a,b,c},0U,source++});mesh.triangles.push_back({{a,c,d},0U,source++});}
    else{mesh.triangles.push_back({{a,c,b},0U,source++});mesh.triangles.push_back({{a,d,c},0U,source++});}
}

[[nodiscard]] ImportedScene make_scene(const HeightmapImage& image,const HeightmapTerrainSettings& settings,bool closeSolid){
    ImportedScene scene;scene.name="HeightmapTerrain";ImportedMaterial material;material.name=settings.materialName;material.baseColorFactor=settings.baseColor;material.metallicFactor=0.0F;material.roughnessFactor=settings.roughness;scene.materials.push_back(material);
    ImportedMesh mesh;mesh.name="HeightmapTerrain";const std::size_t topCount=static_cast<std::size_t>(image.width)*image.height;mesh.vertices.reserve(closeSolid?topCount*2U:topCount);
    for(std::uint32_t y=0;y<image.height;++y)for(std::uint32_t x=0;x<image.width;++x){ImportedVertex v;v.position=terrain_position(image,settings,x,y);v.normal=terrain_normal(image,settings,x,y);v.texcoord={static_cast<float>(x)/static_cast<float>(image.width-1U),static_cast<float>(y)/static_cast<float>(image.height-1U)};mesh.vertices.push_back(v);}
    std::uint32_t source=0U;auto index=[&](std::uint32_t x,std::uint32_t y){return y*image.width+x;};
    for(std::uint32_t y=0;y+1U<image.height;++y)for(std::uint32_t x=0;x+1U<image.width;++x){const auto a=index(x,y),b=index(x+1U,y),c=index(x+1U,y+1U),d=index(x,y+1U);const bool alternate=settings.alternateTriangleDiagonals&&((x+y)&1U)!=0U;if(!alternate){mesh.triangles.push_back({{a,c,b},0U,source++});mesh.triangles.push_back({{a,d,c},0U,source++});}else{mesh.triangles.push_back({{a,d,b},0U,source++});mesh.triangles.push_back({{b,d,c},0U,source++});}}
    if(closeSolid){
        const std::uint32_t bottomBase=static_cast<std::uint32_t>(mesh.vertices.size());const float baseY=settings.originMeters.y+settings.solidBaseHeightMeters;
        for(std::uint32_t y=0;y<image.height;++y)for(std::uint32_t x=0;x<image.width;++x){ImportedVertex v;v.position={settings.originMeters.x+static_cast<float>(x)*settings.spacingXMeters,baseY,settings.originMeters.z+static_cast<float>(y)*settings.spacingZMeters};v.normal={0.0F,-1.0F,0.0F};v.texcoord={static_cast<float>(x)/static_cast<float>(image.width-1U),static_cast<float>(y)/static_cast<float>(image.height-1U)};mesh.vertices.push_back(v);}
        auto bottom=[&](std::uint32_t x,std::uint32_t y){return bottomBase+y*image.width+x;};
        for(std::uint32_t y=0;y+1U<image.height;++y)for(std::uint32_t x=0;x+1U<image.width;++x)add_quad(mesh,bottom(x,y),bottom(x+1U,y),bottom(x+1U,y+1U),bottom(x,y+1U),source,true);
        for(std::uint32_t x=0;x+1U<image.width;++x){add_quad(mesh,index(x,0U),index(x+1U,0U),bottom(x+1U,0U),bottom(x,0U),source,true);const auto y=image.height-1U;add_quad(mesh,index(x,y),bottom(x,y),bottom(x+1U,y),index(x+1U,y),source,true);}
        for(std::uint32_t y=0;y+1U<image.height;++y){add_quad(mesh,index(0U,y),bottom(0U,y),bottom(0U,y+1U),index(0U,y+1U),source,true);const auto x=image.width-1U;add_quad(mesh,index(x,y),index(x,y+1U),bottom(x,y+1U),bottom(x,y),source,true);}
    }else if(settings.polygonSkirtDepthMeters>0.0F){
        // Skirts hide cracks at streamed terrain boundaries without changing the top surface used by navigation.
        const float skirt=settings.polygonSkirtDepthMeters;auto add_skirt_edge=[&](std::uint32_t a,std::uint32_t b){const auto va=mesh.vertices[a],vb=mesh.vertices[b];const std::uint32_t base=static_cast<std::uint32_t>(mesh.vertices.size());auto sa=va,sb=vb;sa.position.y-=skirt;sb.position.y-=skirt;mesh.vertices.push_back(va);mesh.vertices.push_back(vb);mesh.vertices.push_back(sb);mesh.vertices.push_back(sa);add_quad(mesh,base,base+1U,base+2U,base+3U,source,false);};
        for(std::uint32_t x=0;x+1U<image.width;++x){add_skirt_edge(index(x,0U),index(x+1U,0U));const auto y=image.height-1U;add_skirt_edge(index(x+1U,y),index(x,y));}
        for(std::uint32_t y=0;y+1U<image.height;++y){add_skirt_edge(index(0U,y+1U),index(0U,y));const auto x=image.width-1U;add_skirt_edge(index(x,y),index(x,y+1U));}
    }
    scene.meshes.push_back(std::move(mesh));ImportedNode node;node.name="HeightmapTerrain";node.mesh=0U;scene.nodes.push_back(node);scene.roots.push_back(0U);return scene;
}

} // namespace

bool HeightmapImage::validate(std::string* error) const noexcept {
    if(width<2U||height<2U)return fail(error,"heightmap dimensions must be at least 2x2");
    if(samples.size()!=static_cast<std::size_t>(width)*height)return fail(error,"heightmap sample count does not match dimensions");
    for(float v:samples)if(!std::isfinite(v)||v<0.0F||v>1.0F)return fail(error,"heightmap samples must be finite and normalized to [0,1]");
    return true;
}
float HeightmapImage::at(std::uint32_t x,std::uint32_t y) const noexcept {return samples[static_cast<std::size_t>(std::min(y,height-1U))*width+std::min(x,width-1U)];}
float HeightmapImage::sample_bilinear(float x,float y) const noexcept {
    x=std::clamp(x,0.0F,static_cast<float>(width-1U));y=std::clamp(y,0.0F,static_cast<float>(height-1U));const auto x0=static_cast<std::uint32_t>(std::floor(x)),y0=static_cast<std::uint32_t>(std::floor(y));const auto x1=std::min(x0+1U,width-1U),y1=std::min(y0+1U,height-1U);const float tx=x-static_cast<float>(x0),ty=y-static_cast<float>(y0);return std::lerp(std::lerp(at(x0,y0),at(x1,y0),tx),std::lerp(at(x0,y1),at(x1,y1),tx),ty);
}

bool HeightmapTerrainSettings::validate(std::string* error) const noexcept {
    auto bad=[&](const char* m){if(error)*error=m;return false;};
    if(!std::isfinite(spacingXMeters)||spacingXMeters<=0.0F||!std::isfinite(spacingZMeters)||spacingZMeters<=0.0F)return bad("heightmap spacing must be finite and positive");
    if(!std::isfinite(heightScaleMeters)||heightScaleMeters<=0.0F||!std::isfinite(heightOffsetMeters)||!std::isfinite(solidBaseHeightMeters)||!std::isfinite(polygonSkirtDepthMeters)||polygonSkirtDepthMeters<0.0F)return bad("heightScaleMeters must be positive and the remaining height/skirt settings must be finite");
    if(!std::isfinite(voxelSizeMeters)||voxelSizeMeters<=0.0F)return bad("voxelSizeMeters must be finite and positive");
    if(maximumWorkingVoxels==0U)return bad("maximumWorkingVoxels must be positive");
    if(maximumTerrainVertices<4U||maximumTerrainTriangles<2U)return bad("terrain geometry limits are too small");
    if(materialName.empty())return bad("materialName must not be empty");
    if(!navigation.validate(error))return false;
    if(!generatePolygonAsset&&!generateVoxelAsset&&!generateNavigationMesh)return bad("at least one terrain output must be enabled");
    return true;
}

bool HeightmapTerrainBuildResult::success() const noexcept {return std::none_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.severity==HeightmapTerrainDiagnostic::Severity::Error;});}

std::optional<HeightmapImage> import_heightmap(const std::filesystem::path& path,const HeightmapImportSettings& settings,std::string* error){
    HeightmapFormat format=settings.format;if(format==HeightmapFormat::Automatic){std::string ext=path.extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(ext==".png")format=HeightmapFormat::Png;else if(ext==".pgm")format=HeightmapFormat::Pgm;else if(ext==".r16"||ext==".raw")format=HeightmapFormat::Raw16LittleEndian;else{fail(error,"could not infer heightmap format; use PNG, PGM, or specify RAW16");return std::nullopt;}}
    std::optional<HeightmapImage> image;switch(format){case HeightmapFormat::Png:image=load_png(path,settings,error);break;case HeightmapFormat::Pgm:image=load_pgm(path,settings,error);break;case HeightmapFormat::Raw16LittleEndian:image=load_raw16(path,settings,true,error);break;case HeightmapFormat::Raw16BigEndian:image=load_raw16(path,settings,false,error);break;case HeightmapFormat::Automatic:break;}
    if(!image)return std::nullopt;
    transform_samples(*image,settings);
    std::string validation;
    if(!image->validate(&validation)){fail(error,validation);return std::nullopt;}
    return image;
}

ImportedScene build_heightmap_imported_scene(const HeightmapImage& image,const HeightmapTerrainSettings& settings,bool closeSolidVolume){return make_scene(image,settings,closeSolidVolume);}

std::vector<NavigationTriangle> heightmap_navigation_triangles(const HeightmapImage& image,const HeightmapTerrainSettings& settings){
    std::vector<NavigationTriangle> out;out.reserve(static_cast<std::size_t>(image.width-1U)*(image.height-1U)*2U);auto pos=[&](std::uint32_t x,std::uint32_t y){return terrain_position(image,settings,x,y);};std::uint64_t id=0U;
    for(std::uint32_t y=0;y+1U<image.height;++y)for(std::uint32_t x=0;x+1U<image.width;++x){const auto a=pos(x,y),b=pos(x+1U,y),c=pos(x+1U,y+1U),d=pos(x,y+1U);const bool alternate=settings.alternateTriangleDiagonals&&((x+y)&1U)!=0U;if(!alternate){out.push_back({{a,c,b},1U,0xFFFFU,id++});out.push_back({{a,d,c},1U,0xFFFFU,id++});}else{out.push_back({{a,d,b},1U,0xFFFFU,id++});out.push_back({{b,d,c},1U,0xFFFFU,id++});}}
    return out;
}

HeightmapTerrainBuildResult cook_heightmap_terrain(const std::filesystem::path& path,const HeightmapImportSettings& importSettings,const HeightmapTerrainSettings& terrainSettings){
    HeightmapTerrainBuildResult out;std::string error;if(!terrainSettings.validate(&error)){out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"SETTINGS",error});return out;}
    auto image=import_heightmap(path,importSettings,&error);if(!image){out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"IMPORT",error});return out;}out.heightmap=*image;
    const std::uint64_t vertexCount=static_cast<std::uint64_t>(image->width)*image->height;
    const std::uint64_t triangleCount=2ULL*static_cast<std::uint64_t>(image->width-1U)*(image->height-1U);
    if(vertexCount>terrainSettings.maximumTerrainVertices||triangleCount>terrainSettings.maximumTerrainTriangles){
        out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"TERRAIN_LIMIT","heightmap exceeds single-asset terrain limits; tile the source or raise the explicit limits"});return out;
    }
    const float minimumSample=*std::min_element(image->samples.begin(),image->samples.end());
    const float minimumSurfaceY=terrainSettings.originMeters.y+terrainSettings.heightOffsetMeters+minimumSample*terrainSettings.heightScaleMeters;
    const float baseY=terrainSettings.originMeters.y+terrainSettings.solidBaseHeightMeters;
    if(terrainSettings.generateVoxelAsset&&baseY>=minimumSurfaceY-kEpsilon){
        out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"SOLID_BASE","solidBaseHeightMeters must place the closed terrain base below the lowest surface sample"});return out;
    }
    if(terrainSettings.generateNavigationMesh&&triangleCount>terrainSettings.navigation.maximumPolygons){
        out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"NAVIGATION_LIMIT","heightmap walkable triangle count exceeds navigation.maximumPolygons"});return out;
    }
    if(terrainSettings.generatePolygonAsset){auto scene=make_scene(*image,terrainSettings,false);PolygonCookOptions options;options.objectId=terrainSettings.objectId;options.maximumVertices=std::max<std::uint64_t>(16ULL*1024ULL*1024ULL,scene.meshes.front().vertices.size());options.maximumIndices=std::max<std::uint64_t>(48ULL*1024ULL*1024ULL,scene.meshes.front().triangles.size()*3ULL);std::vector<ImportDiagnostic> diagnostics;auto asset=cook_polygon_scene(scene,options,&diagnostics);const auto valid=validate_polygon_asset(asset);if(!valid)out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Error,"POLYGON",valid.message});else out.polygon=std::move(asset);for(const auto& d:diagnostics)out.diagnostics.push_back({d.severity==ImportDiagnostic::Severity::Error?HeightmapTerrainDiagnostic::Severity::Error:d.severity==ImportDiagnostic::Severity::Warning?HeightmapTerrainDiagnostic::Severity::Warning:HeightmapTerrainDiagnostic::Severity::Information,d.code,d.message});}
    if(terrainSettings.generateVoxelAsset){auto scene=make_scene(*image,terrainSettings,true);VoxelizeSettings settings;settings.mode=VoxelizationMode::Solid;settings.voxelSizeMeters=terrainSettings.voxelSizeMeters;settings.objectId=terrainSettings.objectId;settings.maximumWorkingVoxels=terrainSettings.maximumWorkingVoxels;auto asset=voxelize_scene(scene,settings);for(const auto& d:asset.diagnostics)out.diagnostics.push_back({d.severity==ImportDiagnostic::Severity::Error?HeightmapTerrainDiagnostic::Severity::Error:d.severity==ImportDiagnostic::Severity::Warning?HeightmapTerrainDiagnostic::Severity::Warning:HeightmapTerrainDiagnostic::Severity::Information,d.code,d.message});if(std::none_of(asset.diagnostics.begin(),asset.diagnostics.end(),[](const auto& d){return d.severity==ImportDiagnostic::Severity::Error;}))out.voxel=std::move(asset);}
    if(terrainSettings.generateNavigationMesh){const auto triangles=heightmap_navigation_triangles(*image,terrainSettings);auto nav=build_navigation_mesh(triangles,terrainSettings.navigation);for(const auto& d:nav.diagnostics)out.diagnostics.push_back({d.severity==NavigationBuildDiagnostic::Severity::Error?HeightmapTerrainDiagnostic::Severity::Error:d.severity==NavigationBuildDiagnostic::Severity::Warning?HeightmapTerrainDiagnostic::Severity::Warning:HeightmapTerrainDiagnostic::Severity::Information,d.code,d.message});if(nav.success())out.navigation=std::move(nav.mesh);}
    out.diagnostics.push_back({HeightmapTerrainDiagnostic::Severity::Information,"HEIGHTMAP",std::to_string(image->width)+"x"+std::to_string(image->height)+" "+image->sourceFormat+" heightmap imported at "+std::to_string(image->sourceBitDepth)+" bits"});return out;
}

} // namespace dve
