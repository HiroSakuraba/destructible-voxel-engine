#include "dve/player/frame_image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace dve::player {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

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

} // namespace

std::uint64_t hash_image(const Rgba8Image& image) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (int shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(image.width >> shift));
    for (int shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(image.height >> shift));
    for (const std::uint8_t byte : image.pixels) mix(byte);
    return hash;
}

std::string format_image_hash(std::uint64_t hash) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

bool write_png(const std::filesystem::path& path, const Rgba8Image& image, std::string* error) {
    if (!image.valid()) {
        set_error(error, "invalid image");
        return false;
    }
    // Raw scanlines: filter type 0 + RGBA row.
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4U;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1U) * image.height);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0U);
        const auto* row = image.pixels.data() + stride * y;
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
        const std::size_t length = std::min<std::size_t>(65535U, raw.size() - offset);
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
    put_be32(header, image.width);
    put_be32(header, image.height);
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

bool write_ppm(const std::filesystem::path& path, const Rgba8Image& image, std::string* error) {
    if (!image.valid()) {
        set_error(error, "invalid image");
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error, "could not open " + path.string());
        return false;
    }
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    std::vector<char> rgb(static_cast<std::size_t>(image.width) * image.height * 3U);
    for (std::size_t i = 0, j = 0; i < image.pixels.size(); i += 4U, j += 3U) {
        rgb[j] = static_cast<char>(image.pixels[i]);
        rgb[j + 1U] = static_cast<char>(image.pixels[i + 1U]);
        rgb[j + 2U] = static_cast<char>(image.pixels[i + 2U]);
    }
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!out) {
        set_error(error, "could not write " + path.string());
        return false;
    }
    return true;
}

std::optional<Rgba8Image> read_ppm(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "could not open " + path.string());
        return std::nullopt;
    }
    std::string magic;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t maximum{};
    in >> magic >> width >> height >> maximum;
    if (!in || magic != "P6" || maximum != 255U || width == 0U || height == 0U ||
        width > 16384U || height > 16384U) {
        set_error(error, "unsupported PPM header in " + path.string());
        return std::nullopt;
    }
    in.get(); // single whitespace after the header
    std::vector<char> rgb(static_cast<std::size_t>(width) * height * 3U);
    in.read(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!in) {
        set_error(error, "truncated PPM " + path.string());
        return std::nullopt;
    }
    Rgba8Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t i = 0, j = 0; j < rgb.size(); i += 4U, j += 3U) {
        image.pixels[i] = static_cast<std::uint8_t>(rgb[j]);
        image.pixels[i + 1U] = static_cast<std::uint8_t>(rgb[j + 1U]);
        image.pixels[i + 2U] = static_cast<std::uint8_t>(rgb[j + 2U]);
        image.pixels[i + 3U] = 255U;
    }
    return image;
}

std::optional<Rgba8Image> downsample_box(const Rgba8Image& image, std::uint32_t factor) {
    if (!image.valid() || factor == 0U || image.width % factor != 0U || image.height % factor != 0U)
        return std::nullopt;
    Rgba8Image result;
    result.width = image.width / factor;
    result.height = image.height / factor;
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4U);
    const std::uint32_t area = factor * factor;
    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            std::array<std::uint32_t, 4> sum{};
            for (std::uint32_t dy = 0; dy < factor; ++dy) {
                for (std::uint32_t dx = 0; dx < factor; ++dx) {
                    const std::size_t source =
                        (static_cast<std::size_t>(y * factor + dy) * image.width + x * factor + dx) * 4U;
                    for (std::size_t c = 0; c < 4U; ++c) sum[c] += image.pixels[source + c];
                }
            }
            const std::size_t target = (static_cast<std::size_t>(y) * result.width + x) * 4U;
            for (std::size_t c = 0; c < 4U; ++c)
                result.pixels[target + c] = static_cast<std::uint8_t>((sum[c] + area / 2U) / area);
        }
    }
    return result;
}

std::optional<ImageDifference> compare_images(const Rgba8Image& a, const Rgba8Image& b,
                                              std::uint32_t perChannelThreshold) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return std::nullopt;
    ImageDifference difference;
    difference.pixelCount = static_cast<std::uint64_t>(a.width) * a.height;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4U) {
        bool over = false;
        for (std::size_t c = 0; c < 3U; ++c) {
            const auto delta = static_cast<std::uint32_t>(std::abs(int(a.pixels[i + c]) - int(b.pixels[i + c])));
            total += delta;
            difference.maxChannelDelta = std::max(difference.maxChannelDelta, delta);
            over = over || delta > perChannelThreshold;
        }
        if (over) ++difference.pixelsOverThreshold;
    }
    difference.meanAbsoluteDelta = static_cast<double>(total) / static_cast<double>(difference.pixelCount * 3U);
    return difference;
}

} // namespace dve::player
