#include "dve/text3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while (false)

namespace {
using Bytes = std::vector<std::uint8_t>;

void u16(Bytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v >> 8U)); b.push_back(static_cast<std::uint8_t>(v)); }
void i16(Bytes& b, std::int16_t v) { u16(b, static_cast<std::uint16_t>(v)); }
void u32(Bytes& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v >> 24U)); b.push_back(static_cast<std::uint8_t>(v >> 16U)); b.push_back(static_cast<std::uint8_t>(v >> 8U)); b.push_back(static_cast<std::uint8_t>(v)); }
void set16(Bytes& b, std::size_t o, std::uint16_t v) { b[o]=static_cast<std::uint8_t>(v>>8U); b[o+1]=static_cast<std::uint8_t>(v); }
void set32(Bytes& b, std::size_t o, std::uint32_t v) { b[o]=static_cast<std::uint8_t>(v>>24U); b[o+1]=static_cast<std::uint8_t>(v>>16U); b[o+2]=static_cast<std::uint8_t>(v>>8U); b[o+3]=static_cast<std::uint8_t>(v); }
std::uint32_t tag(const char* s) { return (static_cast<std::uint32_t>(s[0])<<24U)|(static_cast<std::uint32_t>(s[1])<<16U)|(static_cast<std::uint32_t>(s[2])<<8U)|static_cast<std::uint32_t>(s[3]); }
void align4(Bytes& b) { while (b.size()%4U) b.push_back(0); }

Bytes make_test_font() {
    std::map<std::string, Bytes> tables;
    Bytes head(54,0); set16(head,18,1000); set16(head,50,1); tables["head"]=head;
    Bytes maxp; u32(maxp,0x00010000U); u16(maxp,2); tables["maxp"]=maxp;
    Bytes hhea(36,0); set32(hhea,0,0x00010000U); set16(hhea,4,800); set16(hhea,6,static_cast<std::uint16_t>(-200)); set16(hhea,8,200); set16(hhea,34,2); tables["hhea"]=hhea;
    Bytes hmtx; u16(hmtx,500); i16(hmtx,0); u16(hmtx,1100); i16(hmtx,0); tables["hmtx"]=hmtx;

    Bytes glyf;
    i16(glyf,1); i16(glyf,0); i16(glyf,0); i16(glyf,1000); i16(glyf,1000);
    u16(glyf,2); u16(glyf,0);
    glyf.push_back(1); glyf.push_back(1); glyf.push_back(1);
    i16(glyf,0); i16(glyf,500); i16(glyf,500);
    i16(glyf,0); i16(glyf,1000); i16(glyf,-1000);
    tables["glyf"]=glyf;
    Bytes loca; u32(loca,0); u32(loca,0); u32(loca,static_cast<std::uint32_t>(glyf.size())); tables["loca"]=loca;

    Bytes cmap;
    u16(cmap,0); u16(cmap,1); u16(cmap,3); u16(cmap,1); u32(cmap,12);
    u16(cmap,4); u16(cmap,32); u16(cmap,0); u16(cmap,4); u16(cmap,4); u16(cmap,1); u16(cmap,0);
    u16(cmap,65); u16(cmap,0xFFFF); u16(cmap,0);
    u16(cmap,65); u16(cmap,0xFFFF);
    i16(cmap,-64); i16(cmap,1);
    u16(cmap,0); u16(cmap,0);
    tables["cmap"]=cmap;

    const std::uint16_t count=static_cast<std::uint16_t>(tables.size());
    Bytes out(12U+static_cast<std::size_t>(count)*16U,0);
    set32(out,0,0x00010000U); set16(out,4,count);
    std::size_t record=12, cursor=out.size();
    for (const auto& [name,data] : tables) {
        while(cursor%4U) { out.push_back(0); ++cursor; }
        set32(out,record,tag(name.c_str()));
        set32(out,record+4,0);
        set32(out,record+8,static_cast<std::uint32_t>(cursor));
        set32(out,record+12,static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(),data.begin(),data.end()); cursor+=data.size(); record+=16;
    }
    return out;
}

std::filesystem::path temp_path(std::string_view ext) {
    static std::uint64_t id=1;
    return std::filesystem::temp_directory_path()/std::filesystem::path("dve_text3d_test_"+std::to_string(id++)+std::string(ext));
}

void test_cook_roundtrip_preview() {
    const auto fontPath=temp_path(".ttf");
    const auto bytes=make_test_font();
    { std::ofstream out(fontPath,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); }
    dve::Text3DCookOptions options;
    options.style.emSizeMeters=0.8F;
    options.style.extrusionDepthMeters=0.18F;
    options.style.horizontalBands=4;
    options.style.verticalBands=4;
    options.style.curveSubdivision=5;
    const auto cooked=dve::cook_text3d(fontPath,"AAA",options);
    CHECK(cooked);
    CHECK(cooked.asset.font.unitsPerEm==1000U);
    CHECK(cooked.asset.glyphInstances.size()==3U);
    CHECK(!cooked.asset.atlas.curveTexels.empty());
    CHECK(!cooked.asset.atlas.bandTexels.empty());
    CHECK(!cooked.asset.sideMesh.vertices.empty());
    CHECK(!cooked.asset.sideMesh.indices.empty());
    CHECK(cooked.asset.contentHash==dve::text3d_content_hash(cooked.asset));
    const auto packet=dve::build_text3d_render_packet(cooked.asset);
    CHECK(packet.faceVertices.size()==cooked.asset.glyphInstances.size()*8U);
    CHECK(packet.faceIndices.size()==cooked.asset.glyphInstances.size()*12U);
    CHECK(packet.contentHash!=0U);

    const auto assetPath=temp_path(".dtext");
    std::string error;
    CHECK(dve::write_dtext(assetPath,cooked.asset,&error));
    const auto read=dve::read_dtext(assetPath);
    CHECK(read);
    CHECK(read.asset.textUtf8=="AAA");
    CHECK(read.asset.glyphInstances.size()==3U);
    CHECK(read.asset.contentHash==cooked.asset.contentHash);

    const auto preview=temp_path(".ppm");
    CHECK(dve::write_text3d_preview_ppm(preview,read.asset,{},&error));
    CHECK(std::filesystem::file_size(preview)>960U*540U*3U);

    auto corrupted=read.asset;
    corrupted.atlas.bandTexels.clear();
    corrupted.contentHash=dve::text3d_content_hash(corrupted);
    CHECK(!dve::validate_text3d_asset(corrupted,&error));

    std::filesystem::remove(fontPath);
    std::filesystem::remove(assetPath);
    std::filesystem::remove(preview);
}

void test_utf8_and_limits() {
    const auto fontPath=temp_path(".ttf");
    const auto bytes=make_test_font();
    { std::ofstream out(fontPath,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); }
    dve::Text3DCookOptions options;
    options.maximumCodepoints=2;
    CHECK(!dve::cook_text3d(fontPath,"AAA",options));
    const std::string invalid{"\xC0\xAF",2};
    CHECK(!dve::cook_text3d(fontPath,invalid,{}));
    CHECK(!dve::cook_text3d(fontPath,"",{}));
    CHECK(!dve::cook_text3d(fontPath,"   ",{}));
    std::filesystem::remove(fontPath);
}
}

int main() {
    try {
        test_cook_roundtrip_preview();
        test_utf8_and_limits();
        std::cout << "text3d tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
