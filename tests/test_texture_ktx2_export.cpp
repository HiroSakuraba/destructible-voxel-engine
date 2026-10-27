#include "dve/texture_ktx2_export.hpp"
#include "dve/compressed_texture.hpp"
#include "dve/rhi/null_device.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    using namespace dve;
    try {
        for (const auto format:{rhi::TextureFormat::BC5RGUnorm,
                                rhi::TextureFormat::BC7RGBAUnorm,
                                rhi::TextureFormat::ASTC6x6RGBAUnorm}) {
            CompressedTextureAsset asset;
            asset.name="KTX-native";asset.format=format;
            std::uint32_t w=13U,h=9U;
            for(int mip=0;mip<4;++mip) {
                CompressedTextureMip level;
                level.width=w;level.height=h;
                level.blocks.resize(rhi::texture_row_bytes(format,w)*rhi::texture_rows(format,h));
                for(std::size_t i=0;i<level.blocks.size();++i)
                    level.blocks[i]=static_cast<std::byte>((i*23U+static_cast<unsigned>(mip))&255U);
                asset.mips.push_back(std::move(level));
                w=std::max(1U,w/2U);h=std::max(1U,h/2U);
            }
            std::string error;std::vector<std::byte> bytes;
            if(!encode_native_ktx2(asset,bytes,&error))
                throw std::runtime_error("libktx export failed: "+error);
            CompressedTextureAsset decoded;
            if(!parse_compressed_texture(bytes,asset.name,decoded,&error))
                throw std::runtime_error("DVE parser refused libktx file: "+error);
            if(decoded.format!=asset.format||decoded.mips.size()!=asset.mips.size())
                throw std::runtime_error("libktx format/mip mismatch");
            for(std::size_t i=0;i<asset.mips.size();++i)
                if(decoded.mips[i].blocks!=asset.mips[i].blocks)
                    throw std::runtime_error("KTX2 payload changed");
        }
        std::cout<<"T3 optional native KTX2: PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
