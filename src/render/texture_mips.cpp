#include "dve/render/polygon_renderer.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace dve::render {
namespace {
Float4 add4(Float4 a,Float4 b) noexcept {return {a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w};}
Float4 scale4(Float4 a,float s) noexcept {return {a.x*s,a.y*s,a.z*s,a.w*s};}
float decode(float v) noexcept {return v<=.04045F?v/12.92F:std::pow((v+.055F)/1.055F,2.4F);}
Float4 texel(const PolygonTextureMip& mip,std::uint32_t x,std::uint32_t y,PolygonTextureColorSpace space) noexcept {
 const auto i=(static_cast<std::size_t>(y)*mip.width+x)*4;
 Float4 v{mip.rgba8[i]/255.F,mip.rgba8[i+1]/255.F,mip.rgba8[i+2]/255.F,mip.rgba8[i+3]/255.F};
 if(space==PolygonTextureColorSpace::Srgb){v.x=decode(v.x);v.y=decode(v.y);v.z=decode(v.z);}return v;
}
}
PolygonTextureMipChain generate_texture_mips(const PolygonImage& image,bool normalMap,bool srgb) {
    PolygonTextureMipChain chain;
    if (image.width && image.height > std::numeric_limits<std::size_t>::max()/4U/image.width) return chain;
    const std::size_t requiredBytes =
        static_cast<std::size_t>(image.width) * image.height * 4U;
    if (image.width == 0 || image.height == 0 || image.rgba8.size() != requiredBytes) {
        return chain;
    }

    chain.heightBounds={1,0};
    for(std::size_t i=0;i<image.rgba8.size();i+=4){const float h=static_cast<float>(image.rgba8[i])/255;
        chain.heightBounds.x=(std::min)(chain.heightBounds.x,h);chain.heightBounds.y=(std::max)(chain.heightBounds.y,h);}
    chain.levels.push_back({image.width, image.height, image.rgba8});
    while (chain.levels.back().width > 1U || chain.levels.back().height > 1U) {
        const auto& source = chain.levels.back();
        PolygonTextureMip next;
        next.width = (std::max)(1U, source.width / 2U);
        next.height = (std::max)(1U, source.height / 2U);
        next.rgba8.resize(static_cast<std::size_t>(next.width) * next.height * 4U);

        for(std::uint32_t y=0;y<next.height;++y)for(std::uint32_t x=0;x<next.width;++x){
            // Float4 defaults alpha to one; an accumulation must start at zero.
            Float4 sum{0,0,0,0};float totalWeight=0;
            // Fractional area weights keep odd-sized boundary texels represented
            // without giving unequal source regions equal weight at the next level.
            const float x0=float(x)*float(source.width)/float(next.width);
            const float x1=float(x+1)*float(source.width)/float(next.width);
            const float y0=float(y)*float(source.height)/float(next.height);
            const float y1=float(y+1)*float(source.height)/float(next.height);
            const auto xEnd=(std::min)(source.width,static_cast<std::uint32_t>(std::ceil(x1)));
            const auto yEnd=(std::min)(source.height,static_cast<std::uint32_t>(std::ceil(y1)));
            for(auto sy=static_cast<std::uint32_t>(y0);sy<yEnd;++sy)
                for(auto sx=static_cast<std::uint32_t>(x0);sx<xEnd;++sx){
                    const float weight=((std::min)(x1,float(sx+1))-(std::max)(x0,float(sx)))*
                        ((std::min)(y1,float(sy+1))-(std::max)(y0,float(sy)));
                    const auto t=texel(source,sx,sy,srgb?PolygonTextureColorSpace::Srgb:PolygonTextureColorSpace::Linear);
                    sum=add4(sum,scale4(t,weight));totalWeight+=weight;
                }
            Float4 v=scale4(sum,1.0F/totalWeight);
            if(normalMap){
                const Float3 vector{v.x*2-1,v.y*2-1,v.z*2-1};
                const auto n=length_squared(vector)>1e-12F?normalize(vector):Float3{0,0,1};
                v.x=n.x*.5F+.5F;v.y=n.y*.5F+.5F;v.z=n.z*.5F+.5F;
            }
            const auto encode=[&](float value,bool color){
                if(color)value=value<=0.0031308F?value*12.92F:1.055F*std::pow(value,1/2.4F)-0.055F;
                return static_cast<std::uint8_t>(std::lround(std::clamp(value,0.0F,1.0F)*255));
            };
            const std::size_t o=(static_cast<std::size_t>(y)*next.width+x)*4;
            next.rgba8[o]=encode(v.x,srgb);next.rgba8[o+1]=encode(v.y,srgb);next.rgba8[o+2]=encode(v.z,srgb);next.rgba8[o+3]=encode(v.w,false);
        }
        chain.levels.push_back(std::move(next));
    }
    return chain;
}

} // namespace dve::render
