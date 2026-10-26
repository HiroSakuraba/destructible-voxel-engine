#include "dve/player/frame_image.hpp"
#include "dve/rgba8_png.hpp"

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

bool write_png(const std::filesystem::path& path,const Rgba8Image& image,std::string* error) {
    return write_rgba8_png(path,image.width,image.height,image.pixels,error);
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
