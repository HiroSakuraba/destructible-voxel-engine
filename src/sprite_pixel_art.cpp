#include "dve/sprite_pixel_art.hpp"
#include "dve/sprite_authoring.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <type_traits>

#include <png.h>

namespace dve::editor {
namespace {

constexpr std::uint32_t kPixelFormatVersion = 1U;
constexpr std::array<char, 8> kPixelMagic{'D','V','E','P','I','X','1','\0'};
constexpr std::size_t kMaximumPixelDimension = 16384U;
constexpr std::size_t kMaximumPixelFrames = 4096U;
constexpr std::size_t kMaximumPixelLayers = 256U;
constexpr std::size_t kMaximumHistory = 128U;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash ^= static_cast<std::uint8_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
}

void hash_bytes(std::uint64_t& hash, std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
}

void hash_string(std::uint64_t& hash, std::string_view text) noexcept {
    hash_value(hash, static_cast<std::uint64_t>(text.size()));
    hash_bytes(hash, std::as_bytes(std::span(text.data(), text.size())));
}

template <class T>
bool write_scalar(std::ofstream& stream, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool write_string(std::ofstream& stream, std::string_view value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    if (!write_scalar(stream, size)) return false;
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(stream);
}

template <class T>
bool read_scalar(std::ifstream& stream, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

bool read_string(std::ifstream& stream, std::string& value, std::uint32_t maximum = 1U << 20U) {
    std::uint32_t size{};
    if (!read_scalar(stream, size) || size > maximum) return false;
    value.resize(size);
    stream.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

bool atomic_replace(const std::filesystem::path& temp, const std::filesystem::path& destination,
                    std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(temp, destination, ec);
    if (!ec) return true;
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temp, destination, ec);
    if (ec) return fail(error, "could not replace '" + destination.string() + "': " + ec.message());
    return true;
}

bool encode_png_rgba8(std::uint32_t width, std::uint32_t height,
                      std::span<const std::byte> rgba,
                      std::vector<std::byte>& encoded,
                      std::string* error) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (width == 0U || height == 0U || rgba.size() != expected)
        return fail(error, "pixel-art PNG dimensions do not match the RGBA payload");
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t bytes{};
    if (!png_image_write_to_memory(&image, nullptr, &bytes, 0,
                                   rgba.data(), 0, nullptr))
        return fail(error, "pixel-art PNG size query failed: " + std::string(image.message));
    encoded.resize(bytes);
    if (!png_image_write_to_memory(&image, encoded.data(), &bytes, 0,
                                   rgba.data(), 0, nullptr))
        return fail(error, "pixel-art PNG encode failed: " + std::string(image.message));
    encoded.resize(bytes);
    return true;
}

bool write_file_atomic(const std::filesystem::path& path, std::span<const std::byte> bytes,
                       std::string* error) {
    const auto temp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "could not open temporary output '" + temp + "'");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) return fail(error, "could not write temporary output '" + temp + "'");
    return atomic_replace(temp, path, error);
}

std::size_t pixel_offset(std::uint32_t width, std::int32_t x, std::int32_t y,
                         std::size_t bytesPerPixel) {
    return (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * bytesPerPixel;
}

SpriteColor8 palette_color(const SpritePixelArtDocument& document, std::uint8_t index) {
    if (document.palette.banks.empty() || document.palette.banks.front().colors.empty())
        return {index, index, index, 255U};
    const auto& colors = document.palette.banks.front().colors;
    if (index >= colors.size()) return {255U, 0U, 255U, 255U};
    return colors[index];
}

SpriteColor8 read_layer_color(const SpritePixelArtDocument& document,
                              const SpritePixelLayer& layer, std::size_t texel) {
    if (document.storage == SpritePixelStorage::Indexed8) {
        return palette_color(document, static_cast<std::uint8_t>(layer.pixels[texel]));
    }
    const std::size_t offset = texel * 4U;
    return {static_cast<std::uint8_t>(layer.pixels[offset + 0U]),
            static_cast<std::uint8_t>(layer.pixels[offset + 1U]),
            static_cast<std::uint8_t>(layer.pixels[offset + 2U]),
            static_cast<std::uint8_t>(layer.pixels[offset + 3U])};
}

void blend_color(SpriteColor8 source, std::uint8_t opacity, SpritePixelBlendMode mode,
                 SpriteColor8& destination) {
    const float sourceAlpha = (static_cast<float>(source.a) / 255.0F) *
                              (static_cast<float>(opacity) / 255.0F);
    const float destinationAlpha = static_cast<float>(destination.a) / 255.0F;
    if (sourceAlpha <= 0.0F) return;
    auto blendChannel = [&](std::uint8_t s, std::uint8_t d) -> float {
        const float sf = static_cast<float>(s) / 255.0F;
        const float df = static_cast<float>(d) / 255.0F;
        switch (mode) {
        case SpritePixelBlendMode::Add: return std::min(1.0F, sf + df);
        case SpritePixelBlendMode::Multiply: return sf * df;
        case SpritePixelBlendMode::Normal: return sf;
        }
        return sf;
    };
    const float outputAlpha = sourceAlpha + destinationAlpha * (1.0F - sourceAlpha);
    if (outputAlpha <= 0.0F) {
        destination = {};
        return;
    }
    const auto channel = [&](std::uint8_t s, std::uint8_t d) -> std::uint8_t {
        const float mixed = (blendChannel(s, d) * sourceAlpha +
                             (static_cast<float>(d) / 255.0F) * destinationAlpha *
                                 (1.0F - sourceAlpha)) / outputAlpha;
        return static_cast<std::uint8_t>(std::clamp(std::lround(mixed * 255.0F), 0L, 255L));
    };
    destination.r = channel(source.r, destination.r);
    destination.g = channel(source.g, destination.g);
    destination.b = channel(source.b, destination.b);
    destination.a = static_cast<std::uint8_t>(std::clamp(std::lround(outputAlpha * 255.0F), 0L, 255L));
}

} // namespace

bool SpritePixelRect::contains(std::int32_t px, std::int32_t py) const noexcept {
    if (empty() || px < x || py < y) return false;
    const std::int64_t right = static_cast<std::int64_t>(x) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(y) + height;
    return static_cast<std::int64_t>(px) < right && static_cast<std::int64_t>(py) < bottom;
}

bool SpritePixelArtDocument::validate(std::string* error) const {
    if (name.empty()) return fail(error, "pixel-art document name is empty");
    if (width == 0U || height == 0U || width > kMaximumPixelDimension || height > kMaximumPixelDimension)
        return fail(error, "pixel-art dimensions are outside the supported range");
    if (frames.empty() || frames.size() > kMaximumPixelFrames)
        return fail(error, "pixel-art document has an invalid frame count");
    if (storage == SpritePixelStorage::Indexed8 && !palette.validate(error)) return false;
    const std::size_t bytesPerPixel = storage == SpritePixelStorage::Indexed8 ? 1U : 4U;
    const std::uint64_t expected64 = static_cast<std::uint64_t>(width) * height * bytesPerPixel;
    if (expected64 > std::numeric_limits<std::size_t>::max())
        return fail(error, "pixel-art frame storage exceeds addressable memory");
    const std::size_t expected = static_cast<std::size_t>(expected64);
    std::vector<std::uint64_t> ids;
    for (const auto& frame : frames) {
        if (frame.name.empty() || !std::isfinite(frame.durationSeconds) || frame.durationSeconds <= 0.0F)
            return fail(error, "pixel-art frame has invalid metadata");
        if (frame.layers.empty() || frame.layers.size() > kMaximumPixelLayers)
            return fail(error, "pixel-art frame has an invalid layer count");
        for (const auto& layer : frame.layers) {
            if (layer.id == 0U || layer.name.empty() || layer.pixels.size() != expected)
                return fail(error, "pixel-art layer has invalid identity or pixel storage");
            if (std::find(ids.begin(), ids.end(), layer.id) != ids.end())
                return fail(error, "pixel-art layer IDs must be unique across the document");
            ids.push_back(layer.id);
        }
    }
    return true;
}

void SpritePixelArtDocument::recompute_hash() noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, name);
    hash_value(hash, storage);
    hash_value(hash, width);
    hash_value(hash, height);
    hash_value(hash, palette.contentHash);
    for (const auto& frame : frames) {
        hash_string(hash, frame.name);
        hash_value(hash, frame.durationSeconds);
        for (const auto& layer : frame.layers) {
            hash_value(hash, layer.id);
            hash_string(hash, layer.name);
            hash_value(hash, layer.visible);
            hash_value(hash, layer.locked);
            hash_value(hash, layer.opacity);
            hash_value(hash, layer.blend);
            hash_bytes(hash, layer.pixels);
        }
    }
    contentHash = hash;
}

SpritePixelArtSession::SpritePixelArtSession(SpritePixelArtDocument document)
    : document_(std::move(document)) {
    std::string error;
    if (!document_.validate(&error)) {
        SpritePaletteAsset palette;
        palette.name = "Default Pixel Palette";
        palette.transparentIndex = 0U;
        SpritePaletteBank bank;
        bank.name = "Default";
        bank.colors = {{0U,0U,0U,0U}, {255U,255U,255U,255U}, {0U,0U,0U,255U},
                       {255U,0U,255U,255U}, {0U,255U,255U,255U}, {255U,255U,0U,255U}};
        palette.banks.push_back(std::move(bank));
        palette.recompute_hash();
        (void)new_document("Untitled Pixel Art", 16U, 16U, SpritePixelStorage::Indexed8,
                           std::move(palette), nullptr);
    }
    document_.recompute_hash();
    savedHash_ = document_.contentHash;
}

std::size_t SpritePixelArtSession::bytes_per_pixel() const noexcept {
    return document_.storage == SpritePixelStorage::Indexed8 ? 1U : 4U;
}

SpritePixelLayer* SpritePixelArtSession::current_layer() noexcept {
    if (selectedFrame_ >= document_.frames.size()) return nullptr;
    auto& layers = document_.frames[selectedFrame_].layers;
    return selectedLayer_ < layers.size() ? &layers[selectedLayer_] : nullptr;
}

const SpritePixelLayer* SpritePixelArtSession::current_layer() const noexcept {
    if (selectedFrame_ >= document_.frames.size()) return nullptr;
    const auto& layers = document_.frames[selectedFrame_].layers;
    return selectedLayer_ < layers.size() ? &layers[selectedLayer_] : nullptr;
}

bool SpritePixelArtSession::new_document(std::string name, std::uint32_t width, std::uint32_t height,
                                         SpritePixelStorage storage, SpritePaletteAsset palette,
                                         std::string* error) {
    SpritePixelArtDocument replacement;
    replacement.name = std::move(name);
    replacement.width = width;
    replacement.height = height;
    replacement.storage = storage;
    replacement.palette = std::move(palette);
    if (storage == SpritePixelStorage::Indexed8 && replacement.palette.contentHash == 0U)
        replacement.palette.recompute_hash();
    SpritePixelFrame frame;
    frame.name = "frame_0";
    SpritePixelLayer layer;
    layer.id = 1U;
    layer.name = "Layer 1";
    const std::size_t bpp = storage == SpritePixelStorage::Indexed8 ? 1U : 4U;
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * bpp;
    if (bytes > 512ULL * 1024ULL * 1024ULL)
        return fail(error, "new pixel-art document exceeds the 512 MiB authoring bound");
    layer.pixels.assign(static_cast<std::size_t>(bytes), std::byte{0});
    frame.layers.push_back(std::move(layer));
    replacement.frames.push_back(std::move(frame));
    replacement.recompute_hash();
    if (!replacement.validate(error)) return false;
    document_ = std::move(replacement);
    selectedFrame_ = selectedLayer_ = 0U;
    selection_.reset();
    undo_.clear();
    redo_.clear();
    savedHash_ = document_.contentHash;
    return true;
}

bool SpritePixelArtSession::open(const std::filesystem::path& path, std::string* error) {
    SpritePixelArtDocument replacement;
    if (!read_dvepixel(path, replacement, error)) return false;
    document_ = std::move(replacement);
    selectedFrame_ = selectedLayer_ = 0U;
    selection_.reset();
    undo_.clear();
    redo_.clear();
    savedHash_ = document_.contentHash;
    return true;
}

bool SpritePixelArtSession::save(const std::filesystem::path& path, std::string* error) {
    document_.recompute_hash();
    if (!write_dvepixel(path, document_, error)) return false;
    savedHash_ = document_.contentHash;
    return true;
}

bool SpritePixelArtSession::select_frame(std::size_t index) noexcept {
    if (index >= document_.frames.size()) return false;
    selectedFrame_ = index;
    selectedLayer_ = std::min(selectedLayer_, document_.frames[index].layers.size() - 1U);
    selection_.reset();
    return true;
}

bool SpritePixelArtSession::select_layer(std::size_t index) noexcept {
    if (selectedFrame_ >= document_.frames.size() || index >= document_.frames[selectedFrame_].layers.size())
        return false;
    selectedLayer_ = index;
    return true;
}

bool SpritePixelArtSession::mutate(std::string label,
                                   const std::function<bool(std::string*)>& operation,
                                   std::string* error) {
    Snapshot before{std::move(label), document_, selectedFrame_, selectedLayer_, selection_};
    if (!operation(error)) return false;
    document_.recompute_hash();
    if (!document_.validate(error)) {
        restore(before);
        return false;
    }
    undo_.push_back(std::move(before));
    if (undo_.size() > kMaximumHistory) undo_.erase(undo_.begin());
    redo_.clear();
    return true;
}

void SpritePixelArtSession::restore(const Snapshot& snapshot) noexcept {
    document_ = snapshot.document;
    selectedFrame_ = snapshot.frame;
    selectedLayer_ = snapshot.layer;
    selection_ = snapshot.selection;
}

bool SpritePixelArtSession::add_frame(std::string name, bool cloneCurrent, std::string* error) {
    return mutate("Add frame", [&](std::string* nested) {
        if (name.empty()) return fail(nested, "pixel-art frame name is empty");
        SpritePixelFrame frame = cloneCurrent ? document_.frames[selectedFrame_] : SpritePixelFrame{};
        frame.name = std::move(name);
        if (!cloneCurrent) {
            SpritePixelLayer layer;
            layer.id = 1U;
            for (const auto& existing : document_.frames)
                for (const auto& candidate : existing.layers) layer.id = std::max(layer.id, candidate.id + 1U);
            layer.name = "Layer 1";
            layer.pixels.assign(static_cast<std::size_t>(document_.width) * document_.height * bytes_per_pixel(),
                                std::byte{0});
            frame.layers.push_back(std::move(layer));
        } else {
            std::uint64_t nextId = 1U;
            for (const auto& existing : document_.frames)
                for (const auto& candidate : existing.layers) nextId = std::max(nextId, candidate.id + 1U);
            for (auto& layer : frame.layers) layer.id = nextId++;
        }
        document_.frames.push_back(std::move(frame));
        selectedFrame_ = document_.frames.size() - 1U;
        selectedLayer_ = 0U;
        selection_.reset();
        return true;
    }, error);
}

bool SpritePixelArtSession::remove_frame(std::size_t index, std::string* error) {
    return mutate("Remove frame", [&](std::string* nested) {
        if (document_.frames.size() <= 1U) return fail(nested, "pixel-art document must retain one frame");
        if (index >= document_.frames.size()) return fail(nested, "pixel-art frame index is out of range");
        document_.frames.erase(document_.frames.begin() + static_cast<std::ptrdiff_t>(index));
        selectedFrame_ = std::min(selectedFrame_, document_.frames.size() - 1U);
        selectedLayer_ = std::min(selectedLayer_, document_.frames[selectedFrame_].layers.size() - 1U);
        selection_.reset();
        return true;
    }, error);
}

bool SpritePixelArtSession::set_frame_duration(std::size_t index, float seconds, std::string* error) {
    return mutate("Set frame duration", [&](std::string* nested) {
        if (index >= document_.frames.size() || !std::isfinite(seconds) || seconds <= 0.0F)
            return fail(nested, "pixel-art frame duration is invalid");
        document_.frames[index].durationSeconds = seconds;
        return true;
    }, error);
}

bool SpritePixelArtSession::add_layer(std::string name, std::string* error) {
    return mutate("Add layer", [&](std::string* nested) {
        if (name.empty()) return fail(nested, "pixel-art layer name is empty");
        auto& frame = document_.frames[selectedFrame_];
        if (frame.layers.size() >= kMaximumPixelLayers) return fail(nested, "pixel-art layer limit reached");
        SpritePixelLayer layer;
        layer.id = 1U;
        for (const auto& existingFrame : document_.frames)
            for (const auto& existing : existingFrame.layers) layer.id = std::max(layer.id, existing.id + 1U);
        layer.name = std::move(name);
        layer.pixels.assign(static_cast<std::size_t>(document_.width) * document_.height * bytes_per_pixel(),
                            std::byte{0});
        frame.layers.push_back(std::move(layer));
        selectedLayer_ = frame.layers.size() - 1U;
        return true;
    }, error);
}

bool SpritePixelArtSession::remove_layer(std::size_t index, std::string* error) {
    return mutate("Remove layer", [&](std::string* nested) {
        auto& layers = document_.frames[selectedFrame_].layers;
        if (layers.size() <= 1U) return fail(nested, "pixel-art frame must retain one layer");
        if (index >= layers.size()) return fail(nested, "pixel-art layer index is out of range");
        layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(index));
        selectedLayer_ = std::min(selectedLayer_, layers.size() - 1U);
        return true;
    }, error);
}

bool SpritePixelArtSession::move_layer(std::size_t from, std::size_t to, std::string* error) {
    return mutate("Move layer", [&](std::string* nested) {
        auto& layers = document_.frames[selectedFrame_].layers;
        if (from >= layers.size() || to >= layers.size()) return fail(nested, "pixel-art layer move is out of range");
        auto layer = std::move(layers[from]);
        layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(from));
        layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(to), std::move(layer));
        selectedLayer_ = to;
        return true;
    }, error);
}

bool SpritePixelArtSession::set_layer_visibility(std::size_t index, bool visible, std::string* error) {
    return mutate("Set layer visibility", [&](std::string* nested) {
        auto& layers = document_.frames[selectedFrame_].layers;
        if (index >= layers.size()) return fail(nested, "pixel-art layer index is out of range");
        layers[index].visible = visible;
        return true;
    }, error);
}

bool SpritePixelArtSession::set_layer_opacity(std::size_t index, std::uint8_t opacity, std::string* error) {
    return mutate("Set layer opacity", [&](std::string* nested) {
        auto& layers = document_.frames[selectedFrame_].layers;
        if (index >= layers.size()) return fail(nested, "pixel-art layer index is out of range");
        layers[index].opacity = opacity;
        return true;
    }, error);
}

bool SpritePixelArtSession::normalize_coordinate(std::int32_t& x, std::int32_t& y) const noexcept {
    const auto width = static_cast<std::int32_t>(document_.width);
    const auto height = static_cast<std::int32_t>(document_.height);
    if (wrapPainting_) {
        x = ((x % width) + width) % width;
        y = ((y % height) + height) % height;
        return true;
    }
    return x >= 0 && y >= 0 && x < width && y < height;
}

bool SpritePixelArtSession::set_pixel(SpritePixelLayer& layer, std::int32_t x, std::int32_t y,
                                      bool erasePixel, std::string* error) {
    if (layer.locked) return fail(error, "selected pixel-art layer is locked");
    if (!normalize_coordinate(x, y)) return false;
    const std::size_t offset = pixel_offset(document_.width, x, y, bytes_per_pixel());
    if (document_.storage == SpritePixelStorage::Indexed8) {
        const std::uint8_t transparent = document_.palette.transparentIndex
            ? static_cast<std::uint8_t>(*document_.palette.transparentIndex) : 0U;
        layer.pixels[offset] = static_cast<std::byte>(erasePixel ? transparent : selectedIndex_);
    } else {
        const SpriteColor8 color = erasePixel ? SpriteColor8{} : selectedColor_;
        layer.pixels[offset + 0U] = static_cast<std::byte>(color.r);
        layer.pixels[offset + 1U] = static_cast<std::byte>(color.g);
        layer.pixels[offset + 2U] = static_cast<std::byte>(color.b);
        layer.pixels[offset + 3U] = static_cast<std::byte>(color.a);
    }
    return true;
}

bool SpritePixelArtSession::paint(std::int32_t x, std::int32_t y, std::string* error) {
    return mutate("Paint pixel", [&](std::string* nested) {
        auto* layer = current_layer();
        return layer && set_pixel(*layer, x, y, false, nested);
    }, error);
}

bool SpritePixelArtSession::erase(std::int32_t x, std::int32_t y, std::string* error) {
    return mutate("Erase pixel", [&](std::string* nested) {
        auto* layer = current_layer();
        return layer && set_pixel(*layer, x, y, true, nested);
    }, error);
}

bool SpritePixelArtSession::draw_line(std::int32_t x0, std::int32_t y0,
                                      std::int32_t x1, std::int32_t y1,
                                      std::string* error) {
    return mutate("Draw line", [&](std::string* nested) {
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        const std::int32_t dx = std::abs(x1 - x0);
        const std::int32_t sx = x0 < x1 ? 1 : -1;
        const std::int32_t dy = -std::abs(y1 - y0);
        const std::int32_t sy = y0 < y1 ? 1 : -1;
        std::int32_t accumulator = dx + dy;
        for (;;) {
            (void)set_pixel(*layer, x0, y0, false, nested);
            if (x0 == x1 && y0 == y1) break;
            const std::int32_t doubled = accumulator * 2;
            if (doubled >= dy) { accumulator += dy; x0 += sx; }
            if (doubled <= dx) { accumulator += dx; y0 += sy; }
        }
        return true;
    }, error);
}

bool SpritePixelArtSession::draw_rectangle(SpritePixelRect rect, bool filled, std::string* error) {
    return mutate("Draw rectangle", [&](std::string* nested) {
        if (rect.empty()) return fail(nested, "pixel-art rectangle is empty");
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        for (std::uint32_t row = 0; row < rect.height; ++row) {
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                if (!filled && row != 0U && column != 0U && row + 1U != rect.height && column + 1U != rect.width)
                    continue;
                (void)set_pixel(*layer, rect.x + static_cast<std::int32_t>(column),
                                rect.y + static_cast<std::int32_t>(row), false, nested);
            }
        }
        return true;
    }, error);
}

bool SpritePixelArtSession::flood_fill(std::int32_t x, std::int32_t y, std::string* error) {
    return mutate("Flood fill", [&](std::string* nested) {
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        if (!normalize_coordinate(x, y)) return fail(nested, "flood-fill origin is outside the canvas");
        const std::size_t bpp = bytes_per_pixel();
        const std::size_t origin = pixel_offset(document_.width, x, y, bpp);
        std::array<std::byte, 4> target{};
        std::array<std::byte, 4> replacement{};
        std::copy_n(layer->pixels.begin() + static_cast<std::ptrdiff_t>(origin), bpp, target.begin());
        if (document_.storage == SpritePixelStorage::Indexed8) replacement[0] = static_cast<std::byte>(selectedIndex_);
        else {
            replacement = {static_cast<std::byte>(selectedColor_.r), static_cast<std::byte>(selectedColor_.g),
                           static_cast<std::byte>(selectedColor_.b), static_cast<std::byte>(selectedColor_.a)};
        }
        if (std::equal(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(bpp), replacement.begin())) return true;
        std::queue<std::pair<std::int32_t, std::int32_t>> queue;
        std::vector<std::uint8_t> visited(static_cast<std::size_t>(document_.width) * document_.height, 0U);
        queue.emplace(x, y);
        while (!queue.empty()) {
            auto [cx, cy] = queue.front(); queue.pop();
            if (cx < 0 || cy < 0 || cx >= static_cast<std::int32_t>(document_.width) ||
                cy >= static_cast<std::int32_t>(document_.height)) continue;
            const std::size_t texel = static_cast<std::size_t>(cy) * document_.width + static_cast<std::size_t>(cx);
            if (visited[texel]) continue;
            visited[texel] = 1U;
            const std::size_t offset = texel * bpp;
            if (!std::equal(target.begin(), target.begin() + static_cast<std::ptrdiff_t>(bpp),
                            layer->pixels.begin() + static_cast<std::ptrdiff_t>(offset))) continue;
            std::copy_n(replacement.begin(), bpp, layer->pixels.begin() + static_cast<std::ptrdiff_t>(offset));
            queue.emplace(cx - 1, cy); queue.emplace(cx + 1, cy);
            queue.emplace(cx, cy - 1); queue.emplace(cx, cy + 1);
        }
        return true;
    }, error);
}

bool SpritePixelArtSession::set_selection(SpritePixelRect rect, std::string* error) {
    if (rect.empty()) return fail(error, "pixel-art selection is empty");
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    if (rect.x < 0 || rect.y < 0 || right > document_.width || bottom > document_.height)
        return fail(error, "pixel-art selection exceeds the canvas");
    selection_ = rect;
    return true;
}

bool SpritePixelArtSession::move_selection(std::int32_t dx, std::int32_t dy, bool copy,
                                           std::string* error) {
    return mutate(copy ? "Copy selection" : "Move selection", [&](std::string* nested) {
        if (!selection_) return fail(nested, "no pixel-art selection is active");
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        const auto rect = *selection_;
        SpritePixelRect destination{rect.x + dx, rect.y + dy, rect.width, rect.height};
        if (destination.x < 0 || destination.y < 0 ||
            static_cast<std::int64_t>(destination.x) + destination.width > document_.width ||
            static_cast<std::int64_t>(destination.y) + destination.height > document_.height)
            return fail(nested, "moved pixel-art selection exceeds the canvas");
        const std::size_t bpp = bytes_per_pixel();
        std::vector<std::byte> captured(static_cast<std::size_t>(rect.width) * rect.height * bpp);
        for (std::uint32_t row = 0; row < rect.height; ++row)
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                const std::size_t source = pixel_offset(document_.width,
                    rect.x + static_cast<std::int32_t>(column), rect.y + static_cast<std::int32_t>(row), bpp);
                const std::size_t target = (static_cast<std::size_t>(row) * rect.width + column) * bpp;
                std::copy_n(layer->pixels.begin() + static_cast<std::ptrdiff_t>(source), bpp,
                            captured.begin() + static_cast<std::ptrdiff_t>(target));
            }
        if (!copy) {
            const std::byte clearIndex = document_.storage == SpritePixelStorage::Indexed8 && document_.palette.transparentIndex
                ? static_cast<std::byte>(*document_.palette.transparentIndex) : std::byte{0};
            for (std::uint32_t row = 0; row < rect.height; ++row)
                for (std::uint32_t column = 0; column < rect.width; ++column) {
                    const std::size_t source = pixel_offset(document_.width,
                        rect.x + static_cast<std::int32_t>(column), rect.y + static_cast<std::int32_t>(row), bpp);
                    std::fill_n(layer->pixels.begin() + static_cast<std::ptrdiff_t>(source), bpp, std::byte{0});
                    if (bpp == 1U) layer->pixels[source] = clearIndex;
                }
        }
        for (std::uint32_t row = 0; row < rect.height; ++row)
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                const std::size_t destinationOffset = pixel_offset(document_.width,
                    destination.x + static_cast<std::int32_t>(column),
                    destination.y + static_cast<std::int32_t>(row), bpp);
                const std::size_t capturedOffset = (static_cast<std::size_t>(row) * rect.width + column) * bpp;
                std::copy_n(captured.begin() + static_cast<std::ptrdiff_t>(capturedOffset), bpp,
                            layer->pixels.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
            }
        selection_ = destination;
        return true;
    }, error);
}

bool SpritePixelArtSession::flip_selection_x(std::string* error) {
    return mutate("Flip selection horizontally", [&](std::string* nested) {
        if (!selection_) return fail(nested, "no pixel-art selection is active");
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        const auto rect = *selection_;
        const std::size_t bpp = bytes_per_pixel();
        for (std::uint32_t row = 0; row < rect.height; ++row)
            for (std::uint32_t column = 0; column < rect.width / 2U; ++column) {
                const std::size_t left = pixel_offset(document_.width, rect.x + static_cast<std::int32_t>(column),
                                                       rect.y + static_cast<std::int32_t>(row), bpp);
                const std::size_t right = pixel_offset(document_.width,
                    rect.x + static_cast<std::int32_t>(rect.width - 1U - column),
                    rect.y + static_cast<std::int32_t>(row), bpp);
                for (std::size_t byte = 0; byte < bpp; ++byte) std::swap(layer->pixels[left + byte], layer->pixels[right + byte]);
            }
        return true;
    }, error);
}

bool SpritePixelArtSession::flip_selection_y(std::string* error) {
    return mutate("Flip selection vertically", [&](std::string* nested) {
        if (!selection_) return fail(nested, "no pixel-art selection is active");
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        const auto rect = *selection_;
        const std::size_t bpp = bytes_per_pixel();
        for (std::uint32_t row = 0; row < rect.height / 2U; ++row)
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                const std::size_t top = pixel_offset(document_.width, rect.x + static_cast<std::int32_t>(column),
                                                      rect.y + static_cast<std::int32_t>(row), bpp);
                const std::size_t bottom = pixel_offset(document_.width, rect.x + static_cast<std::int32_t>(column),
                    rect.y + static_cast<std::int32_t>(rect.height - 1U - row), bpp);
                for (std::size_t byte = 0; byte < bpp; ++byte) std::swap(layer->pixels[top + byte], layer->pixels[bottom + byte]);
            }
        return true;
    }, error);
}

bool SpritePixelArtSession::rotate_selection_90(bool clockwise, std::string* error) {
    return mutate(clockwise ? "Rotate selection clockwise" : "Rotate selection counter-clockwise",
                  [&](std::string* nested) {
        if (!selection_) return fail(nested, "no pixel-art selection is active");
        const auto rect = *selection_;
        if (rect.width != rect.height) return fail(nested, "90-degree rotation currently requires a square selection");
        auto* layer = current_layer();
        if (!layer || layer->locked) return fail(nested, "selected pixel-art layer is locked");
        const std::size_t bpp = bytes_per_pixel();
        std::vector<std::byte> captured(static_cast<std::size_t>(rect.width) * rect.height * bpp);
        for (std::uint32_t row = 0; row < rect.height; ++row)
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                const std::size_t source = pixel_offset(document_.width, rect.x + static_cast<std::int32_t>(column),
                                                         rect.y + static_cast<std::int32_t>(row), bpp);
                const std::size_t target = (static_cast<std::size_t>(row) * rect.width + column) * bpp;
                std::copy_n(layer->pixels.begin() + static_cast<std::ptrdiff_t>(source), bpp,
                            captured.begin() + static_cast<std::ptrdiff_t>(target));
            }
        for (std::uint32_t row = 0; row < rect.height; ++row)
            for (std::uint32_t column = 0; column < rect.width; ++column) {
                const std::uint32_t sourceX = clockwise ? row : rect.width - 1U - row;
                const std::uint32_t sourceY = clockwise ? rect.height - 1U - column : column;
                const std::size_t source = (static_cast<std::size_t>(sourceY) * rect.width + sourceX) * bpp;
                const std::size_t target = pixel_offset(document_.width, rect.x + static_cast<std::int32_t>(column),
                                                         rect.y + static_cast<std::int32_t>(row), bpp);
                std::copy_n(captured.begin() + static_cast<std::ptrdiff_t>(source), bpp,
                            layer->pixels.begin() + static_cast<std::ptrdiff_t>(target));
            }
        return true;
    }, error);
}

bool SpritePixelArtSession::remap_index(std::uint8_t from, std::uint8_t to, bool allFrames,
                                        std::string* error) {
    return mutate("Remap palette index", [&](std::string* nested) {
        if (document_.storage != SpritePixelStorage::Indexed8)
            return fail(nested, "palette index remapping requires an indexed document");
        const std::size_t begin = allFrames ? 0U : selectedFrame_;
        const std::size_t end = allFrames ? document_.frames.size() : selectedFrame_ + 1U;
        for (std::size_t frame = begin; frame < end; ++frame)
            for (auto& layer : document_.frames[frame].layers)
                for (auto& pixel : layer.pixels)
                    if (static_cast<std::uint8_t>(pixel) == from) pixel = static_cast<std::byte>(to);
        return true;
    }, error);
}

bool SpritePixelArtSession::composite_rgba8(std::size_t frameIndex, std::vector<std::byte>& out,
                                            std::string* error) const {
    if (frameIndex >= document_.frames.size()) return fail(error, "pixel-art frame index is out of range");
    const std::size_t texels = static_cast<std::size_t>(document_.width) * document_.height;
    out.assign(texels * 4U, std::byte{0});
    for (const auto& layer : document_.frames[frameIndex].layers) {
        if (!layer.visible || layer.opacity == 0U) continue;
        for (std::size_t texel = 0; texel < texels; ++texel) {
            SpriteColor8 destination{static_cast<std::uint8_t>(out[texel * 4U + 0U]),
                                     static_cast<std::uint8_t>(out[texel * 4U + 1U]),
                                     static_cast<std::uint8_t>(out[texel * 4U + 2U]),
                                     static_cast<std::uint8_t>(out[texel * 4U + 3U])};
            blend_color(read_layer_color(document_, layer, texel), layer.opacity, layer.blend, destination);
            out[texel * 4U + 0U] = static_cast<std::byte>(destination.r);
            out[texel * 4U + 1U] = static_cast<std::byte>(destination.g);
            out[texel * 4U + 2U] = static_cast<std::byte>(destination.b);
            out[texel * 4U + 3U] = static_cast<std::byte>(destination.a);
        }
    }
    return true;
}

bool SpritePixelArtSession::composite_index8(std::size_t frameIndex, std::vector<std::byte>& out,
                                             std::string* error) const {
    if (document_.storage != SpritePixelStorage::Indexed8)
        return fail(error, "indexed composition requires an indexed pixel-art document");
    if (frameIndex >= document_.frames.size()) return fail(error, "pixel-art frame index is out of range");
    const std::size_t texels = static_cast<std::size_t>(document_.width) * document_.height;
    const std::uint8_t transparent = document_.palette.transparentIndex
        ? static_cast<std::uint8_t>(*document_.palette.transparentIndex) : 0U;
    out.assign(texels, static_cast<std::byte>(transparent));
    for (const auto& layer : document_.frames[frameIndex].layers) {
        if (!layer.visible || layer.opacity < 128U) continue;
        for (std::size_t texel = 0; texel < texels; ++texel) {
            const std::uint8_t index = static_cast<std::uint8_t>(layer.pixels[texel]);
            if (index != transparent) out[texel] = static_cast<std::byte>(index);
        }
    }
    return true;
}

bool SpritePixelArtSession::onion_rgba8(std::size_t frameIndex, bool previous, bool next,
                                        std::vector<std::byte>& out, std::string* error) const {
    if (!composite_rgba8(frameIndex, out, error)) return false;
    auto overlay = [&](std::size_t index, SpriteColor8 tint) {
        std::vector<std::byte> image;
        if (!composite_rgba8(index, image, nullptr)) return;
        for (std::size_t pixel = 0; pixel < image.size() / 4U; ++pixel) {
            const std::uint8_t alpha = static_cast<std::uint8_t>(image[pixel * 4U + 3U]);
            if (alpha == 0U) continue;
            SpriteColor8 destination{static_cast<std::uint8_t>(out[pixel * 4U + 0U]),
                                     static_cast<std::uint8_t>(out[pixel * 4U + 1U]),
                                     static_cast<std::uint8_t>(out[pixel * 4U + 2U]),
                                     static_cast<std::uint8_t>(out[pixel * 4U + 3U])};
            tint.a = static_cast<std::uint8_t>(alpha / 3U);
            blend_color(tint, 255U, SpritePixelBlendMode::Normal, destination);
            out[pixel * 4U + 0U] = static_cast<std::byte>(destination.r);
            out[pixel * 4U + 1U] = static_cast<std::byte>(destination.g);
            out[pixel * 4U + 2U] = static_cast<std::byte>(destination.b);
            out[pixel * 4U + 3U] = static_cast<std::byte>(destination.a);
        }
    };
    if (previous && frameIndex > 0U) overlay(frameIndex - 1U, {255U, 64U, 64U, 96U});
    if (next && frameIndex + 1U < document_.frames.size()) overlay(frameIndex + 1U, {64U, 128U, 255U, 96U});
    return true;
}

bool SpritePixelArtSession::publish(const SpritePixelArtPublishSettings& settings,
                                    SpritePixelArtPublishResult& out,
                                    std::string* error) const {
    if (!document_.validate(error)) return false;
    if (settings.imagePath.empty() || settings.spritePath.empty())
        return fail(error, "pixel-art publication requires image and sprite output paths");
    const std::uint32_t frameCount = static_cast<std::uint32_t>(document_.frames.size());
    std::uint32_t columns = settings.columns;
    if (columns == 0U) columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(frameCount))));
    columns = std::max(1U, std::min(columns, frameCount));
    const std::uint32_t rows = (frameCount + columns - 1U) / columns;
    const std::uint32_t sheetWidth = document_.width * columns;
    const std::uint32_t sheetHeight = document_.height * rows;
    if (sheetWidth > kMaximumPixelDimension || sheetHeight > kMaximumPixelDimension)
        return fail(error, "published pixel-art sheet exceeds the 16384-pixel PNG bound");
    std::vector<std::byte> sheet(static_cast<std::size_t>(sheetWidth) * sheetHeight * 4U, std::byte{0});
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        std::vector<std::byte> rgba;
        if (document_.storage == SpritePixelStorage::Indexed8 && settings.indexedTransport) {
            std::vector<std::byte> indices;
            if (!composite_index8(frame, indices, error) ||
                !encode_sprite_indices_rgba8(indices, document_.palette, rgba, error)) return false;
        } else if (!composite_rgba8(frame, rgba, error)) return false;
        const std::uint32_t originX = (frame % columns) * document_.width;
        const std::uint32_t originY = (frame / columns) * document_.height;
        for (std::uint32_t row = 0; row < document_.height; ++row) {
            const std::size_t source = static_cast<std::size_t>(row) * document_.width * 4U;
            const std::size_t destination = (static_cast<std::size_t>(originY + row) * sheetWidth + originX) * 4U;
            std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(source),
                        static_cast<std::size_t>(document_.width) * 4U,
                        sheet.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }
    std::vector<std::byte> encoded;
    if (!encode_png_rgba8(sheetWidth, sheetHeight, sheet, encoded, error) ||
        !write_file_atomic(settings.imagePath, encoded, error)) return false;
    if (document_.storage == SpritePixelStorage::Indexed8 && !settings.palettePath.empty() &&
        !write_dvepalette(settings.palettePath, document_.palette, error)) return false;

    SpriteAsset sprite = make_sprite_authoring_asset(document_.name,
        settings.imageAssetReference.empty() ? settings.imagePath.filename().generic_string()
                                             : settings.imageAssetReference,
        sheetWidth, sheetHeight, settings.pixelsPerWorldUnit);
    sprite.frames.clear();
    sprite.clips.clear();
    if (document_.storage == SpritePixelStorage::Indexed8 && settings.indexedTransport) {
        sprite.paletteAsset = settings.paletteAssetReference.empty()
            ? settings.palettePath.filename().generic_string() : settings.paletteAssetReference;
    }
    SpriteClip clip;
    clip.name = settings.clipName.empty() ? "default" : settings.clipName;
    clip.loopMode = SpriteLoopMode::Loop;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        SpriteFrame outputFrame;
        outputFrame.name = document_.frames[frame].name;
        outputFrame.atlasRect = {(frame % columns) * document_.width,
                                 (frame / columns) * document_.height,
                                 document_.width, document_.height};
        outputFrame.sourceWidth = document_.width;
        outputFrame.sourceHeight = document_.height;
        outputFrame.pivotPixels = {static_cast<float>(document_.width) * 0.5F, 0.0F};
        outputFrame.durationSeconds = document_.frames[frame].durationSeconds;
        sprite.frames.push_back(std::move(outputFrame));
        clip.frames.push_back(frame);
    }
    sprite.clips.push_back(std::move(clip));
    sprite.recompute_hash();
    if (!write_dvesprite(settings.spritePath, sprite, error)) return false;
    std::uint64_t imageHash = 1469598103934665603ULL;
    hash_bytes(imageHash, encoded);
    out = {std::move(sprite), sheetWidth, sheetHeight, imageHash, document_.contentHash};
    return true;
}

bool SpritePixelArtSession::undo() noexcept {
    if (undo_.empty()) return false;
    Snapshot current{"Redo", document_, selectedFrame_, selectedLayer_, selection_};
    Snapshot previous = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(std::move(current));
    restore(previous);
    return true;
}

bool SpritePixelArtSession::redo() noexcept {
    if (redo_.empty()) return false;
    Snapshot current{"Undo", document_, selectedFrame_, selectedLayer_, selection_};
    Snapshot next = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(std::move(current));
    restore(next);
    return true;
}

bool write_dvepixel(const std::filesystem::path& path,
                    const SpritePixelArtDocument& source,
                    std::string* error) {
    SpritePixelArtDocument document = source;
    document.recompute_hash();
    if (!document.validate(error)) return false;
    const auto temp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "could not open temporary pixel-art document");
    stream.write(kPixelMagic.data(), static_cast<std::streamsize>(kPixelMagic.size()));
    if (!write_scalar(stream, kPixelFormatVersion) || !write_string(stream, document.name) ||
        !write_scalar(stream, document.storage) || !write_scalar(stream, document.width) ||
        !write_scalar(stream, document.height)) return fail(error, "could not write pixel-art header");
    const std::filesystem::path paletteTemp = temp + ".palette";
    if (!write_dvepalette(paletteTemp, document.palette, error)) return false;
    std::ifstream paletteStream(paletteTemp, std::ios::binary | std::ios::ate);
    if (!paletteStream) return fail(error, "could not reopen temporary palette");
    const auto paletteSize = paletteStream.tellg();
    paletteStream.seekg(0);
    std::vector<char> paletteBytes(static_cast<std::size_t>(paletteSize));
    paletteStream.read(paletteBytes.data(), paletteSize);
    paletteStream.close();
    std::filesystem::remove(paletteTemp, ec);
    const auto paletteCount = static_cast<std::uint32_t>(paletteBytes.size());
    if (!write_scalar(stream, paletteCount)) return fail(error, "could not write pixel-art palette size");
    stream.write(paletteBytes.data(), static_cast<std::streamsize>(paletteBytes.size()));
    const auto frameCount = static_cast<std::uint32_t>(document.frames.size());
    if (!write_scalar(stream, frameCount)) return fail(error, "could not write pixel-art frame count");
    for (const auto& frame : document.frames) {
        if (!write_string(stream, frame.name) || !write_scalar(stream, frame.durationSeconds))
            return fail(error, "could not write pixel-art frame metadata");
        const auto layerCount = static_cast<std::uint32_t>(frame.layers.size());
        if (!write_scalar(stream, layerCount)) return fail(error, "could not write pixel-art layer count");
        for (const auto& layer : frame.layers) {
            if (!write_scalar(stream, layer.id) || !write_string(stream, layer.name) ||
                !write_scalar(stream, layer.visible) || !write_scalar(stream, layer.locked) ||
                !write_scalar(stream, layer.opacity) || !write_scalar(stream, layer.blend))
                return fail(error, "could not write pixel-art layer metadata");
            const auto pixelBytes = static_cast<std::uint64_t>(layer.pixels.size());
            if (!write_scalar(stream, pixelBytes)) return fail(error, "could not write pixel-art layer size");
            stream.write(reinterpret_cast<const char*>(layer.pixels.data()),
                         static_cast<std::streamsize>(layer.pixels.size()));
        }
    }
    if (!write_scalar(stream, document.contentHash)) return fail(error, "could not write pixel-art hash");
    stream.close();
    if (!stream) return fail(error, "could not finish pixel-art document");
    return atomic_replace(temp, path, error);
}

bool read_dvepixel(const std::filesystem::path& path,
                   SpritePixelArtDocument& document,
                   std::string* error,
                   std::uint64_t maximumBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) return fail(error, "pixel-art document is missing or exceeds the read limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "could not open pixel-art document");
    std::array<char, 8> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t version{};
    if (!stream || magic != kPixelMagic || !read_scalar(stream, version) || version != kPixelFormatVersion)
        return fail(error, "pixel-art document has an unsupported header");
    SpritePixelArtDocument replacement;
    if (!read_string(stream, replacement.name) || !read_scalar(stream, replacement.storage) ||
        !read_scalar(stream, replacement.width) || !read_scalar(stream, replacement.height))
        return fail(error, "pixel-art document header is truncated");
    std::uint32_t paletteSize{};
    if (!read_scalar(stream, paletteSize) || paletteSize > 4U * 1024U * 1024U)
        return fail(error, "pixel-art embedded palette size is invalid");
    std::vector<char> paletteBytes(paletteSize);
    stream.read(paletteBytes.data(), static_cast<std::streamsize>(paletteSize));
    if (!stream) return fail(error, "pixel-art embedded palette is truncated");
    const auto paletteTemp = path.string() + ".read.palette.tmp";
    {
        std::ofstream paletteStream(paletteTemp, std::ios::binary | std::ios::trunc);
        paletteStream.write(paletteBytes.data(), static_cast<std::streamsize>(paletteBytes.size()));
    }
    auto paletteRead = read_dvepalette(paletteTemp);
    std::filesystem::remove(paletteTemp, ec);
    if (!paletteRead) return fail(error, "pixel-art embedded palette is invalid: " + paletteRead.error);
    replacement.palette = std::move(paletteRead.asset);
    std::uint32_t frameCount{};
    if (!read_scalar(stream, frameCount) || frameCount == 0U || frameCount > kMaximumPixelFrames)
        return fail(error, "pixel-art frame count is invalid");
    const std::size_t bpp = replacement.storage == SpritePixelStorage::Indexed8 ? 1U : 4U;
    const std::uint64_t expected = static_cast<std::uint64_t>(replacement.width) * replacement.height * bpp;
    for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        SpritePixelFrame frame;
        if (!read_string(stream, frame.name) || !read_scalar(stream, frame.durationSeconds))
            return fail(error, "pixel-art frame metadata is truncated");
        std::uint32_t layerCount{};
        if (!read_scalar(stream, layerCount) || layerCount == 0U || layerCount > kMaximumPixelLayers)
            return fail(error, "pixel-art layer count is invalid");
        for (std::uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
            SpritePixelLayer layer;
            if (!read_scalar(stream, layer.id) || !read_string(stream, layer.name) ||
                !read_scalar(stream, layer.visible) || !read_scalar(stream, layer.locked) ||
                !read_scalar(stream, layer.opacity) || !read_scalar(stream, layer.blend))
                return fail(error, "pixel-art layer metadata is truncated");
            std::uint64_t bytes{};
            if (!read_scalar(stream, bytes) || bytes != expected || bytes > maximumBytes)
                return fail(error, "pixel-art layer payload has an invalid size");
            layer.pixels.resize(static_cast<std::size_t>(bytes));
            stream.read(reinterpret_cast<char*>(layer.pixels.data()), static_cast<std::streamsize>(bytes));
            if (!stream) return fail(error, "pixel-art layer payload is truncated");
            frame.layers.push_back(std::move(layer));
        }
        replacement.frames.push_back(std::move(frame));
    }
    std::uint64_t storedHash{};
    if (!read_scalar(stream, storedHash)) return fail(error, "pixel-art document hash is missing");
    replacement.recompute_hash();
    if (replacement.contentHash != storedHash) return fail(error, "pixel-art document hash does not match its contents");
    if (!replacement.validate(error)) return false;
    document = std::move(replacement);
    return true;
}

} // namespace dve::editor
