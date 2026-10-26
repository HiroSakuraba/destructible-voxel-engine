#include "dve/rgba8_png.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
namespace dve {
namespace {
void set_error(std::string* error,std::string message){if(error)*error=std::move(message);}
std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256U; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
        table[n] = c;
    }
    return table;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFU) {
    static const std::array<std::uint32_t, 256> table = make_crc_table();
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    return crc;
}

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_chunk(std::vector<std::uint8_t>& out, const char type[4], const std::vector<std::uint8_t>& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t typeOffset = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t crc = crc32(out.data() + typeOffset, 4U + data.size()) ^ 0xFFFFFFFFU;
    put_be32(out, crc);
}

}
bool write_rgba8_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> pixels, std::string* error) {
    const auto count=std::uint64_t(width)*height;
    // One IDAT chunk uses a 32-bit length. Bound stored-deflate overhead too.
    if (!width || !height || count>0x1fffffffULL || count*4 != pixels.size() || count*4+height>0x7fe00000ULL) {
        set_error(error, "invalid image");
        return false;
    }
    // Raw scanlines: filter type 0 + RGBA row.
    const std::size_t stride = static_cast<std::size_t>(width) * 4U;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1U) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0U);
        const auto* row = pixels.data() + stride * y;
        raw.insert(raw.end(), row, row + stride);
    }
    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    std::size_t offset = 0;
    do {
        const std::size_t length = (std::min<std::size_t>)(65535U, raw.size() - offset);
        const bool final = offset + length == raw.size();
        zlib.push_back(final ? 1U : 0U);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length));
        zlib.push_back(static_cast<std::uint8_t>(~length >> 8U));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());
    put_be32(zlib, (b << 16U) | a);

    std::vector<std::uint8_t> png{0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    std::vector<std::uint8_t> header;
    put_be32(header, width);
    put_be32(header, height);
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U}); // 8-bit RGBA, deflate, no filter, no interlace
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", zlib);
    put_chunk(png, "IEND", {});

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error, "could not open " + path.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        set_error(error, "could not write " + path.string());
        return false;
    }
    return true;
}

}
