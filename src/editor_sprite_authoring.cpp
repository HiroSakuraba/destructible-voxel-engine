#include "dve/editor_sprite_authoring.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <setjmp.h>
#include <sstream>
#include <system_error>
#include <utility>

#include <jpeglib.h>
#include <png.h>

namespace dve::editor {
namespace {

constexpr std::uint32_t kMaximumSpriteDimension = 16384U;
constexpr std::uint64_t kMaximumEncodedBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumDecodedBytes = 1024ULL * 1024ULL * 1024ULL;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

[[nodiscard]] std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1U) return 1U;
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

struct JpegErrorState {
    jpeg_error_mgr manager{};
    jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr common) {
    auto* state = reinterpret_cast<JpegErrorState*>(common->err);
    (*common->err->format_message)(common, state->message);
    longjmp(state->jump, 1);
}

bool decode_png(std::span<const std::byte> encoded, SpriteDecodedImage& out,
                std::string* error) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) {
        return fail(error, "PNG header decode failed: " + std::string(image.message));
    }
    if (image.width == 0U || image.height == 0U || image.width > kMaximumSpriteDimension ||
        image.height > kMaximumSpriteDimension) {
        png_image_free(&image);
        return fail(error, "PNG dimensions exceed sprite authoring limits");
    }
    image.format = PNG_FORMAT_RGBA;
    const png_alloc_size_t bytes = PNG_IMAGE_SIZE(image);
    if (bytes > kMaximumDecodedBytes) {
        png_image_free(&image);
        return fail(error, "decoded PNG exceeds sprite authoring memory limits");
    }
    out.width = image.width;
    out.height = image.height;
    out.mimeType = "image/png";
    out.rgba8.resize(static_cast<std::size_t>(bytes));
    if (!png_image_finish_read(&image, nullptr, out.rgba8.data(), 0, nullptr)) {
        const std::string message = image.message;
        png_image_free(&image);
        out = {};
        return fail(error, "PNG pixel decode failed: " + message);
    }
    png_image_free(&image);
    return true;
}

bool decode_jpeg(std::span<const std::byte> encoded, SpriteDecodedImage& out,
                 std::string* error) {
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        return fail(error, "JPEG input exceeds decoder range");
    }
    jpeg_decompress_struct decoder{};
    JpegErrorState jpegError{};
    decoder.err = jpeg_std_error(&jpegError.manager);
    jpegError.manager.error_exit = jpeg_error_exit;
    if (setjmp(jpegError.jump) != 0) {
        jpeg_destroy_decompress(&decoder);
        out = {};
        return fail(error, "JPEG decode failed: " + std::string(jpegError.message));
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder,
                 reinterpret_cast<const unsigned char*>(encoded.data()),
                 static_cast<unsigned long>(encoded.size()));
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (decoder.output_width == 0U || decoder.output_height == 0U ||
        decoder.output_width > kMaximumSpriteDimension ||
        decoder.output_height > kMaximumSpriteDimension) {
        jpeg_destroy_decompress(&decoder);
        return fail(error, "JPEG dimensions exceed sprite authoring limits");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(decoder.output_width) *
        static_cast<std::uint64_t>(decoder.output_height) * 4ULL;
    if (bytes > kMaximumDecodedBytes || bytes > std::numeric_limits<std::size_t>::max()) {
        jpeg_destroy_decompress(&decoder);
        return fail(error, "decoded JPEG exceeds sprite authoring memory limits");
    }
    out.width = decoder.output_width;
    out.height = decoder.output_height;
    out.mimeType = "image/jpeg";
    out.rgba8.resize(static_cast<std::size_t>(bytes));
    std::vector<unsigned char> row(static_cast<std::size_t>(decoder.output_width) * 3U);
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW rowPointer = row.data();
        jpeg_read_scanlines(&decoder, &rowPointer, 1U);
        const std::size_t y = static_cast<std::size_t>(decoder.output_scanline - 1U);
        for (std::size_t x = 0U; x < decoder.output_width; ++x) {
            const std::size_t source = x * 3U;
            const std::size_t destination = (y * decoder.output_width + x) * 4U;
            out.rgba8[destination] = std::byte{row[source]};
            out.rgba8[destination + 1U] = std::byte{row[source + 1U]};
            out.rgba8[destination + 2U] = std::byte{row[source + 2U]};
            out.rgba8[destination + 3U] = std::byte{255U};
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return true;
}

void copy_pixel(const SpriteSourceImageView& source, std::uint32_t sourceX,
                std::uint32_t sourceY, SpriteAtlasPackResult& destination,
                std::uint32_t destinationX, std::uint32_t destinationY) {
    const std::size_t sourceOffset =
        (static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4U;
    const std::size_t destinationOffset =
        (static_cast<std::size_t>(destinationY) * destination.width + destinationX) * 4U;
    std::copy_n(source.rgba8.data() + static_cast<std::ptrdiff_t>(sourceOffset), 4U,
                destination.rgba8.data() + static_cast<std::ptrdiff_t>(destinationOffset));
}

[[nodiscard]] const SpriteClip* find_clip(const SpriteAsset& asset,
                                          std::string_view name) noexcept {
    const auto found = std::find_if(asset.clips.begin(), asset.clips.end(),
        [name](const SpriteClip& clip) { return clip.name == name; });
    return found == asset.clips.end() ? nullptr : &*found;
}

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]] std::uint64_t hash_bytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u64(hash, static_cast<std::uint64_t>(bytes.size()));
    for (const std::byte value : bytes) hash_byte(hash, std::to_integer<std::uint8_t>(value));
    return hash;
}

[[nodiscard]] bool encode_png_rgba8(std::uint32_t width, std::uint32_t height,
                                    std::span<const std::byte> rgba8,
                                    std::vector<std::byte>& encoded,
                                    std::string* error) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (width == 0U || height == 0U || expected != rgba8.size()) {
        return fail(error, "PNG encode dimensions do not match the RGBA8 payload");
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t encodedSize = 0U;
    if (png_image_write_to_memory(&image, nullptr, &encodedSize, 0,
                                  rgba8.data(), 0, nullptr) == 0) {
        return fail(error, "PNG size query failed: " + std::string(image.message));
    }
    if (encodedSize == 0U || encodedSize > kMaximumEncodedBytes) {
        return fail(error, "encoded sprite atlas exceeds the PNG output limit");
    }
    encoded.resize(static_cast<std::size_t>(encodedSize));
    if (png_image_write_to_memory(&image, encoded.data(), &encodedSize, 0,
                                  rgba8.data(), 0, nullptr) == 0) {
        encoded.clear();
        return fail(error, "PNG encode failed: " + std::string(image.message));
    }
    encoded.resize(static_cast<std::size_t>(encodedSize));
    return true;
}

[[nodiscard]] bool write_file_bytes(const std::filesystem::path& path,
                                    std::span<const std::byte> bytes,
                                    std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return fail(error, "could not create sprite output directory: " + ec.message());
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "could not open staged sprite output");
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) return fail(error, "could not write the complete staged sprite output");
    return true;
}

[[nodiscard]] bool write_file_text(const std::filesystem::path& path,
                                   std::string_view text,
                                   std::string* error) {
    return write_file_bytes(path,
        std::as_bytes(std::span<const char>(text.data(), text.size())), error);
}

[[nodiscard]] bool read_file_bytes(const std::filesystem::path& path,
                                   std::vector<std::byte>& bytes) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaximumEncodedBytes || size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream) || bytes.empty();
}

[[nodiscard]] bool read_file_text(const std::filesystem::path& path, std::string& text) {
    std::vector<std::byte> bytes;
    if (!read_file_bytes(path, bytes)) return false;
    text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

[[nodiscard]] std::filesystem::path sibling_work_path(const std::filesystem::path& target,
                                                       std::string_view role,
                                                       std::uint64_t key,
                                                       std::size_t index) {
    std::ostringstream suffix;
    suffix << '.' << role << '-' << std::hex << std::setw(16) << std::setfill('0') << key
           << '-' << std::dec << index;
    std::filesystem::path result = target;
    result += suffix.str();
    return result;
}

struct SpriteTransactionFile {
    std::filesystem::path target;
    std::filesystem::path stage;
    std::filesystem::path backup;
    bool hadOriginal{};
    bool installed{};
};

[[nodiscard]] bool commit_sprite_transaction(std::vector<SpriteTransactionFile>& files,
                                             std::string* error) {
    std::error_code ec;
    for (SpriteTransactionFile& file : files) {
        if (!file.target.parent_path().empty()) {
            std::filesystem::create_directories(file.target.parent_path(), ec);
            if (ec) break;
        }
        file.hadOriginal = std::filesystem::exists(file.target, ec) && !ec;
        if (ec) break;
        std::filesystem::remove(file.backup, ec);
        ec.clear();
        if (file.hadOriginal) {
            std::filesystem::rename(file.target, file.backup, ec);
            if (ec) break;
        }
        std::filesystem::rename(file.stage, file.target, ec);
        if (ec) break;
        file.installed = true;
    }
    if (ec) {
        const std::string commitError = ec.message();
        for (auto iterator = files.rbegin(); iterator != files.rend(); ++iterator) {
            std::error_code rollbackError;
            if (iterator->installed) std::filesystem::remove(iterator->target, rollbackError);
            rollbackError.clear();
            if (iterator->hadOriginal && std::filesystem::exists(iterator->backup, rollbackError)) {
                rollbackError.clear();
                std::filesystem::rename(iterator->backup, iterator->target, rollbackError);
            }
            rollbackError.clear();
            std::filesystem::remove(iterator->stage, rollbackError);
        }
        return fail(error, "sprite publication transaction failed: " + commitError);
    }
    for (SpriteTransactionFile& file : files) {
        std::filesystem::remove(file.backup, ec);
        ec.clear();
    }
    return true;
}

[[nodiscard]] bool source_file_signature(const std::filesystem::path& path,
                                         std::filesystem::file_time_type& writeTime,
                                         std::uintmax_t& size) noexcept {
    std::error_code ec;
    writeTime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    size = std::filesystem::file_size(path, ec);
    return !ec;
}

[[nodiscard]] bool looks_like_sprite_index_transport(
    const SpriteDecodedImage& image, const SpritePaletteAsset& palette) noexcept {
    if (image.width == 0U || image.height == 0U || image.rgba8.size() !=
        static_cast<std::size_t>(image.width) * image.height * 4U || palette.entry_count() == 0U) {
        return false;
    }
    for (std::size_t offset = 0U; offset < image.rgba8.size(); offset += 4U) {
        const std::uint8_t index = std::to_integer<std::uint8_t>(image.rgba8[offset]);
        const std::uint8_t green = std::to_integer<std::uint8_t>(image.rgba8[offset + 1U]);
        const std::uint8_t blue = std::to_integer<std::uint8_t>(image.rgba8[offset + 2U]);
        const std::uint8_t alpha = std::to_integer<std::uint8_t>(image.rgba8[offset + 3U]);
        if (index >= palette.entry_count() || green != 0U || blue != 0U) return false;
        const bool transparent = palette.transparentIndex && *palette.transparentIndex == index;
        if (alpha != static_cast<std::uint8_t>(transparent ? 0U : 255U)) return false;
    }
    return true;
}

[[nodiscard]] std::string cooked_texture_reference(
    const SpriteAtlasPublishSettings& settings) {
    if (!settings.textureAssetReference.empty()) return settings.textureAssetReference;
    std::filesystem::path reference = settings.atlasPath;
    if (!settings.cookedAssetPath.parent_path().empty()) {
        const std::filesystem::path relative =
            settings.atlasPath.lexically_relative(settings.cookedAssetPath.parent_path());
        if (!relative.empty()) reference = relative;
    }
    return reference.generic_string();
}

[[nodiscard]] std::string cooked_palette_reference(
    const SpriteAtlasPublishSettings& settings) {
    if (!settings.indexedPalette) return {};
    if (!settings.indexedPalette->paletteAssetReference.empty())
        return settings.indexedPalette->paletteAssetReference;
    std::filesystem::path reference = settings.indexedPalette->palettePath;
    if (!settings.cookedAssetPath.parent_path().empty()) {
        const std::filesystem::path relative = reference.lexically_relative(
            settings.cookedAssetPath.parent_path());
        if (!relative.empty()) reference = relative;
    }
    return reference.generic_string();
}

[[nodiscard]] std::string sprite_cook_manifest(
    const SpriteAtlasPublishSettings& settings,
    const SpriteAtlasPublishResult& result,
    std::uint64_t sourceAssetHash,
    std::uint64_t sourceImageHash,
    std::string_view textureReference,
    std::string_view paletteReference) {
    std::ostringstream stream;
    stream << "DVE_SPRITE_COOK 2\n"
           << "dependency_key " << result.dependencyKey << "\n"
           << "source_asset_hash " << sourceAssetHash << "\n"
           << "source_image_hash " << sourceImageHash << "\n"
           << "atlas_content_hash " << result.atlasContentHash << "\n"
           << "cooked_asset_hash " << result.cookedAssetHash << "\n"
           << "palette_content_hash " << result.paletteContentHash << "\n"
           << "atlas_dimensions " << result.atlasWidth << ' ' << result.atlasHeight << "\n"
           << "frame_count " << result.frameCount << "\n"
           << "packing " << settings.packing.maximumWidth << ' '
           << settings.packing.paddingPixels << ' '
           << (settings.packing.powerOfTwo ? 1 : 0) << ' '
           << (settings.packing.extrudeEdges ? 1 : 0) << "\n"
           << "texture_reference " << std::quoted(std::string(textureReference)) << "\n"
           << "atlas_path " << std::quoted(settings.atlasPath.generic_string()) << "\n"
           << "asset_path " << std::quoted(settings.cookedAssetPath.generic_string()) << "\n"
           << "indexed_palette " << (settings.indexedPalette ? 1 : 0) << "\n";
    if (settings.indexedPalette) {
        const SpritePaletteIndexSettings& indexing = settings.indexedPalette->indexing;
        stream << "palette_reference " << std::quoted(std::string(paletteReference)) << "\n"
               << "palette_path "
               << std::quoted(settings.indexedPalette->palettePath.generic_string()) << "\n"
               << "palette_indexing " << static_cast<unsigned>(indexing.policy) << ' '
               << indexing.bank << ' ' << static_cast<unsigned>(indexing.alphaThreshold) << ' '
               << indexing.unmatchedIndex << "\n"
               << "palette_diagnostics " << result.paletteDiagnostics.totalPixels << ' '
               << result.paletteDiagnostics.transparentPixels << ' '
               << result.paletteDiagnostics.exactPixels << ' '
               << result.paletteDiagnostics.nearestPixels << ' '
               << result.paletteDiagnostics.unmatchedPixels << ' '
               << result.paletteDiagnostics.maximumDistanceSquared << "\n";
    }
    return stream.str();
}
} // namespace

bool SpriteDecodedImage::validate(std::string* error) const {
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (width == 0U || height == 0U || width > kMaximumSpriteDimension ||
        height > kMaximumSpriteDimension || bytes != rgba8.size()) {
        return fail(error, "decoded sprite image dimensions do not match its RGBA8 payload");
    }
    return true;
}

bool decode_sprite_image(std::span<const std::byte> encoded, std::string_view mimeType,
                         SpriteDecodedImage& out, std::string* error) {
    out = {};
    if (encoded.empty() || encoded.size() > kMaximumEncodedBytes) {
        return fail(error, "encoded sprite image is empty or exceeds the input limit");
    }
    const bool pngSignature = encoded.size() >= 8U &&
        std::to_integer<std::uint8_t>(encoded[0]) == 0x89U &&
        std::to_integer<std::uint8_t>(encoded[1]) == 0x50U &&
        std::to_integer<std::uint8_t>(encoded[2]) == 0x4EU &&
        std::to_integer<std::uint8_t>(encoded[3]) == 0x47U;
    const bool jpegSignature = encoded.size() >= 2U &&
        std::to_integer<std::uint8_t>(encoded[0]) == 0xFFU &&
        std::to_integer<std::uint8_t>(encoded[1]) == 0xD8U;
    if (mimeType == "image/png" || (mimeType.empty() && pngSignature)) {
        return decode_png(encoded, out, error);
    }
    if (mimeType == "image/jpeg" || mimeType == "image/jpg" ||
        (mimeType.empty() && jpegSignature)) {
        return decode_jpeg(encoded, out, error);
    }
    return fail(error, "sprite authoring supports PNG and JPEG source images");
}

bool load_sprite_image(const std::filesystem::path& path, SpriteDecodedImage& out,
                       std::string* error) {
    std::error_code fileError;
    const std::uintmax_t size = std::filesystem::file_size(path, fileError);
    if (fileError || size == 0U || size > kMaximumEncodedBytes) {
        return fail(error, "sprite source image is missing, empty, or exceeds the input limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "could not open sprite source image");
    std::vector<std::byte> encoded(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!stream) return fail(error, "could not read the complete sprite source image");
    std::string mime;
    const std::string extension = path.extension().string();
    if (extension == ".png" || extension == ".PNG") mime = "image/png";
    else if (extension == ".jpg" || extension == ".jpeg" ||
             extension == ".JPG" || extension == ".JPEG") mime = "image/jpeg";
    if (!decode_sprite_image(encoded, mime, out, error)) return false;
    out.sourcePath = path;
    return true;
}

bool repack_sprite_atlas(const SpriteAsset& asset, const SpriteSourceImageView& source,
                         const SpriteAtlasPackSettings& settings,
                         SpriteAtlasPackResult& out, std::string* error) {
    out = {};
    std::string validationError;
    if (!asset.validate(&validationError)) return fail(error, "invalid sprite asset: " + validationError);
    if (!source.validate(&validationError)) return fail(error, validationError);
    if (source.width != asset.textureWidth || source.height != asset.textureHeight) {
        return fail(error, "source image dimensions do not match the sprite asset texture dimensions");
    }
    if (settings.maximumWidth == 0U || settings.maximumWidth > kMaximumSpriteDimension ||
        settings.paddingPixels > 64U) {
        return fail(error, "atlas packing settings are outside supported limits");
    }

    struct Placement { std::uint32_t x{}; std::uint32_t y{}; };
    std::vector<Placement> placements(asset.frames.size());
    std::uint32_t cursorX = settings.paddingPixels;
    std::uint32_t cursorY = settings.paddingPixels;
    std::uint32_t rowHeight{};
    std::uint32_t usedWidth{};
    for (std::size_t index = 0U; index < asset.frames.size(); ++index) {
        const SpriteFrame& frame = asset.frames[index];
        const std::uint32_t packedWidth = frame.atlasRect.width + settings.paddingPixels * 2U;
        const std::uint32_t packedHeight = frame.atlasRect.height + settings.paddingPixels * 2U;
        if (packedWidth > settings.maximumWidth) {
            return fail(error, "a sprite frame is wider than the configured atlas width");
        }
        if (cursorX + packedWidth > settings.maximumWidth && cursorX > settings.paddingPixels) {
            cursorX = settings.paddingPixels;
            cursorY += rowHeight;
            rowHeight = 0U;
        }
        placements[index] = {cursorX + settings.paddingPixels, cursorY + settings.paddingPixels};
        cursorX += packedWidth;
        rowHeight = std::max(rowHeight, packedHeight);
        usedWidth = std::max(usedWidth, cursorX);
    }
    std::uint32_t usedHeight = cursorY + rowHeight;
    usedWidth = std::max(1U, usedWidth);
    usedHeight = std::max(1U, usedHeight);
    if (settings.powerOfTwo) {
        usedWidth = next_power_of_two(usedWidth);
        usedHeight = next_power_of_two(usedHeight);
    }
    if (usedWidth > kMaximumSpriteDimension || usedHeight > kMaximumSpriteDimension) {
        return fail(error, "packed sprite atlas exceeds maximum texture dimensions");
    }
    const std::uint64_t byteCount = static_cast<std::uint64_t>(usedWidth) * usedHeight * 4ULL;
    if (byteCount > kMaximumDecodedBytes || byteCount > std::numeric_limits<std::size_t>::max()) {
        return fail(error, "packed sprite atlas exceeds memory limits");
    }
    out.width = usedWidth;
    out.height = usedHeight;
    out.rgba8.assign(static_cast<std::size_t>(byteCount), std::byte{0});
    out.frameRects.resize(asset.frames.size());

    for (std::size_t index = 0U; index < asset.frames.size(); ++index) {
        const SpriteFrame& frame = asset.frames[index];
        const Placement placement = placements[index];
        out.frameRects[index] = {placement.x, placement.y,
                                 frame.atlasRect.width, frame.atlasRect.height};
        for (std::uint32_t y = 0U; y < frame.atlasRect.height; ++y) {
            for (std::uint32_t x = 0U; x < frame.atlasRect.width; ++x) {
                copy_pixel(source, frame.atlasRect.x + x, frame.atlasRect.y + y,
                           out, placement.x + x, placement.y + y);
            }
        }
        if (!settings.extrudeEdges || settings.paddingPixels == 0U) continue;
        for (std::uint32_t pad = 1U; pad <= settings.paddingPixels; ++pad) {
            for (std::uint32_t x = 0U; x < frame.atlasRect.width; ++x) {
                copy_pixel(source, frame.atlasRect.x + x, frame.atlasRect.y,
                           out, placement.x + x, placement.y - pad);
                copy_pixel(source, frame.atlasRect.x + x,
                           frame.atlasRect.y + frame.atlasRect.height - 1U,
                           out, placement.x + x,
                           placement.y + frame.atlasRect.height - 1U + pad);
            }
            for (std::uint32_t y = 0U; y < frame.atlasRect.height; ++y) {
                copy_pixel(source, frame.atlasRect.x, frame.atlasRect.y + y,
                           out, placement.x - pad, placement.y + y);
                copy_pixel(source, frame.atlasRect.x + frame.atlasRect.width - 1U,
                           frame.atlasRect.y + y, out,
                           placement.x + frame.atlasRect.width - 1U + pad,
                           placement.y + y);
            }
        }
    }
    return true;
}

SpriteAuthoringWorkspace::SpriteAuthoringWorkspace() = default;
SpriteAuthoringWorkspace::SpriteAuthoringWorkspace(SpriteAsset asset)
    : session_(std::move(asset)) {
    timeline_.clip = session_.document().selectedClip;
    normalize_timeline_selection();
}

bool SpriteAuthoringWorkspace::open_asset(
    const std::filesystem::path& assetPath,
    const std::filesystem::path& projectRoot,
    std::string* error) {
    SpriteAuthoringSession loaded;
    if (!loaded.open(assetPath, error)) return false;
    const SpriteAsset& asset = loaded.asset();
    std::filesystem::path sourcePath = asset.textureAsset;
    if (!sourcePath.is_absolute()) {
        const std::filesystem::path projectCandidate = projectRoot.empty()
            ? std::filesystem::path{} : projectRoot / sourcePath;
        const std::filesystem::path assetCandidate = assetPath.parent_path() / sourcePath;
        if (!projectCandidate.empty() && std::filesystem::exists(projectCandidate)) {
            sourcePath = projectCandidate;
        } else {
            sourcePath = assetCandidate;
        }
    }
    SpriteDecodedImage image;
    if (!load_sprite_image(sourcePath, image, error)) return false;
    if (image.width != asset.textureWidth || image.height != asset.textureHeight) {
        return fail(error, "sprite source dimensions do not match the .dvesprite asset");
    }
    session_ = std::move(loaded);
    sourceImage_ = image;
    importSourceImage_ = std::move(image);
    activePackSettings_.reset();
    capture_source_frame_rects();
    timeline_ = {};
    timeline_.clip = session_.document().selectedClip;
    normalize_timeline_selection();
    reset_view();
    sourceWatchSignatureValid_ = source_file_signature(
        importSourceImage_.sourcePath, sourceWriteTime_, sourceFileSize_);
    sourceWatchElapsedSeconds_ = 0.0F;
    return true;
}

bool SpriteAuthoringWorkspace::create_from_texture(
    const std::filesystem::path& sourcePath, std::string assetName,
    std::string textureAsset, float pixelsPerWorldUnit, std::string* error) {
    SpriteDecodedImage image;
    if (!load_sprite_image(sourcePath, image, error)) return false;
    SpriteAsset asset = make_sprite_authoring_asset(
        std::move(assetName), std::move(textureAsset), image.width, image.height,
        pixelsPerWorldUnit);
    session_ = SpriteAuthoringSession(std::move(asset));
    sourceImage_ = image;
    importSourceImage_ = std::move(image);
    activePackSettings_.reset();
    capture_source_frame_rects();
    timeline_ = {};
    timeline_.clip = session_.document().selectedClip;
    normalize_timeline_selection();
    reset_view();
    sourceWatchSignatureValid_ = source_file_signature(
        importSourceImage_.sourcePath, sourceWriteTime_, sourceFileSize_);
    sourceWatchElapsedSeconds_ = 0.0F;
    return true;
}

bool SpriteAuthoringWorkspace::reimport_source(bool preserveSelection, std::string* error) {
    if (importSourceImage_.sourcePath.empty()) return fail(error, "sprite source has no reimport path");
    SpriteDecodedImage image;
    if (!load_sprite_image(importSourceImage_.sourcePath, image, error)) return false;
    return set_source_image(std::move(image), preserveSelection, error);
}

bool SpriteAuthoringWorkspace::save_asset(const std::filesystem::path& path,
                                            std::string* error) const {
    SpriteAsset serialized = session_.asset();
    if (activePackSettings_) {
        if (sourceFrameRects_.size() != serialized.frames.size() ||
            importSourceImage_.width == 0U || importSourceImage_.height == 0U) {
            return fail(error, "packed sprite source recipe is unavailable for saving");
        }
        serialized.textureWidth = importSourceImage_.width;
        serialized.textureHeight = importSourceImage_.height;
        if (!importSourceImage_.sourcePath.empty()) {
            serialized.textureAsset = importSourceImage_.sourcePath.generic_string();
        }
        for (std::size_t index = 0U; index < sourceFrameRects_.size(); ++index) {
            serialized.frames[index].atlasRect = sourceFrameRects_[index];
        }
    }
    serialized.recompute_hash();
    return write_dvesprite(path, serialized, error);
}

bool SpriteAuthoringWorkspace::set_source_image(SpriteDecodedImage image,
                                                bool preserveSelection,
                                                std::string* error) {
    if (!image.validate(error)) return false;
    const SpriteTimelineState savedTimeline = timeline_;
    const SpriteCanvasViewState savedCanvas = canvas_;
    const std::string textureAsset = image.sourcePath.empty()
        ? session_.asset().textureAsset : image.sourcePath.generic_string();

    if (activePackSettings_) {
        if (sourceFrameRects_.size() != session_.asset().frames.size()) {
            return fail(error, "packed sprite source recipe no longer matches the frame table");
        }
        SpriteAsset sourceAsset = session_.asset();
        sourceAsset.textureAsset = textureAsset;
        sourceAsset.textureWidth = image.width;
        sourceAsset.textureHeight = image.height;
        for (std::size_t index = 0U; index < sourceFrameRects_.size(); ++index) {
            sourceAsset.frames[index].atlasRect = sourceFrameRects_[index];
        }
        sourceAsset.recompute_hash();
        SpriteAtlasPackResult packed;
        if (!repack_sprite_atlas(sourceAsset, image.view(), *activePackSettings_, packed, error)) {
            return false;
        }
        std::string sourceError;
        const bool sourceChanged = session_.set_texture_source(
            textureAsset, session_.asset().textureWidth, session_.asset().textureHeight,
            &sourceError);
        // The packed dimensions are intentionally retained here; only the source reference
        // changes before the new packed layout is published. An identical reference is a valid
        // no-op during reimport.
        if (!sourceChanged && !sourceError.empty()) return fail(error, std::move(sourceError));
        std::string layoutError;
        const bool layoutChanged = session_.set_atlas_layout(
            packed.width, packed.height, packed.frameRects, &layoutError);
        if (!layoutChanged && !layoutError.empty()) return fail(error, std::move(layoutError));
        importSourceImage_ = std::move(image);
        sourceImage_.sourcePath = importSourceImage_.sourcePath;
        sourceImage_.mimeType = "image/raw-rgba8";
        sourceImage_.width = packed.width;
        sourceImage_.height = packed.height;
        sourceImage_.rgba8 = std::move(packed.rgba8);
    } else {
        std::string sourceError;
        const bool assetChanged = session_.set_texture_source(
            textureAsset, image.width, image.height, &sourceError);
        // Reimporting identical dimensions and the same path is a valid pixel refresh even though
        // the serialized SpriteAsset does not change and the session reports a no-op mutation.
        if (!assetChanged && !sourceError.empty()) return fail(error, std::move(sourceError));
        sourceImage_ = image;
        importSourceImage_ = std::move(image);
        capture_source_frame_rects();
    }
    if (!preserveSelection) {
        timeline_ = {};
        canvas_ = {};
    } else {
        timeline_ = savedTimeline;
        canvas_ = savedCanvas;
    }
    normalize_timeline_selection();
    sourceWatchSignatureValid_ = source_file_signature(
        importSourceImage_.sourcePath, sourceWriteTime_, sourceFileSize_);
    sourceWatchElapsedSeconds_ = 0.0F;
    return true;
}

bool SpriteAuthoringWorkspace::slice_grid(const SpriteGridSliceSettings& settings,
                                          std::string* error) {
    if (!has_source_image()) return fail(error, "load a sprite source image before slicing");
    if (!restore_unpacked_source(error)) return false;
    if (!session_.slice_grid(sourceImage_.view(), settings, error)) return false;
    capture_source_frame_rects();
    timeline_.clip = session_.document().selectedClip;
    timeline_.selectedSequenceIndices.clear();
    timeline_.primarySequenceIndex.reset();
    normalize_timeline_selection();
    return true;
}

bool SpriteAuthoringWorkspace::slice_freeform(std::span<const SpriteSliceSpec> slices,
                                              bool trimTransparent,
                                              std::uint8_t alphaThreshold,
                                              bool skipTransparent,
                                              std::string* error) {
    if (!has_source_image()) return fail(error, "load a sprite source image before slicing");
    if (!restore_unpacked_source(error)) return false;
    if (!session_.replace_slices(sourceImage_.view(), slices, trimTransparent,
                                 alphaThreshold, skipTransparent, error)) return false;
    capture_source_frame_rects();
    timeline_.clip = session_.document().selectedClip;
    timeline_.selectedSequenceIndices.clear();
    timeline_.primarySequenceIndex.reset();
    normalize_timeline_selection();
    return true;
}

void SpriteAuthoringWorkspace::zoom_at(float factor, SpriteVec2 canvasPoint) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0F ||
        !std::isfinite(canvasPoint.x) || !std::isfinite(canvasPoint.y)) return;
    const float oldZoom = canvas_.zoom;
    const float newZoom = std::clamp(oldZoom * factor, 0.125F, 128.0F);
    const float ratio = newZoom / oldZoom;
    canvas_.panPixels.x = canvasPoint.x - (canvasPoint.x - canvas_.panPixels.x) * ratio;
    canvas_.panPixels.y = canvasPoint.y - (canvasPoint.y - canvas_.panPixels.y) * ratio;
    canvas_.zoom = newZoom;
}

void SpriteAuthoringWorkspace::pan_by(SpriteVec2 deltaPixels) noexcept {
    if (!std::isfinite(deltaPixels.x) || !std::isfinite(deltaPixels.y)) return;
    canvas_.panPixels.x += deltaPixels.x;
    canvas_.panPixels.y += deltaPixels.y;
}

void SpriteAuthoringWorkspace::reset_view() noexcept {
    canvas_.zoom = 8.0F;
    canvas_.panPixels = {};
}

bool SpriteAuthoringWorkspace::select_timeline_frames(
    std::span<const std::size_t> sequenceIndices,
    std::optional<std::size_t> primary) noexcept {
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr) return false;
    std::vector<std::size_t> selected(sequenceIndices.begin(), sequenceIndices.end());
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (std::any_of(selected.begin(), selected.end(),
                    [clip](std::size_t index) { return index >= clip->frames.size(); })) return false;
    if (primary && (std::find(selected.begin(), selected.end(), *primary) == selected.end())) return false;
    timeline_.selectedSequenceIndices = std::move(selected);
    timeline_.primarySequenceIndex = primary;
    if (!timeline_.primarySequenceIndex && !timeline_.selectedSequenceIndices.empty()) {
        timeline_.primarySequenceIndex = timeline_.selectedSequenceIndices.front();
    }
    if (timeline_.primarySequenceIndex) {
        static_cast<void>(session_.select_frame(clip->frames[*timeline_.primarySequenceIndex]));
    }
    return true;
}

bool SpriteAuthoringWorkspace::reorder_timeline_selection(std::size_t destinationIndex,
                                                          std::string* error) {
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || timeline_.selectedSequenceIndices.empty() ||
        destinationIndex > clip->frames.size()) {
        return fail(error, "timeline selection or destination is invalid");
    }
    std::vector<std::size_t> order;
    order.reserve(clip->frames.size());
    for (std::size_t index = 0U; index < clip->frames.size(); ++index) {
        if (std::find(timeline_.selectedSequenceIndices.begin(),
                      timeline_.selectedSequenceIndices.end(), index) ==
            timeline_.selectedSequenceIndices.end()) {
            order.push_back(index);
        }
    }
    std::size_t removedBeforeDestination{};
    for (const std::size_t index : timeline_.selectedSequenceIndices) {
        if (index < destinationIndex) ++removedBeforeDestination;
    }
    const std::size_t insertion = destinationIndex - std::min(destinationIndex, removedBeforeDestination);
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertion),
                 timeline_.selectedSequenceIndices.begin(),
                 timeline_.selectedSequenceIndices.end());
    if (!session_.reorder_clip_timeline(timeline_.clip, order, error)) return false;
    timeline_.selectedSequenceIndices.resize(timeline_.selectedSequenceIndices.size());
    for (std::size_t index = 0U; index < timeline_.selectedSequenceIndices.size(); ++index) {
        timeline_.selectedSequenceIndices[index] = insertion + index;
    }
    timeline_.primarySequenceIndex = insertion;
    normalize_timeline_selection();
    return true;
}

bool SpriteAuthoringWorkspace::set_selected_frame_duration(float seconds,
                                                           std::string* error) {
    if (!std::isfinite(seconds) || seconds <= 0.0F) return fail(error, "frame duration must be positive");
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || timeline_.selectedSequenceIndices.empty()) return fail(error, "no timeline frames are selected");
    for (const std::size_t sequenceIndex : timeline_.selectedSequenceIndices) {
        const SpriteFrameIndex frame = clip->frames[sequenceIndex];
        const SpriteFrame& current = session_.asset().frames[frame];
        if (!session_.set_frame_timing_event(frame, seconds, current.event, error)) return false;
        clip = selected_clip();
    }
    return true;
}

bool SpriteAuthoringWorkspace::set_selected_frame_event(std::string event,
                                                        std::string* error) {
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || timeline_.selectedSequenceIndices.empty()) return fail(error, "no timeline frames are selected");
    for (const std::size_t sequenceIndex : timeline_.selectedSequenceIndices) {
        const SpriteFrameIndex frame = clip->frames[sequenceIndex];
        const SpriteFrame& current = session_.asset().frames[frame];
        if (!session_.set_frame_timing_event(frame, current.durationSeconds, event, error)) return false;
        clip = selected_clip();
    }
    return true;
}

bool SpriteAuthoringWorkspace::set_selected_frame_pivot(SpriteVec2 pivotPixels,
                                                        std::string* error) {
    if (!std::isfinite(pivotPixels.x) || !std::isfinite(pivotPixels.y)) return fail(error, "pivot must be finite");
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || timeline_.selectedSequenceIndices.empty()) return fail(error, "no timeline frames are selected");
    for (const std::size_t sequenceIndex : timeline_.selectedSequenceIndices) {
        if (!session_.set_frame_pivot(clip->frames[sequenceIndex], pivotPixels, error)) return false;
        clip = selected_clip();
    }
    return true;
}

bool SpriteAuthoringWorkspace::inset_selected_frame_trim(int pixels,
                                                           std::string* error) {
    if (pixels == 0) return true;
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || timeline_.selectedSequenceIndices.empty()) {
        return fail(error, "no timeline frames are selected");
    }
    for (const std::size_t sequenceIndex : timeline_.selectedSequenceIndices) {
        const SpriteFrameIndex frameIndex = clip->frames[sequenceIndex];
        SpriteFrame frame = session_.asset().frames[frameIndex];
        const std::int64_t inset = pixels;
        const std::int64_t x = static_cast<std::int64_t>(frame.atlasRect.x) + inset;
        const std::int64_t y = static_cast<std::int64_t>(frame.atlasRect.y) + inset;
        const std::int64_t width = static_cast<std::int64_t>(frame.atlasRect.width) - 2 * inset;
        const std::int64_t height = static_cast<std::int64_t>(frame.atlasRect.height) - 2 * inset;
        const std::int64_t offsetX = static_cast<std::int64_t>(frame.sourceOffsetX) + inset;
        const std::int64_t offsetY = static_cast<std::int64_t>(frame.sourceOffsetY) + inset;
        if (x < 0 || y < 0 || width <= 0 || height <= 0 || offsetX < 0 || offsetY < 0 ||
            x + width > session_.asset().textureWidth ||
            y + height > session_.asset().textureHeight ||
            offsetX + width > frame.sourceWidth || offsetY + height > frame.sourceHeight) {
            return fail(error, "trim adjustment would move the selected frame outside its source bounds");
        }
        frame.atlasRect = {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                           static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        frame.sourceOffsetX = static_cast<std::int32_t>(offsetX);
        frame.sourceOffsetY = static_cast<std::int32_t>(offsetY);
        if (activePackSettings_) {
            if (frameIndex >= sourceFrameRects_.size()) {
                return fail(error, "packed sprite source recipe no longer matches the frame table");
            }
            SpriteRectPixels sourceRect = sourceFrameRects_[frameIndex];
            const std::int64_t sourceX = static_cast<std::int64_t>(sourceRect.x) + inset;
            const std::int64_t sourceY = static_cast<std::int64_t>(sourceRect.y) + inset;
            if (sourceX < 0 || sourceY < 0 || width <= 0 || height <= 0 ||
                sourceX + width > importSourceImage_.width ||
                sourceY + height > importSourceImage_.height) {
                return fail(error, "trim adjustment would move the source recipe outside the imported texture");
            }
            sourceFrameRects_[frameIndex] = {
                static_cast<std::uint32_t>(sourceX), static_cast<std::uint32_t>(sourceY),
                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        }
        if (!session_.update_frame(frameIndex, std::move(frame), error)) return false;
        clip = selected_clip();
    }
    return true;
}

bool SpriteAuthoringWorkspace::set_clip_playback(SpriteLoopMode loopMode, float playbackRate,
                                                 std::string* error) {
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr || !std::isfinite(playbackRate) || playbackRate <= 0.0F) {
        return fail(error, "selected clip or playback rate is invalid");
    }
    SpriteClip replacement = *clip;
    replacement.loopMode = loopMode;
    replacement.playbackRate = playbackRate;
    return session_.update_clip(timeline_.clip, std::move(replacement), error);
}

bool SpriteAuthoringWorkspace::repack_atlas(const SpriteAtlasPackSettings& settings,
                                            SpriteAtlasPackResult& out,
                                            std::string* error) {
    if (!has_source_image() || importSourceImage_.width == 0U) {
        return fail(error, "load a sprite source image before atlas packing");
    }
    if (!activePackSettings_) capture_source_frame_rects();
    if (sourceFrameRects_.size() != session_.asset().frames.size()) {
        return fail(error, "sprite source recipe no longer matches the frame table");
    }
    SpriteAsset sourceAsset = session_.asset();
    sourceAsset.textureWidth = importSourceImage_.width;
    sourceAsset.textureHeight = importSourceImage_.height;
    for (std::size_t index = 0U; index < sourceFrameRects_.size(); ++index) {
        sourceAsset.frames[index].atlasRect = sourceFrameRects_[index];
    }
    sourceAsset.recompute_hash();
    if (!repack_sprite_atlas(sourceAsset, importSourceImage_.view(), settings, out, error)) return false;
    std::string layoutError;
    const bool layoutChanged = session_.set_atlas_layout(
        out.width, out.height, out.frameRects, &layoutError);
    if (!layoutChanged && !layoutError.empty()) return fail(error, std::move(layoutError));
    activePackSettings_ = settings;
    sourceImage_.sourcePath = importSourceImage_.sourcePath;
    sourceImage_.width = out.width;
    sourceImage_.height = out.height;
    sourceImage_.rgba8 = out.rgba8;
    sourceImage_.mimeType = "image/raw-rgba8";
    normalize_timeline_selection();
    return true;
}

bool SpriteAuthoringWorkspace::publish_packed_atlas(
    const SpriteAtlasPublishSettings& requestedSettings,
    SpriteAtlasPublishResult& out,
    std::string* error) const {
    out = {};
    if (requestedSettings.atlasPath.empty() || requestedSettings.cookedAssetPath.empty()) {
        return fail(error, "sprite publication requires atlas and cooked asset output paths");
    }
    SpriteAtlasPublishSettings settings = requestedSettings;
    if (settings.dependencyManifestPath.empty()) {
        settings.dependencyManifestPath = settings.cookedAssetPath;
        settings.dependencyManifestPath += ".cook";
    }
    if (settings.indexedPalette && settings.indexedPalette->palettePath.empty()) {
        return fail(error, "indexed sprite publication requires a palette output path");
    }

    std::vector<std::filesystem::path> targets{
        settings.atlasPath, settings.cookedAssetPath, settings.dependencyManifestPath};
    if (settings.indexedPalette) targets.push_back(settings.indexedPalette->palettePath);
    for (std::size_t left = 0U; left < targets.size(); ++left) {
        const std::filesystem::path normalizedLeft = targets[left].lexically_normal();
        for (std::size_t right = left + 1U; right < targets.size(); ++right) {
            if (normalizedLeft == targets[right].lexically_normal()) {
                return fail(error, "sprite publication outputs must use distinct paths");
            }
        }
    }
    if (importSourceImage_.width == 0U || importSourceImage_.height == 0U ||
        !importSourceImage_.validate(error)) {
        return false;
    }
    if (sourceFrameRects_.size() != session_.asset().frames.size()) {
        return fail(error, "sprite source recipe no longer matches the frame table");
    }

    SpriteAsset sourceAsset = session_.asset();
    sourceAsset.textureWidth = importSourceImage_.width;
    sourceAsset.textureHeight = importSourceImage_.height;
    if (!importSourceImage_.sourcePath.empty()) {
        sourceAsset.textureAsset = importSourceImage_.sourcePath.generic_string();
    }
    // Authoring sources are RGBA. Indexed mode is applied only to the packed publication result.
    sourceAsset.paletteAsset.clear();
    for (std::size_t index = 0U; index < sourceFrameRects_.size(); ++index) {
        sourceAsset.frames[index].atlasRect = sourceFrameRects_[index];
    }
    sourceAsset.recompute_hash();

    SpriteAtlasPackResult packed;
    if (!repack_sprite_atlas(sourceAsset, importSourceImage_.view(), settings.packing,
                             packed, error)) {
        return false;
    }

    std::vector<std::byte> atlasRgba = packed.rgba8;
    std::optional<SpritePaletteAsset> publishedPalette;
    SpritePaletteIndexDiagnostics paletteDiagnostics;
    std::string paletteReference;
    if (settings.indexedPalette) {
        publishedPalette = settings.indexedPalette->palette;
        if (!publishedPalette->validate(error)) return false;
        const std::uint64_t paletteHash = sprite_palette_content_hash(*publishedPalette);
        if (publishedPalette->contentHash != 0U &&
            publishedPalette->contentHash != paletteHash) {
            return fail(error, "sprite palette content hash is stale");
        }
        publishedPalette->contentHash = paletteHash;
        paletteReference = cooked_palette_reference(settings);
        if (paletteReference.empty()) {
            return fail(error, "cooked sprite palette reference is empty");
        }
        std::vector<std::byte> indices;
        if (!index_sprite_rgba8(packed.width, packed.height, packed.rgba8,
                                *publishedPalette, settings.indexedPalette->indexing,
                                indices, paletteDiagnostics, error)) {
            return false;
        }
        if (!encode_sprite_indices_rgba8(indices, *publishedPalette, atlasRgba, error)) {
            return false;
        }
    }

    std::vector<std::byte> atlasPng;
    if (!encode_png_rgba8(packed.width, packed.height, atlasRgba, atlasPng, error)) {
        return false;
    }

    const std::string textureReference = cooked_texture_reference(settings);
    if (textureReference.empty()) return fail(error, "cooked sprite texture reference is empty");
    SpriteAsset cookedAsset = sourceAsset;
    cookedAsset.textureAsset = textureReference;
    cookedAsset.textureWidth = packed.width;
    cookedAsset.textureHeight = packed.height;
    for (std::size_t index = 0U; index < packed.frameRects.size(); ++index) {
        cookedAsset.frames[index].atlasRect = packed.frameRects[index];
    }
    if (publishedPalette) {
        cookedAsset.paletteAsset = paletteReference;
        cookedAsset.paletteBank = settings.indexedPalette->indexing.bank;
        cookedAsset.sampling = SpriteSampling::Nearest;
    } else {
        cookedAsset.paletteAsset.clear();
    }
    cookedAsset.recompute_hash();
    std::string validationError;
    if (!cookedAsset.validate(&validationError)) {
        return fail(error, "cooked sprite asset is invalid: " + validationError);
    }

    const std::uint64_t sourceImageHash = hash_bytes(importSourceImage_.rgba8);
    std::uint64_t dependencyKey = kFnvOffset;
    hash_string(dependencyKey, "DVE_SPRITE_COOK_2");
    hash_u64(dependencyKey, sourceAsset.contentHash);
    hash_u64(dependencyKey, sourceImageHash);
    hash_u64(dependencyKey, settings.packing.maximumWidth);
    hash_u64(dependencyKey, settings.packing.paddingPixels);
    hash_u64(dependencyKey, settings.packing.powerOfTwo ? 1U : 0U);
    hash_u64(dependencyKey, settings.packing.extrudeEdges ? 1U : 0U);
    hash_string(dependencyKey, textureReference);
    hash_u64(dependencyKey, publishedPalette ? publishedPalette->contentHash : 0U);
    if (settings.indexedPalette) {
        hash_string(dependencyKey, paletteReference);
        hash_u64(dependencyKey, static_cast<std::uint64_t>(settings.indexedPalette->indexing.policy));
        hash_u64(dependencyKey, settings.indexedPalette->indexing.bank);
        hash_u64(dependencyKey, settings.indexedPalette->indexing.alphaThreshold);
        hash_u64(dependencyKey, settings.indexedPalette->indexing.unmatchedIndex);
    }

    SpriteAtlasPublishResult result;
    result.dependencyKey = dependencyKey;
    result.atlasContentHash = hash_bytes(atlasPng);
    result.cookedAssetHash = cookedAsset.contentHash;
    result.paletteContentHash = publishedPalette ? publishedPalette->contentHash : 0U;
    result.paletteDiagnostics = paletteDiagnostics;
    result.atlasWidth = packed.width;
    result.atlasHeight = packed.height;
    result.frameCount = packed.frameRects.size();
    const std::string manifest = sprite_cook_manifest(
        settings, result, sourceAsset.contentHash, sourceImageHash,
        textureReference, paletteReference);

    if (settings.skipIfUnchanged) {
        std::string existingManifest;
        std::vector<std::byte> existingAtlas;
        const SpriteAssetReadResult existingAsset = read_dvesprite(settings.cookedAssetPath);
        bool paletteMatches = true;
        if (publishedPalette) {
            const SpritePaletteReadResult existingPalette =
                read_dvepalette(settings.indexedPalette->palettePath);
            paletteMatches = existingPalette &&
                existingPalette.asset.contentHash == result.paletteContentHash;
        }
        if (read_file_text(settings.dependencyManifestPath, existingManifest) &&
            existingManifest == manifest && read_file_bytes(settings.atlasPath, existingAtlas) &&
            hash_bytes(existingAtlas) == result.atlasContentHash && existingAsset &&
            existingAsset.asset.contentHash == result.cookedAssetHash && paletteMatches) {
            result.upToDate = true;
            out = result;
            return true;
        }
    }

    std::vector<SpriteTransactionFile> files;
    files.reserve(targets.size());
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        SpriteTransactionFile file;
        file.target = targets[index];
        file.stage = sibling_work_path(file.target, "stage", dependencyKey, index);
        file.backup = sibling_work_path(file.target, "backup", dependencyKey, index);
        std::error_code cleanupError;
        std::filesystem::remove(file.stage, cleanupError);
        cleanupError.clear();
        std::filesystem::remove(file.backup, cleanupError);
        files.push_back(std::move(file));
    }
    const auto cleanup_stages = [&files]() noexcept {
        for (const SpriteTransactionFile& file : files) {
            std::error_code cleanupError;
            std::filesystem::remove(file.stage, cleanupError);
        }
    };
    if (!write_file_bytes(files[0].stage, atlasPng, error)) {
        cleanup_stages();
        return false;
    }
    if (!write_dvesprite(files[1].stage, cookedAsset, error)) {
        cleanup_stages();
        return false;
    }
    if (!write_file_text(files[2].stage, manifest, error)) {
        cleanup_stages();
        return false;
    }
    if (publishedPalette && !write_dvepalette(files[3].stage, *publishedPalette, error)) {
        cleanup_stages();
        return false;
    }
    if (!commit_sprite_transaction(files, error)) return false;
    out = result;
    return true;
}

void SpriteAuthoringWorkspace::set_source_watch_interval(float seconds) noexcept {
    if (!std::isfinite(seconds)) return;
    sourceWatchIntervalSeconds_ = std::clamp(seconds, 0.05F, 60.0F);
    sourceWatchElapsedSeconds_ = 0.0F;
}

SpriteSourceWatchResult SpriteAuthoringWorkspace::update_source_watch(float elapsedSeconds) {
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F) return {};
    sourceWatchElapsedSeconds_ += elapsedSeconds;
    if (sourceWatchElapsedSeconds_ < sourceWatchIntervalSeconds_) return {};
    sourceWatchElapsedSeconds_ = 0.0F;
    return poll_source_change(false);
}

SpriteSourceWatchResult SpriteAuthoringWorkspace::poll_source_change(bool force) {
    SpriteSourceWatchResult result;
    result.sourcePath = importSourceImage_.sourcePath;
    if ((!autoReimport_ && !force) || result.sourcePath.empty()) return result;
    std::filesystem::file_time_type writeTime{};
    std::uintmax_t size{};
    if (!source_file_signature(result.sourcePath, writeTime, size)) {
        result.state = SpriteSourceWatchState::Missing;
        result.message = "Sprite source is missing or unreadable: " + result.sourcePath.generic_string();
        return result;
    }
    if (!force && sourceWatchSignatureValid_ && writeTime == sourceWriteTime_ &&
        size == sourceFileSize_) {
        return result;
    }
    if (!sourceWatchSignatureValid_ && !force) {
        sourceWriteTime_ = writeTime;
        sourceFileSize_ = size;
        sourceWatchSignatureValid_ = true;
        return result;
    }
    std::string reimportError;
    if (!reimport_source(true, &reimportError)) {
        result.state = SpriteSourceWatchState::Failed;
        result.message = "Automatic sprite reimport failed: " + reimportError;
        return result;
    }
    sourceWriteTime_ = writeTime;
    sourceFileSize_ = size;
    sourceWatchSignatureValid_ = true;
    result.state = SpriteSourceWatchState::Reloaded;
    result.message = "Reimported changed sprite source: " + result.sourcePath.filename().generic_string();
    return result;
}

void SpriteAuthoringWorkspace::capture_source_frame_rects() {
    sourceFrameRects_.clear();
    sourceFrameRects_.reserve(session_.asset().frames.size());
    for (const SpriteFrame& frame : session_.asset().frames) {
        sourceFrameRects_.push_back(frame.atlasRect);
    }
}

bool SpriteAuthoringWorkspace::restore_unpacked_source(std::string* error) {
    if (!activePackSettings_) return true;
    if (sourceFrameRects_.size() != session_.asset().frames.size() ||
        importSourceImage_.width == 0U || importSourceImage_.height == 0U) {
        return fail(error, "packed sprite source recipe is unavailable");
    }
    if (!session_.set_atlas_layout(importSourceImage_.width, importSourceImage_.height,
                                   sourceFrameRects_, error)) {
        return false;
    }
    const std::string textureAsset = importSourceImage_.sourcePath.empty()
        ? session_.asset().textureAsset : importSourceImage_.sourcePath.generic_string();
    std::string sourceError;
    const bool changed = session_.set_texture_source(textureAsset, importSourceImage_.width,
                                                     importSourceImage_.height, &sourceError);
    if (!changed && !sourceError.empty()) return fail(error, std::move(sourceError));
    sourceImage_ = importSourceImage_;
    activePackSettings_.reset();
    return true;
}

void SpriteAuthoringWorkspace::normalize_timeline_selection() noexcept {
    if (timeline_.clip.empty() || find_clip(session_.asset(), timeline_.clip) == nullptr) {
        timeline_.clip = session_.document().selectedClip;
    }
    const SpriteClip* clip = selected_clip();
    if (clip == nullptr) {
        timeline_.selectedSequenceIndices.clear();
        timeline_.primarySequenceIndex.reset();
        return;
    }
    timeline_.selectedSequenceIndices.erase(
        std::remove_if(timeline_.selectedSequenceIndices.begin(),
                       timeline_.selectedSequenceIndices.end(),
                       [clip](std::size_t index) { return index >= clip->frames.size(); }),
        timeline_.selectedSequenceIndices.end());
    if (timeline_.selectedSequenceIndices.empty() && !clip->frames.empty()) {
        timeline_.selectedSequenceIndices.push_back(0U);
    }
    if (!timeline_.primarySequenceIndex ||
        std::find(timeline_.selectedSequenceIndices.begin(),
                  timeline_.selectedSequenceIndices.end(),
                  *timeline_.primarySequenceIndex) == timeline_.selectedSequenceIndices.end()) {
        timeline_.primarySequenceIndex = timeline_.selectedSequenceIndices.empty()
            ? std::nullopt
            : std::optional<std::size_t>(timeline_.selectedSequenceIndices.front());
    }
    if (timeline_.primarySequenceIndex) {
        static_cast<void>(session_.select_clip(timeline_.clip));
        static_cast<void>(session_.select_frame(clip->frames[*timeline_.primarySequenceIndex]));
    }
}


const SpriteClip* SpriteAuthoringWorkspace::selected_clip() const noexcept {
    return find_clip(session_.asset(), timeline_.clip);
}

void EditorSpriteAuthoringPanel::close() noexcept {
    open_ = false;
    capture_ = Capture::NoCapture;
    trackCaptureHandle_.reset();
    timelineDragIndex_.reset();
    paletteInspectorMode_ = false;
    tracksInspectorMode_ = false;
}

void EditorSpriteAuthoringPanel::toggle() noexcept {
    open_ = !open_;
    capture_ = Capture::NoCapture;
}

void EditorSpriteAuthoringPanel::resize(int width, int height) noexcept {
    width_ = std::max(640, width);
    height_ = std::max(480, height);
}

void EditorSpriteAuthoringPanel::update(float elapsedSeconds) noexcept {
    if (!open_ || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F) return;
    const SpriteSourceWatchResult watch = workspace_.update_source_watch(elapsedSeconds);
    if (watch.state == SpriteSourceWatchState::Reloaded ||
        watch.state == SpriteSourceWatchState::Missing ||
        watch.state == SpriteSourceWatchState::Failed) {
        set_status(watch.message);
    }
    if (paletteOpen_) {
        const double exactTicks = palettePreviewTickRemainder_ +
            static_cast<double>(elapsedSeconds) * kSpritePaletteClockTicksPerSecond;
        const double boundedTicks = std::min(exactTicks,
            static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
        const auto ticks = static_cast<std::uint64_t>(std::floor(boundedTicks));
        palettePreviewTickRemainder_ = boundedTicks - static_cast<double>(ticks);
        paletteSession_.advance_preview(ticks);
        paletteWatchElapsedSeconds_ += elapsedSeconds;
        if (paletteWatchElapsedSeconds_ >= 0.5F) {
            paletteWatchElapsedSeconds_ = std::fmod(paletteWatchElapsedSeconds_, 0.5F);
            const SpritePaletteExternalChangeResult paletteWatch =
                paletteSession_.poll_external_change(false);
            if (paletteWatch.state != SpritePaletteExternalChange::Unchanged) {
                set_status(paletteWatch.message);
            }
        }
    }
    if (!workspace_.timeline().playing) return;
    static_cast<void>(workspace_.session().set_preview_playing(true));
    static_cast<void>(workspace_.session().tick_preview(elapsedSeconds));
    workspace_.timeline().playheadSeconds += elapsedSeconds;
}

bool EditorSpriteAuthoringPanel::open_texture(const std::filesystem::path& sourcePath,
                                              const std::filesystem::path& assetPath,
                                              std::string* error) {
    const std::string name = sourcePath.stem().string().empty()
        ? std::string("Untitled Sprite") : sourcePath.stem().string();
    if (!workspace_.create_from_texture(sourcePath, name, sourcePath.generic_string(), 16.0F, error)) {
        return false;
    }
    assetPath_ = assetPath.empty() ? sourcePath.parent_path() / (name + ".dvesprite") : assetPath;
    const std::uint32_t width = workspace_.source_image().width;
    const std::uint32_t height = workspace_.source_image().height;
    gridSettings_ = {};
    gridSettings_.cellWidth = std::min<std::uint32_t>(16U, width);
    gridSettings_.cellHeight = std::min<std::uint32_t>(16U, height);
    gridSettings_.pivotPixels = {static_cast<float>(gridSettings_.cellWidth) * 0.5F, 0.0F};
    packSettings_ = {};
    paletteOpen_ = false;
    paletteInspectorMode_ = false;
    tracksInspectorMode_ = false;
    paletteTextureIsIndexTransport_ = false;
    paletteFirstVisibleColor_ = 0U;
    palettePreviewTickRemainder_ = 0.0;
    paletteWatchElapsedSeconds_ = 0.0F;
    open_ = true;
    freeSliceMode_ = false;
    set_status("Texture opened. Adjust the grid or drag a free slice.");
    return true;
}

bool EditorSpriteAuthoringPanel::open_asset(const std::filesystem::path& assetPath,
                                            const std::filesystem::path& projectRoot,
                                            std::string* error) {
    if (!workspace_.open_asset(assetPath, projectRoot, error)) return false;
    assetPath_ = assetPath;
    const SpriteFrame& first = workspace_.session().asset().frames.front();
    gridSettings_ = {};
    gridSettings_.cellWidth = std::max<std::uint32_t>(1U, first.sourceWidth);
    gridSettings_.cellHeight = std::max<std::uint32_t>(1U, first.sourceHeight);
    gridSettings_.pivotPixels = first.pivotPixels;
    if (!open_linked_palette(projectRoot, error)) return false;
    open_ = true;
    freeSliceMode_ = false;
    set_status(paletteOpen_ ? "Sprite asset and linked palette opened" : "Sprite asset opened");
    return true;
}

bool EditorSpriteAuthoringPanel::open_linked_palette(
    const std::filesystem::path& projectRoot, std::string* error) {
    paletteOpen_ = false;
    paletteInspectorMode_ = false;
    tracksInspectorMode_ = false;
    paletteTextureIsIndexTransport_ = false;
    paletteFirstVisibleColor_ = 0U;
    palettePreviewTickRemainder_ = 0.0;
    paletteWatchElapsedSeconds_ = 0.0F;
    const std::string& reference = workspace_.session().asset().paletteAsset;
    if (reference.empty()) return true;
    std::filesystem::path palettePath = reference;
    if (!palettePath.is_absolute()) {
        const std::filesystem::path projectCandidate = projectRoot.empty()
            ? std::filesystem::path{} : projectRoot / palettePath;
        const std::filesystem::path assetCandidate = assetPath_.parent_path() / palettePath;
        if (!projectCandidate.empty() && std::filesystem::exists(projectCandidate)) {
            palettePath = projectCandidate;
        } else {
            palettePath = assetCandidate;
        }
    }
    if (!paletteSession_.open(palettePath, error)) return false;
    paletteOpen_ = true;
    paletteTextureIsIndexTransport_ = looks_like_sprite_index_transport(
        workspace_.source_image(), paletteSession_.asset());
    return true;
}

bool EditorSpriteAuthoringPanel::save(std::string* error) {
    if (assetPath_.empty()) return fail(error, "choose a .dvesprite asset path before saving");
    if (!workspace_.save_asset(assetPath_, error)) return false;
    if (paletteOpen_ && paletteSession_.dirty() && !paletteSession_.save({}, error)) return false;
    set_status("Saved " + assetPath_.filename().generic_string() +
               (paletteOpen_ ? " and palette" : ""));
    return true;
}

SpriteAuthoringPanelFrame EditorSpriteAuthoringPanel::frame() const {
    SpriteAuthoringPanelFrame result;
    if (!open_) return result;
    const int margin = 26;
    result.panel = {margin, margin, std::max(580, width_ - margin * 2),
                    std::max(420, height_ - margin * 2)};
    const int titleHeight = 32;
    const int toolbarHeight = 34;
    const int statusHeight = 24;
    const int inspectorWidth = std::clamp(result.panel.width / 4, 190, 300);
    const int timelineHeight = std::clamp(result.panel.height / 4, 120, 190);
    result.titleBar = {result.panel.x, result.panel.y, result.panel.width, titleHeight};
    result.closeButton = {result.panel.x + result.panel.width - 30, result.panel.y + 5, 22, 22};
    const int toolbarY = result.panel.y + titleHeight;
    int x = result.panel.x + 8;
    auto button = [&](int width) {
        UiRect rect{x, toolbarY + 4, width, toolbarHeight - 8};
        x += width + 5;
        return rect;
    };
    result.saveButton = button(52);
    result.reimportButton = button(70);
    result.gridSliceButton = button(72);
    result.freeSliceButton = button(72);
    result.repackButton = button(64);
    result.previousButton = button(28);
    result.playButton = button(46);
    result.nextButton = button(28);
    result.loopButton = button(64);
    result.gridButton = button(52);
    result.onionPreviousButton = button(60);
    result.onionNextButton = button(60);

    const int contentY = toolbarY + toolbarHeight;
    const int contentBottom = result.panel.y + result.panel.height - statusHeight;
    result.inspector = {result.panel.x + result.panel.width - inspectorWidth, contentY,
                        inspectorWidth, contentBottom - contentY};
    result.timeline = {result.panel.x, contentBottom - timelineHeight,
                       result.panel.width - inspectorWidth, timelineHeight};
    result.canvas = {result.panel.x, contentY, result.panel.width - inspectorWidth,
                     result.timeline.y - contentY};
    result.statusBar = {result.panel.x, contentBottom, result.panel.width, statusHeight};

    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    if (clip != nullptr && !clip->frames.empty()) {
        const int padding = 8;
        const int frameWidth = 92;
        const int frameHeight = std::max(64, timelineHeight - 36);
        const int available = std::max(1, result.timeline.width - padding * 2);
        const std::size_t visible = std::max<std::size_t>(1U,
            std::min<std::size_t>(clip->frames.size(), static_cast<std::size_t>(available / frameWidth)));
        const std::size_t maximumStart = clip->frames.size() - visible;
        const std::size_t first = std::min(workspace_.timeline().firstVisibleSequenceIndex,
                                           maximumStart);
        result.timelineFrames.reserve(visible);
        result.timelineFrameIndices.reserve(visible);
        for (std::size_t local = 0U; local < visible; ++local) {
            result.timelineFrames.push_back({result.timeline.x + padding + static_cast<int>(local) * frameWidth,
                                             result.timeline.y + 24, frameWidth - 6, frameHeight});
            result.timelineFrameIndices.push_back(first + local);
        }
    }

    const int tabY = result.inspector.y + 6;
    const int tabWidth = std::max(52, (result.inspector.width - 24) / 3);
    result.sliceTabButton = {result.inspector.x + 8, tabY, tabWidth, 24};
    result.paletteTabButton = {result.sliceTabButton.x + tabWidth + 4, tabY, tabWidth, 24};
    result.tracksTabButton = {result.paletteTabButton.x + tabWidth + 4, tabY,
                              result.inspector.x + result.inspector.width - 8 -
                                  (result.paletteTabButton.x + tabWidth + 4), 24};
    result.paletteMode = paletteInspectorMode_ && paletteOpen_;
    result.tracksMode = tracksInspectorMode_;

    const int rowHeight = 28;
    const int minusX = result.inspector.x + result.inspector.width - 76;
    const int plusX = result.inspector.x + result.inspector.width - 38;
    int rowY = result.inspector.y + 40;
    if (!result.paletteMode && !result.tracksMode) {
        auto stepper = [&](UiRect& down, UiRect& up) {
            down = {minusX, rowY, 30, 22};
            up = {plusX, rowY, 30, 22};
            rowY += rowHeight;
        };
        stepper(result.cellWidthDown, result.cellWidthUp);
        stepper(result.cellHeightDown, result.cellHeightUp);
        stepper(result.durationDown, result.durationUp);
        stepper(result.pivotXDown, result.pivotXUp);
        stepper(result.pivotYDown, result.pivotYUp);
        result.trimInButton = {minusX, rowY, 30, 22};
        result.trimOutButton = {plusX, rowY, 30, 22};
        rowY += rowHeight;
        result.trimTransparentButton = {result.inspector.x + 10, rowY,
                                        result.inspector.width - 20, 24};
        rowY += rowHeight;
        result.skipTransparentButton = {result.inspector.x + 10, rowY,
                                        result.inspector.width - 20, 24};
    } else if (result.paletteMode) {
        result.palettePanel = {result.inspector.x + 8, rowY,
                               result.inspector.width - 16,
                               result.inspector.y + result.inspector.height - rowY - 8};
        const int left = result.palettePanel.x + 2;
        const int innerWidth = result.palettePanel.width - 4;
        const int small = 28;
        result.palettePreviousBankButton = {left, rowY, small, 22};
        result.paletteNextBankButton = {left + small + 3, rowY, small, 22};
        result.paletteAddBankButton = {left + small * 2 + 6, rowY, 42, 22};
        result.paletteRemoveBankButton = {left + small * 2 + 51, rowY, 42, 22};
        result.paletteSaveButton = {left + innerWidth - 48, rowY, 48, 22};
        rowY += 27;
        result.palettePlayButton = {left, rowY, 52, 22};
        result.paletteAddCycleButton = {left + 55, rowY, 54, 22};
        result.paletteRemoveCycleButton = {left + 112, rowY, 54, 22};
        result.paletteTransparentButton = {left + innerWidth - 78, rowY, 78, 22};
        rowY += 28;
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            result.paletteChannelDown[channel] = {minusX, rowY, 30, 20};
            result.paletteChannelUp[channel] = {plusX, rowY, 30, 20};
            rowY += 23;
        }
        result.paletteCycleFirstDown = {minusX, rowY, 30, 20};
        result.paletteCycleFirstUp = {plusX, rowY, 30, 20};
        rowY += 23;
        result.paletteCycleLastDown = {minusX, rowY, 30, 20};
        result.paletteCycleLastUp = {plusX, rowY, 30, 20};
        rowY += 23;
        result.paletteCycleTicksDown = {minusX, rowY, 30, 20};
        result.paletteCycleTicksUp = {plusX, rowY, 30, 20};
        rowY += 25;
        result.paletteSwatchArea = {left, rowY, innerWidth,
                                    std::max(0, result.palettePanel.y + result.palettePanel.height - rowY)};
        if (paletteOpen_ && !paletteSession_.asset().banks.empty()) {
            const auto selection = paletteSession_.selection();
            const auto& colors = paletteSession_.asset().banks[selection.bank].colors;
            constexpr int swatchSize = 20;
            constexpr int swatchGap = 3;
            const std::size_t columns = std::max<std::size_t>(1U,
                static_cast<std::size_t>(std::max(1, result.paletteSwatchArea.width) /
                                         (swatchSize + swatchGap)));
            const std::size_t rows = static_cast<std::size_t>(
                std::max(0, result.paletteSwatchArea.height) / (swatchSize + swatchGap));
            const std::size_t capacity = columns * rows;
            const std::size_t maximumStart = colors.size() > capacity ? colors.size() - capacity : 0U;
            const std::size_t first = std::min(paletteFirstVisibleColor_, maximumStart);
            const std::size_t count = std::min(capacity, colors.size() - first);
            result.paletteSwatches.reserve(count);
            result.paletteSwatchIndices.reserve(count);
            for (std::size_t local = 0U; local < count; ++local) {
                const std::size_t column = local % columns;
                const std::size_t row = local / columns;
                result.paletteSwatches.push_back({
                    result.paletteSwatchArea.x + static_cast<int>(column) * (swatchSize + swatchGap),
                    result.paletteSwatchArea.y + static_cast<int>(row) * (swatchSize + swatchGap),
                    swatchSize, swatchSize});
                result.paletteSwatchIndices.push_back(first + local);
            }
        }
    } else {
        const int left = result.inspector.x + 10;
        const int width = result.inspector.width - 20;
        const int half = (width - 4) / 2;
        result.trackAddHitboxButton = {left, rowY, half, 24};
        result.trackAddHurtboxButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackAddSocketButton = {left, rowY, half, 24};
        result.trackAddPropertyButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackAddSpawnButton = {left, rowY, half, 24};
        result.trackSetRootMotionButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackDeleteButton = {left, rowY, half, 24};
        result.trackPresetButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackCopyRangeButton = {left, rowY, half, 24};
        result.trackPasteRangeButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackDuplicateButton = {left, rowY, half, 24};
        result.trackMirrorButton = {left + half + 4, rowY, width - half - 4, 24};
        rowY += 29;
        result.trackRetimeButton = {left, rowY, width, 24};
        rowY += 29;
        result.trackSearchQuery = trackSearchQuery_;
        if (clip != nullptr && workspace_.session().document().selectedTrack) {
            SpriteTrackProductionEditor tools(const_cast<SpriteAuthoringSession&>(workspace_.session()));
            result.trackNumericDescriptors = tools.numeric_descriptors(
                clip->name, *workspace_.session().document().selectedTrack);
            if (result.trackNumericDescriptors.size() > 6U)
                result.trackNumericDescriptors.resize(6U);
            for (std::size_t index = 0U; index < result.trackNumericDescriptors.size(); ++index) {
                result.trackNumericDown.push_back({left + width - 48, rowY, 22, 20});
                result.trackNumericUp.push_back({left + width - 24, rowY, 22, 20});
                rowY += 22;
            }
            rowY += 4;
        }
        if (clip != nullptr && workspace_.timeline().primarySequenceIndex) {
            const std::size_t sequence = *workspace_.timeline().primarySequenceIndex;
            const int rowWidth = width;
            const int maximumBottom = result.inspector.y + result.inspector.height - 8;
            const auto appendRow = [&](SpriteTrackItemKind kind, SpriteTrackId id) {
                if (rowY + 22 > maximumBottom) return;
                result.trackRows.push_back({left, rowY, rowWidth, 22});
                result.trackRowKinds.push_back(kind);
                result.trackRowIds.push_back(id);
                rowY += 24;
            };
            for (const SpriteCombatWindow& item : clip->combatWindows) {
                if (sequence >= item.firstSequenceIndex && sequence <= item.lastSequenceIndex)
                    appendRow(SpriteTrackItemKind::CombatWindow, item.id);
            }
            for (const SpriteSocketKey& item : clip->socketKeys)
                if (item.sequenceIndex == sequence) appendRow(SpriteTrackItemKind::SocketKey, item.id);
            for (const SpritePropertyKey& item : clip->propertyKeys)
                if (item.sequenceIndex == sequence) appendRow(SpriteTrackItemKind::PropertyKey, item.id);
            for (const SpriteRootMotionKey& item : clip->rootMotionKeys)
                if (item.sequenceIndex == sequence) appendRow(SpriteTrackItemKind::RootMotionKey, item.id);
            if (!trackSearchQuery_.empty()) {
                SpriteTrackProductionEditor tools(const_cast<SpriteAuthoringSession&>(workspace_.session()));
                const auto matches = tools.search(trackSearchQuery_, clip->name);
                for (const SpriteTrackSearchMatch& match : matches) {
                    const bool already = std::any_of(result.trackRowIds.begin(), result.trackRowIds.end(),
                        [&match](SpriteTrackId id) { return id == match.id; });
                    if (!already) appendRow(match.kind, match.id);
                }
            }
        }
    }

    if (clip != nullptr) {
        for (std::size_t local = 0U; local < result.timelineFrameIndices.size(); ++local) {
            const std::size_t sequence = result.timelineFrameIndices[local];
            const UiRect& card = result.timelineFrames[local];
            const int y = card.y + 3;
            if (std::any_of(clip->combatWindows.begin(), clip->combatWindows.end(),
                            [sequence](const SpriteCombatWindow& window) {
                                return sequence >= window.firstSequenceIndex &&
                                       sequence <= window.lastSequenceIndex;
                            })) {
                result.timelineCombatMarkers.push_back({card.x + 3, y, 12, 4});
            }
            if (std::any_of(clip->socketKeys.begin(), clip->socketKeys.end(),
                            [sequence](const SpriteSocketKey& key) {
                                return key.sequenceIndex == sequence;
                            })) {
                result.timelineSocketMarkers.push_back({card.x + 18, y, 12, 4});
            }
            if (std::any_of(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                            [sequence](const SpritePropertyKey& key) {
                                return key.sequenceIndex == sequence;
                            })) {
                result.timelinePropertyMarkers.push_back({card.x + 33, y, 12, 4});
            }
            if (std::any_of(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
                            [sequence](const SpriteRootMotionKey& key) {
                                return key.sequenceIndex == sequence;
                            })) {
                result.timelineRootMarkers.push_back({card.x + 48, y, 12, 4});
            }
        }
    }

    if (capture_ == Capture::FreeSlice) {
        const int left = std::min(pointerDownX_, pointerX_);
        const int top = std::min(pointerDownY_, pointerY_);
        const int right = std::max(pointerDownX_, pointerX_);
        const int bottom = std::max(pointerDownY_, pointerY_);
        result.freeSlicePreview = UiRect{left, top, right - left, bottom - top};
    }
    return result;
}

std::optional<SpriteVec2> EditorSpriteAuthoringPanel::canvas_to_source(int x, int y) const noexcept {
    const SpriteAuthoringPanelFrame layout = frame();
    if (!layout.canvas.contains(x, y) || !workspace_.has_source_image()) return std::nullopt;
    const SpriteCanvasViewState& view = workspace_.canvas();
    const float localX = static_cast<float>(x - layout.canvas.x) - view.panPixels.x;
    const float localY = static_cast<float>(y - layout.canvas.y) - view.panPixels.y;
    if (view.zoom <= 0.0F) return std::nullopt;
    const float sourceX = localX / view.zoom;
    const float sourceY = localY / view.zoom;
    if (sourceX < 0.0F || sourceY < 0.0F ||
        sourceX >= static_cast<float>(workspace_.source_image().width) ||
        sourceY >= static_cast<float>(workspace_.source_image().height)) return std::nullopt;
    return SpriteVec2{sourceX, sourceY};
}

std::optional<SpriteVec2> EditorSpriteAuthoringPanel::canvas_to_track_local(int x, int y) const noexcept {
    const auto source = canvas_to_source(x, y);
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    if (!source || clip == nullptr || !workspace_.timeline().primarySequenceIndex ||
        *workspace_.timeline().primarySequenceIndex >= clip->frames.size()) return std::nullopt;
    const SpriteFrame& frame = workspace_.session().asset().frames[
        clip->frames[*workspace_.timeline().primarySequenceIndex]];
    return SpriteVec2{
        source->x - static_cast<float>(frame.atlasRect.x) - frame.pivotPixels.x +
            static_cast<float>(frame.sourceOffsetX),
        static_cast<float>(frame.atlasRect.height) -
            (source->y - static_cast<float>(frame.atlasRect.y)) - frame.pivotPixels.y +
            static_cast<float>(frame.sourceOffsetY)};
}

std::optional<SpriteTrackHandleKind> EditorSpriteAuthoringPanel::hit_test_track_handle(
    int x, int y) const noexcept {
    if (!tracksInspectorMode_) return std::nullopt;
    const auto selection = workspace_.session().document().selectedTrack;
    const auto local = canvas_to_track_local(x, y);
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    if (!selection || !local || clip == nullptr) return std::nullopt;
    const float tolerance = std::max(1.0F, 7.0F / std::max(0.25F, workspace_.canvas().zoom));
    if (selection->kind == SpriteTrackItemKind::SocketKey) {
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [selection](const SpriteSocketKey& key) { return key.id == selection->id; });
        if (found == clip->socketKeys.end()) return std::nullopt;
        const float dx = std::fabs(local->x - found->positionPixels.x);
        const float dy = std::fabs(local->y - found->positionPixels.y);
        return dx <= tolerance && dy <= tolerance
            ? std::optional<SpriteTrackHandleKind>{SpriteTrackHandleKind::Move} : std::nullopt;
    }
    if (selection->kind != SpriteTrackItemKind::CombatWindow) return std::nullopt;
    const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
        [selection](const SpriteCombatWindow& window) { return window.id == selection->id; });
    if (found == clip->combatWindows.end()) return std::nullopt;
    const float left = found->volume.centerPixels.x - found->volume.sizePixels.x * 0.5F;
    const float right = found->volume.centerPixels.x + found->volume.sizePixels.x * 0.5F;
    const float bottom = found->volume.centerPixels.y - found->volume.sizePixels.y * 0.5F;
    const float top = found->volume.centerPixels.y + found->volume.sizePixels.y * 0.5F;
    const bool nearLeft = std::fabs(local->x - left) <= tolerance;
    const bool nearRight = std::fabs(local->x - right) <= tolerance;
    const bool nearBottom = std::fabs(local->y - bottom) <= tolerance;
    const bool nearTop = std::fabs(local->y - top) <= tolerance;
    if (nearLeft && nearTop) return SpriteTrackHandleKind::ResizeTopLeft;
    if (nearRight && nearTop) return SpriteTrackHandleKind::ResizeTopRight;
    if (nearLeft && nearBottom) return SpriteTrackHandleKind::ResizeBottomLeft;
    if (nearRight && nearBottom) return SpriteTrackHandleKind::ResizeBottomRight;
    if (nearLeft && local->y >= bottom - tolerance && local->y <= top + tolerance)
        return SpriteTrackHandleKind::ResizeLeft;
    if (nearRight && local->y >= bottom - tolerance && local->y <= top + tolerance)
        return SpriteTrackHandleKind::ResizeRight;
    if (nearTop && local->x >= left - tolerance && local->x <= right + tolerance)
        return SpriteTrackHandleKind::ResizeTop;
    if (nearBottom && local->x >= left - tolerance && local->x <= right + tolerance)
        return SpriteTrackHandleKind::ResizeBottom;
    if (local->x >= left && local->x <= right && local->y >= bottom && local->y <= top)
        return SpriteTrackHandleKind::Move;
    return std::nullopt;
}

bool EditorSpriteAuthoringPanel::create_free_slice(SpriteVec2 start, SpriteVec2 end,
                                                    std::string* error) {
    if (!workspace_.has_source_image()) return fail(error, "open a source image before free slicing");
    const float leftF = std::floor(std::min(start.x, end.x));
    const float topF = std::floor(std::min(start.y, end.y));
    const float rightF = std::ceil(std::max(start.x, end.x));
    const float bottomF = std::ceil(std::max(start.y, end.y));
    if (rightF - leftF < 1.0F || bottomF - topF < 1.0F) return fail(error, "free slice is empty");
    std::vector<SpriteSliceSpec> slices;
    slices.reserve(workspace_.session().asset().frames.size() + 1U);
    for (const SpriteFrame& frame : workspace_.session().asset().frames) {
        SpriteSliceSpec slice;
        slice.name = frame.name;
        slice.rect = frame.atlasRect;
        slice.pivotPixels = frame.pivotPixels;
        slice.durationSeconds = frame.durationSeconds;
        slice.event = frame.event;
        slices.push_back(std::move(slice));
    }
    SpriteSliceSpec added;
    added.name = "frame_" + std::to_string(slices.size());
    added.rect = {static_cast<std::uint32_t>(leftF), static_cast<std::uint32_t>(topF),
                  static_cast<std::uint32_t>(rightF - leftF),
                  static_cast<std::uint32_t>(bottomF - topF)};
    added.pivotPixels = {static_cast<float>(added.rect.width) * 0.5F, 0.0F};
    added.durationSeconds = gridSettings_.durationSeconds;
    slices.push_back(std::move(added));
    if (!workspace_.slice_freeform(slices, gridSettings_.trimTransparent,
                                   gridSettings_.alphaThreshold,
                                   gridSettings_.skipTransparent, error)) return false;
    const std::size_t selected = slices.size() - 1U;
    static_cast<void>(workspace_.select_timeline_frames(std::span<const std::size_t>(&selected, 1U), selected));
    set_status("Free slice added");
    return true;
}

bool EditorSpriteAuthoringPanel::edit_palette_channel(
    std::size_t channel, int delta, std::string* error) {
    if (!paletteOpen_ || channel >= 4U) return fail(error, "palette color channel is unavailable");
    const SpritePaletteAuthoringSelection selection = paletteSession_.selection();
    if (selection.bank >= paletteSession_.asset().banks.size() ||
        selection.color >= paletteSession_.asset().banks[selection.bank].colors.size()) {
        return fail(error, "palette color selection is invalid");
    }
    SpriteColor8 color = paletteSession_.asset().banks[selection.bank].colors[selection.color];
    std::uint8_t* value = channel == 0U ? &color.r : channel == 1U ? &color.g :
                          channel == 2U ? &color.b : &color.a;
    const int adjusted = std::clamp(static_cast<int>(*value) + delta, 0, 255);
    *value = static_cast<std::uint8_t>(adjusted);
    return paletteSession_.set_selected_color(color, error);
}

bool EditorSpriteAuthoringPanel::edit_selected_cycle(
    int field, int delta, std::string* error) {
    if (!paletteOpen_ || !paletteSession_.selection().cycle) {
        return fail(error, "select or add a palette cycle before editing it");
    }
    const std::size_t index = *paletteSession_.selection().cycle;
    SpritePaletteCycleTrack cycle = paletteSession_.asset().cycles[index];
    const std::size_t entries = paletteSession_.asset().entry_count();
    if (field == 0) {
        const int adjusted = std::clamp(static_cast<int>(cycle.firstIndex) + delta, 0,
                                        static_cast<int>(cycle.lastIndex) - 1);
        cycle.firstIndex = static_cast<std::uint16_t>(adjusted);
    } else if (field == 1) {
        const int adjusted = std::clamp(static_cast<int>(cycle.lastIndex) + delta,
                                        static_cast<int>(cycle.firstIndex) + 1,
                                        static_cast<int>(entries) - 1);
        cycle.lastIndex = static_cast<std::uint16_t>(adjusted);
    } else {
        const std::int64_t adjusted = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(cycle.ticksPerStep) + delta, 1, 86400000);
        cycle.ticksPerStep = static_cast<std::uint32_t>(adjusted);
    }
    return paletteSession_.update_cycle(index, std::move(cycle), error);
}

void EditorSpriteAuthoringPanel::keep_timeline_primary_visible() noexcept {
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    if (clip == nullptr || clip->frames.empty() || !workspace_.timeline().primarySequenceIndex) return;
    const SpriteAuthoringPanelFrame layout = frame();
    const std::size_t visible = std::max<std::size_t>(1U, layout.timelineFrames.size());
    const std::size_t primary = *workspace_.timeline().primarySequenceIndex;
    std::size_t& first = workspace_.timeline().firstVisibleSequenceIndex;
    if (primary < first) first = primary;
    else if (primary >= first + visible) first = primary - visible + 1U;
    const std::size_t maximumStart = clip->frames.size() > visible ? clip->frames.size() - visible : 0U;
    first = std::min(first, maximumStart);
}


bool EditorSpriteAuthoringPanel::nudge_selected_track(
    int axis, float delta, bool resize, bool rotate, std::string* error) {
    const auto selection = workspace_.session().document().selectedTrack;
    const std::string clipName = workspace_.timeline().clip;
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), clipName);
    if (!selection || clip == nullptr || axis < 0 || axis > 1)
        return fail(error, "select a track row before precision editing");
    switch (selection->kind) {
    case SpriteTrackItemKind::CombatWindow: {
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [selection](const SpriteCombatWindow& item) { return item.id == selection->id; });
        if (found == clip->combatWindows.end()) return fail(error, "selected combat window no longer exists");
        if (rotate) return workspace_.session().rotate_combat_window(
            clipName, selection->id, found->volume.rotationDegrees + delta, error);
        if (resize) {
            SpriteVec2 size = found->volume.sizePixels;
            if (axis == 0) size.x = std::max(1.0F, size.x + delta);
            else size.y = std::max(1.0F, size.y + delta);
            float radius = found->volume.radiusPixels;
            if (found->volume.shape == SpriteCombatShape::Circle) {
                radius = std::max(0.5F, radius + delta * 0.5F);
            } else if (found->volume.shape == SpriteCombatShape::Capsule) {
                radius = std::clamp(radius, 0.5F, std::min(size.x, size.y) * 0.5F);
            }
            return workspace_.session().resize_combat_window(
                clipName, selection->id, size, radius, error);
        }
        SpriteVec2 movement{};
        if (axis == 0) movement.x = delta; else movement.y = delta;
        return workspace_.session().translate_combat_window(clipName, selection->id, movement, error);
    }
    case SpriteTrackItemKind::SocketKey: {
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [selection](const SpriteSocketKey& item) { return item.id == selection->id; });
        if (found == clip->socketKeys.end()) return fail(error, "selected socket key no longer exists");
        SpriteSocketKey replacement = *found;
        if (rotate) replacement.rotationDegrees += delta;
        else if (axis == 0) replacement.positionPixels.x += delta;
        else replacement.positionPixels.y += delta;
        return workspace_.session().update_socket_key_by_id(
            clipName, selection->id, std::move(replacement), error);
    }
    case SpriteTrackItemKind::PropertyKey: {
        const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [selection](const SpritePropertyKey& item) { return item.id == selection->id; });
        if (found == clip->propertyKeys.end()) return fail(error, "selected property key no longer exists");
        if (found->value.type != SpritePropertyType::Vec2)
            return fail(error, "arrow precision editing requires a Vec2 property");
        SpritePropertyKey replacement = *found;
        if (axis == 0) replacement.value.vec2Value.x += delta;
        else replacement.value.vec2Value.y += delta;
        return workspace_.session().update_property_key_by_id(
            clipName, selection->id, std::move(replacement), error);
    }
    case SpriteTrackItemKind::RootMotionKey: {
        const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [selection](const SpriteRootMotionKey& item) { return item.id == selection->id; });
        if (found == clip->rootMotionKeys.end()) return fail(error, "selected root-motion key no longer exists");
        SpriteRootMotionKey replacement = *found;
        if (rotate) replacement.rotationDegrees += delta;
        else if (axis == 0) replacement.deltaPixels.x += delta;
        else replacement.deltaPixels.y += delta;
        return workspace_.session().update_root_motion_key_by_id(
            clipName, selection->id, replacement, error);
    }
    }
    return fail(error, "selected sprite track type is invalid");
}

void EditorSpriteAuthoringPanel::select_relative_frame(int direction) noexcept {
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    if (clip == nullptr || clip->frames.empty() || direction == 0) return;
    std::size_t current = workspace_.timeline().primarySequenceIndex.value_or(0U);
    if (direction > 0) current = (current + 1U) % clip->frames.size();
    else current = (current + clip->frames.size() - 1U) % clip->frames.size();
    static_cast<void>(workspace_.select_timeline_frames(std::span<const std::size_t>(&current, 1U), current));
    keep_timeline_primary_visible();
}

bool EditorSpriteAuthoringPanel::pointer_move(int x, int y, std::uint32_t) {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (capture_ == Capture::Pan) {
        workspace_.canvas().panPixels = {
            canvasPanStart_.x + static_cast<float>(x - pointerDownX_),
            canvasPanStart_.y + static_cast<float>(y - pointerDownY_)};
    }
    return frame().panel.contains(x, y) || capture_ != Capture::NoCapture;
}

bool EditorSpriteAuthoringPanel::pointer_down(int button, int x, int y, std::uint32_t modifiers) {
    if (!open_) return false;
    pointerX_ = pointerDownX_ = x;
    pointerY_ = pointerDownY_ = y;
    const SpriteAuthoringPanelFrame layout = frame();
    if (!layout.panel.contains(x, y)) return true;
    if (layout.closeButton.contains(x, y)) { close(); return true; }
    std::string error;
    if (layout.saveButton.contains(x, y)) {
        if (!save(&error)) set_status(error);
        return true;
    }
    if (layout.reimportButton.contains(x, y)) {
        if (!workspace_.reimport_source(true, &error)) set_status(error);
        else set_status("Source reimported without losing the current selection");
        return true;
    }
    if (layout.gridSliceButton.contains(x, y)) {
        if (!workspace_.slice_grid(gridSettings_, &error)) set_status(error);
        else set_status("Grid slice rebuilt");
        return true;
    }
    if (layout.freeSliceButton.contains(x, y)) {
        freeSliceMode_ = !freeSliceMode_;
        set_status(freeSliceMode_ ? "Drag a rectangle on the canvas" : "Free slice mode disabled");
        return true;
    }
    if (layout.repackButton.contains(x, y)) {
        SpriteAtlasPackResult packed;
        if (!workspace_.repack_atlas(packSettings_, packed, &error)) set_status(error);
        else set_status("Atlas repacked deterministically to " + std::to_string(packed.width) + "x" +
                        std::to_string(packed.height));
        return true;
    }
    if (layout.playButton.contains(x, y)) {
        workspace_.timeline().playing = !workspace_.timeline().playing;
        static_cast<void>(workspace_.session().set_preview_playing(workspace_.timeline().playing));
        return true;
    }
    if (layout.previousButton.contains(x, y)) { select_relative_frame(-1); return true; }
    if (layout.nextButton.contains(x, y)) { select_relative_frame(1); return true; }
    if (layout.loopButton.contains(x, y)) {
        const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
        if (clip != nullptr) {
            const SpriteLoopMode next = clip->loopMode == SpriteLoopMode::Once ? SpriteLoopMode::Loop
                : clip->loopMode == SpriteLoopMode::Loop ? SpriteLoopMode::PingPong : SpriteLoopMode::Once;
            if (!workspace_.set_clip_playback(next, clip->playbackRate, &error)) set_status(error);
        }
        return true;
    }
    if (layout.gridButton.contains(x, y)) {
        workspace_.canvas().showPixelGrid = !workspace_.canvas().showPixelGrid;
        return true;
    }
    if (layout.onionPreviousButton.contains(x, y)) {
        workspace_.canvas().onionSkinPrevious = !workspace_.canvas().onionSkinPrevious;
        return true;
    }
    if (layout.onionNextButton.contains(x, y)) {
        workspace_.canvas().onionSkinNext = !workspace_.canvas().onionSkinNext;
        return true;
    }
    if (layout.sliceTabButton.contains(x, y)) {
        paletteInspectorMode_ = false;
        tracksInspectorMode_ = false;
        return true;
    }
    if (layout.paletteTabButton.contains(x, y)) {
        if (!paletteOpen_) {
            if (!workspace_.has_source_image()) {
                set_status("Open a sprite source before creating a palette");
                return true;
            }
            SpritePaletteImportResult imported;
            const SpriteDecodedImage& image = workspace_.source_image();
            if (!paletteSession_.import_rgba8(
                    workspace_.session().asset().name + " Palette", image.width, image.height,
                    image.rgba8, 0U, imported, &error)) {
                set_status(error);
                return true;
            }
            std::filesystem::path palettePath = assetPath_;
            palettePath.replace_extension(".dvepalette");
            if (!paletteSession_.save(palettePath, &error)) {
                set_status(error);
                return true;
            }
            std::filesystem::path reference = palettePath.filename();
            if (!workspace_.session().set_palette_source(reference.generic_string(), &error)) {
                set_status(error);
                return true;
            }
            paletteOpen_ = true;
            paletteTextureIsIndexTransport_ = false;
            set_status("Created palette with " + std::to_string(imported.uniqueColors) +
                       " entries; " + std::to_string(imported.discardedColors) +
                       " colors exceeded the 256-entry limit");
        }
        paletteInspectorMode_ = true;
        tracksInspectorMode_ = false;
        return true;
    }
    if (layout.tracksTabButton.contains(x, y)) {
        paletteInspectorMode_ = false;
        tracksInspectorMode_ = true;
        keep_timeline_primary_visible();
        set_status("Track authoring uses pivot-relative frame coordinates");
        return true;
    }
    if (layout.paletteMode) {
        const SpritePaletteAuthoringSelection selection = paletteSession_.selection();
        const std::size_t bankCount = paletteSession_.asset().banks.size();
        if (layout.paletteSaveButton.contains(x, y)) {
            if (!paletteSession_.save({}, &error)) set_status(error);
            else set_status("Palette saved");
            return true;
        }
        if (layout.palettePreviousBankButton.contains(x, y) ||
            layout.paletteNextBankButton.contains(x, y)) {
            const bool next = layout.paletteNextBankButton.contains(x, y);
            const std::size_t bank = next ? (selection.bank + 1U) % bankCount
                                          : (selection.bank + bankCount - 1U) % bankCount;
            static_cast<void>(paletteSession_.select_bank(bank));
            paletteFirstVisibleColor_ = 0U;
            return true;
        }
        if (layout.paletteAddBankButton.contains(x, y)) {
            std::size_t suffix = bankCount + 1U;
            std::string name;
            do {
                name = "Bank " + std::to_string(suffix++);
            } while (find_sprite_palette_bank(paletteSession_.asset(), name) != nullptr);
            if (!paletteSession_.add_bank(std::move(name), true, &error)) set_status(error);
            else set_status("Palette bank added");
            return true;
        }
        if (layout.paletteRemoveBankButton.contains(x, y)) {
            if (!paletteSession_.remove_bank(selection.bank, &error)) set_status(error);
            else set_status("Palette bank removed");
            return true;
        }
        if (layout.palettePlayButton.contains(x, y)) {
            paletteSession_.set_preview_playing(!paletteSession_.preview_playing());
            return true;
        }
        if (layout.paletteAddCycleButton.contains(x, y)) {
            const std::size_t entries = paletteSession_.asset().entry_count();
            std::vector<bool> occupied(entries, false);
            for (const SpritePaletteCycleTrack& existing : paletteSession_.asset().cycles) {
                for (std::size_t entry = existing.firstIndex; entry <= existing.lastIndex; ++entry) {
                    occupied[entry] = true;
                }
            }
            const auto transparent = paletteSession_.asset().transparentIndex;
            std::optional<std::size_t> first;
            for (std::size_t entry = 0U; entry + 1U < entries; ++entry) {
                if (!occupied[entry] && !occupied[entry + 1U] &&
                    (!transparent || (*transparent != entry && *transparent != entry + 1U))) {
                    first = entry;
                    break;
                }
            }
            if (!first) {
                set_status("No free adjacent palette entries remain for another cycle");
                return true;
            }
            SpritePaletteCycleTrack cycle;
            cycle.name = "Cycle " + std::to_string(paletteSession_.asset().cycles.size() + 1U);
            cycle.firstIndex = static_cast<std::uint16_t>(*first);
            cycle.lastIndex = static_cast<std::uint16_t>(*first + 1U);
            cycle.ticksPerStep = 8U;
            if (!paletteSession_.add_cycle(std::move(cycle), &error)) set_status(error);
            else set_status("Palette cycle added");
            return true;
        }
        if (layout.paletteRemoveCycleButton.contains(x, y)) {
            if (!selection.cycle) set_status("No palette cycle is selected");
            else if (!paletteSession_.remove_cycle(*selection.cycle, &error)) set_status(error);
            else set_status("Palette cycle removed");
            return true;
        }
        if (layout.paletteTransparentButton.contains(x, y)) {
            const std::optional<std::uint16_t> transparent =
                paletteSession_.asset().transparentIndex == selection.color
                    ? std::nullopt
                    : std::optional<std::uint16_t>{static_cast<std::uint16_t>(selection.color)};
            if (!paletteSession_.set_transparent_index(transparent, &error)) set_status(error);
            else set_status(transparent ? "Transparent index assigned" : "Transparent index cleared");
            return true;
        }
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            if (!layout.paletteChannelDown[channel].contains(x, y) &&
                !layout.paletteChannelUp[channel].contains(x, y)) continue;
            const int delta = layout.paletteChannelUp[channel].contains(x, y) ? 1 : -1;
            if (!edit_palette_channel(channel, delta, &error)) set_status(error);
            return true;
        }
        const std::array<std::pair<UiRect, UiRect>, 3> cycleSteppers{{
            {layout.paletteCycleFirstDown, layout.paletteCycleFirstUp},
            {layout.paletteCycleLastDown, layout.paletteCycleLastUp},
            {layout.paletteCycleTicksDown, layout.paletteCycleTicksUp}}};
        for (std::size_t field = 0U; field < cycleSteppers.size(); ++field) {
            if (!cycleSteppers[field].first.contains(x, y) &&
                !cycleSteppers[field].second.contains(x, y)) continue;
            const int delta = cycleSteppers[field].second.contains(x, y) ? 1 : -1;
            if (!edit_selected_cycle(static_cast<int>(field), delta, &error)) set_status(error);
            return true;
        }
        for (std::size_t local = 0U; local < layout.paletteSwatches.size(); ++local) {
            if (!layout.paletteSwatches[local].contains(x, y)) continue;
            static_cast<void>(paletteSession_.select_color(layout.paletteSwatchIndices[local]));
            return true;
        }
    }
    if (layout.tracksMode) {
        const SpriteClip* trackClip = find_sprite_clip(
            workspace_.session().asset(), workspace_.timeline().clip);
        const std::size_t sequence = workspace_.timeline().primarySequenceIndex.value_or(0U);
        if (trackClip == nullptr || sequence >= trackClip->frames.size()) {
            set_status("Select an animation frame before authoring tracks");
            return true;
        }
        for (std::size_t row = 0U; row < layout.trackRows.size(); ++row) {
            if (!layout.trackRows[row].contains(x, y)) continue;
            if (workspace_.session().select_track(layout.trackRowKinds[row], layout.trackRowIds[row]))
                set_status("Track item selected; drag canvas handles or use numeric steppers");
            return true;
        }
        SpriteTrackProductionEditor productionTools(workspace_.session());
        const auto selectedRange = [&]() {
            std::pair<std::size_t, std::size_t> range{sequence, sequence};
            if (!workspace_.timeline().selectedSequenceIndices.empty()) {
                const auto [minimum, maximum] = std::minmax_element(
                    workspace_.timeline().selectedSequenceIndices.begin(),
                    workspace_.timeline().selectedSequenceIndices.end());
                range = {*minimum, *maximum};
            }
            return range;
        };
        for (std::size_t index = 0U; index < layout.trackNumericDescriptors.size(); ++index) {
            const bool decrement = layout.trackNumericDown[index].contains(x, y);
            const bool increment = layout.trackNumericUp[index].contains(x, y);
            if (!decrement && !increment) continue;
            const auto selected = workspace_.session().document().selectedTrack;
            if (!selected) { set_status("Select a track row first"); return true; }
            const float direction = increment ? 1.0F : -1.0F;
            if (!productionTools.adjust_numeric(trackClip->name, *selected,
                    layout.trackNumericDescriptors[index].field,
                    direction * layout.trackNumericDescriptors[index].step, &error)) set_status(error);
            else set_status(layout.trackNumericDescriptors[index].label + " adjusted");
            return true;
        }
        if (layout.trackCopyRangeButton.contains(x, y)) {
            const auto [first, last] = selectedRange();
            if (!productionTools.copy_range(trackClip->name, first, last, trackClipboard_, &error))
                set_status(error);
            else set_status("Copied track range " + std::to_string(first) + "-" + std::to_string(last));
            return true;
        }
        if (layout.trackPasteRangeButton.contains(x, y)) {
            if (!productionTools.paste_range(trackClip->name, sequence, trackClipboard_,
                    (modifiers & 1U) != 0U ? SpriteTrackPasteMode::ReplaceDestinationRange
                                           : SpriteTrackPasteMode::Merge, &error)) set_status(error);
            else set_status((modifiers & 1U) != 0U ? "Track range replace-pasted" : "Track range pasted");
            return true;
        }
        if (layout.trackRetimeButton.contains(x, y)) {
            const auto [first, last] = selectedRange();
            const std::size_t destinationLast = (modifiers & 1U) != 0U
                ? std::min(trackClip->frames.size() - 1U, last + 1U)
                : last > first ? last - 1U : last;
            if (!productionTools.bulk_retime(trackClip->name, first, last, first,
                                              destinationLast, &error)) set_status(error);
            else set_status((modifiers & 1U) != 0U ? "Track timing expanded" : "Track timing compressed");
            return true;
        }
        if (layout.trackAddSpawnButton.contains(x, y)) {
            if (!productionTools.apply_preset(trackClip->name, sequence,
                    SpriteTrackPresetKind::ProjectileSpawnRegion, &error)) set_status(error);
            else set_status("Projectile spawn region preset added");
            return true;
        }
        if (layout.trackPresetButton.contains(x, y)) {
            SpriteTrackPresetKind preset = SpriteTrackPresetKind::MeleeHitbox;
            if ((modifiers & 1U) != 0U) preset = SpriteTrackPresetKind::CharacterHurtbox;
            else if ((modifiers & 2U) != 0U) preset = SpriteTrackPresetKind::FootstepEvent;
            else if ((modifiers & 4U) != 0U) preset = SpriteTrackPresetKind::WeaponSocket;
            if (!productionTools.apply_preset(trackClip->name, sequence, preset, &error)) set_status(error);
            else set_status("Track preset applied (Shift hurtbox, Ctrl footstep, Alt socket)");
            return true;
        }
        if (layout.trackDuplicateButton.contains(x, y) || layout.trackMirrorButton.contains(x, y)) {
            const auto selected = workspace_.session().document().selectedTrack;
            if (!selected) { set_status("Select a track row first"); return true; }
            const auto [first, last] = selectedRange();
            if (workspace_.timeline().selectedSequenceIndices.size() > 1U) {
                if (layout.trackDuplicateButton.contains(x, y)) {
                    if (!productionTools.duplicate_range(trackClip->name, first, last,
                            static_cast<std::ptrdiff_t>(last - first + 1U), &error)) set_status(error);
                    else set_status("Selected multiframe tracks duplicated");
                } else if (!productionTools.mirror_range_x(trackClip->name, first, last, &error)) {
                    set_status(error);
                } else set_status("Selected multiframe tracks mirrored");
            } else if (layout.trackDuplicateButton.contains(x, y)) {
                SpriteTrackId duplicate{};
                if (!workspace_.session().duplicate_track_item(
                        trackClip->name, selected->kind, selected->id, 1, &duplicate, &error)) {
                    set_status(error);
                } else set_status("Track item duplicated one frame later");
            } else if (!workspace_.session().mirror_track_item_x(
                           trackClip->name, selected->kind, selected->id, &error)) {
                set_status(error);
            } else set_status("Track item mirrored across the pivot");
            return true;
        }
        if (layout.trackAddHitboxButton.contains(x, y) ||
            layout.trackAddHurtboxButton.contains(x, y)) {
            SpriteCombatWindow window;
            window.firstSequenceIndex = sequence;
            window.lastSequenceIndex = sequence;
            window.volume.name = layout.trackAddHitboxButton.contains(x, y)
                ? "hit_" + std::to_string(trackClip->combatWindows.size() + 1U)
                : "hurt_" + std::to_string(trackClip->combatWindows.size() + 1U);
            window.volume.role = layout.trackAddHitboxButton.contains(x, y)
                ? SpriteCombatRole::Hitbox : SpriteCombatRole::Hurtbox;
            window.volume.shape = SpriteCombatShape::Box;
            window.volume.centerPixels = {0.0F, 8.0F};
            window.volume.sizePixels = layout.trackAddHitboxButton.contains(x, y)
                ? SpriteVec2{12.0F, 8.0F} : SpriteVec2{12.0F, 16.0F};
            window.volume.radiusPixels = 4.0F;
            window.volume.attackId = static_cast<std::uint32_t>(trackClip->combatWindows.size() + 1U);
            window.volume.damage = layout.trackAddHitboxButton.contains(x, y) ? 1.0F : 0.0F;
            if (!workspace_.session().add_combat_window(trackClip->name, std::move(window), &error)) {
                set_status(error);
            } else {
                set_status(layout.trackAddHitboxButton.contains(x, y)
                    ? "Hitbox window added" : "Hurtbox window added");
            }
            return true;
        }
        if (layout.trackAddSocketButton.contains(x, y)) {
            SpriteSocketKey key;
            key.name = "socket_" + std::to_string(trackClip->socketKeys.size() + 1U);
            key.sequenceIndex = sequence;
            key.positionPixels = {0.0F, 8.0F};
            if (!workspace_.session().upsert_socket_key(trackClip->name, std::move(key), &error)) {
                set_status(error);
            } else set_status("Socket key added");
            return true;
        }
        if (layout.trackAddPropertyButton.contains(x, y)) {
            SpritePropertyKey key;
            key.name = "property_" + std::to_string(trackClip->propertyKeys.size() + 1U);
            key.sequenceIndex = sequence;
            key.value.type = SpritePropertyType::Boolean;
            key.value.booleanValue = true;
            if (!workspace_.session().upsert_property_key(trackClip->name, std::move(key), &error)) {
                set_status(error);
            } else set_status("Boolean property key added");
            return true;
        }
        if (layout.trackSetRootMotionButton.contains(x, y)) {
            const auto found = std::find_if(trackClip->rootMotionKeys.begin(), trackClip->rootMotionKeys.end(),
                [sequence](const SpriteRootMotionKey& key) { return key.sequenceIndex == sequence; });
            if (found != trackClip->rootMotionKeys.end()) {
                if (!workspace_.session().remove_root_motion_key(trackClip->name, sequence, &error)) {
                    set_status(error);
                } else set_status("Root-motion key removed");
            } else {
                SpriteRootMotionKey key;
                key.sequenceIndex = sequence;
                if (!workspace_.session().set_root_motion_key(trackClip->name, key, &error)) {
                    set_status(error);
                } else set_status("Root-motion key added");
            }
            return true;
        }
        if (layout.trackDeleteButton.contains(x, y)) {
            const auto selected = workspace_.session().document().selectedTrack;
            if (selected) {
                if (!workspace_.session().remove_track_by_id(
                        trackClip->name, selected->kind, selected->id, &error)) set_status(error);
                else set_status("Selected track item removed");
                return true;
            }
            for (std::size_t index = trackClip->combatWindows.size(); index > 0U; --index) {
                const SpriteCombatWindow& window = trackClip->combatWindows[index - 1U];
                if (sequence >= window.firstSequenceIndex && sequence <= window.lastSequenceIndex) {
                    if (!workspace_.session().remove_combat_window(trackClip->name, index - 1U, &error)) {
                        set_status(error);
                    } else set_status("Active combat window removed");
                    return true;
                }
            }
            for (auto it = trackClip->socketKeys.rbegin(); it != trackClip->socketKeys.rend(); ++it) {
                if (it->sequenceIndex == sequence) {
                    if (!workspace_.session().remove_socket_key(trackClip->name, it->name, sequence, &error)) {
                        set_status(error);
                    } else set_status("Socket key removed");
                    return true;
                }
            }
            for (auto it = trackClip->propertyKeys.rbegin(); it != trackClip->propertyKeys.rend(); ++it) {
                if (it->sequenceIndex == sequence) {
                    if (!workspace_.session().remove_property_key(trackClip->name, it->name, sequence, &error)) {
                        set_status(error);
                    } else set_status("Property key removed");
                    return true;
                }
            }
            set_status("No authored track item exists on the selected frame");
            return true;
        }
    }
    auto adjustUnsigned = [](std::uint32_t& value, int delta, std::uint32_t maximum) {
        const std::int64_t adjusted = std::clamp<std::int64_t>(static_cast<std::int64_t>(value) + delta, 1, maximum);
        value = static_cast<std::uint32_t>(adjusted);
    };
    if (layout.cellWidthDown.contains(x, y)) { adjustUnsigned(gridSettings_.cellWidth, -1, 16384U); return true; }
    if (layout.cellWidthUp.contains(x, y)) { adjustUnsigned(gridSettings_.cellWidth, 1, 16384U); return true; }
    if (layout.cellHeightDown.contains(x, y)) { adjustUnsigned(gridSettings_.cellHeight, -1, 16384U); return true; }
    if (layout.cellHeightUp.contains(x, y)) { adjustUnsigned(gridSettings_.cellHeight, 1, 16384U); return true; }
    if (layout.durationDown.contains(x, y) || layout.durationUp.contains(x, y)) {
        const float direction = layout.durationUp.contains(x, y) ? 1.0F : -1.0F;
        gridSettings_.durationSeconds = std::clamp(gridSettings_.durationSeconds + direction / 120.0F,
                                                   1.0F / 240.0F, 10.0F);
        if (!workspace_.set_selected_frame_duration(gridSettings_.durationSeconds, &error)) set_status(error);
        return true;
    }
    const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
    SpriteVec2 pivot = gridSettings_.pivotPixels;
    if (clip != nullptr && workspace_.timeline().primarySequenceIndex &&
        *workspace_.timeline().primarySequenceIndex < clip->frames.size()) {
        pivot = workspace_.session().asset().frames[clip->frames[*workspace_.timeline().primarySequenceIndex]].pivotPixels;
    }
    if (layout.pivotXDown.contains(x, y) || layout.pivotXUp.contains(x, y) ||
        layout.pivotYDown.contains(x, y) || layout.pivotYUp.contains(x, y)) {
        if (layout.pivotXDown.contains(x, y)) pivot.x -= 1.0F;
        if (layout.pivotXUp.contains(x, y)) pivot.x += 1.0F;
        if (layout.pivotYDown.contains(x, y)) pivot.y -= 1.0F;
        if (layout.pivotYUp.contains(x, y)) pivot.y += 1.0F;
        gridSettings_.pivotPixels = pivot;
        if (!workspace_.set_selected_frame_pivot(pivot, &error)) set_status(error);
        return true;
    }
    if (layout.trimInButton.contains(x, y) || layout.trimOutButton.contains(x, y)) {
        const int inset = layout.trimInButton.contains(x, y) ? 1 : -1;
        if (!workspace_.inset_selected_frame_trim(inset, &error)) set_status(error);
        return true;
    }
    if (layout.trimTransparentButton.contains(x, y)) {
        gridSettings_.trimTransparent = !gridSettings_.trimTransparent; return true;
    }
    if (layout.skipTransparentButton.contains(x, y)) {
        gridSettings_.skipTransparent = !gridSettings_.skipTransparent; return true;
    }
    for (std::size_t local = 0U; local < layout.timelineFrames.size(); ++local) {
        if (!layout.timelineFrames[local].contains(x, y)) continue;
        const std::size_t index = layout.timelineFrameIndices[local];
        std::vector<std::size_t> selected = workspace_.timeline().selectedSequenceIndices;
        const bool additive = (modifiers & 2U) != 0U;
        const bool range = (modifiers & 1U) != 0U;
        if (range && workspace_.timeline().primarySequenceIndex) {
            const std::size_t first = std::min(*workspace_.timeline().primarySequenceIndex, index);
            const std::size_t last = std::max(*workspace_.timeline().primarySequenceIndex, index);
            selected.clear();
            for (std::size_t candidate = first; candidate <= last; ++candidate) selected.push_back(candidate);
        } else if (additive) {
            const auto found = std::find(selected.begin(), selected.end(), index);
            if (found == selected.end()) selected.push_back(index); else selected.erase(found);
        } else {
            selected = {index};
        }
        static_cast<void>(workspace_.select_timeline_frames(selected, index));
        keep_timeline_primary_visible();
        capture_ = Capture::TimelineReorder;
        timelineDragIndex_ = index;
        return true;
    }
    if (layout.canvas.contains(x, y)) {
        if (button == 2 || (button == 1 && (modifiers & 4U) != 0U)) {
            capture_ = Capture::Pan;
            canvasPanStart_ = workspace_.canvas().panPixels;
            return true;
        }
        if (button == 1 && tracksInspectorMode_) {
            const auto handle = hit_test_track_handle(x, y);
            const auto local = canvas_to_track_local(x, y);
            if (handle && local) {
                capture_ = Capture::TrackHandle;
                trackCaptureHandle_ = handle;
                trackCaptureLocalStart_ = *local;
                return true;
            }
        }
        if (button == 1 && freeSliceMode_) {
            const auto point = canvas_to_source(x, y);
            if (point) {
                capture_ = Capture::FreeSlice;
                captureSourceStart_ = *point;
            }
            return true;
        }
        if (button == 1) {
            const auto point = canvas_to_source(x, y);
            if (point) {
                SpriteVec2 authoredPivot{point->x, static_cast<float>(workspace_.source_image().height) - point->y};
                if (!workspace_.set_selected_frame_pivot(authoredPivot, &error)) set_status(error);
                else gridSettings_.pivotPixels = authoredPivot;
            }
            return true;
        }
    }
    return true;
}

bool EditorSpriteAuthoringPanel::pointer_up(int button, int x, int y, std::uint32_t) {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    std::string error;
    if (capture_ == Capture::FreeSlice && button == 1) {
        const auto end = canvas_to_source(x, y);
        if (!end || !create_free_slice(captureSourceStart_, *end, &error)) {
            if (!error.empty()) set_status(error);
        }
    } else if (capture_ == Capture::TrackHandle && button == 1) {
        const auto end = canvas_to_track_local(x, y);
        const auto selection = workspace_.session().document().selectedTrack;
        if (end && selection && trackCaptureHandle_) {
            const SpriteVec2 delta{end->x - trackCaptureLocalStart_.x,
                                   end->y - trackCaptureLocalStart_.y};
            SpriteTrackProductionEditor tools(workspace_.session());
            if (!tools.drag_item(workspace_.timeline().clip, *selection, *trackCaptureHandle_,
                                 delta, &error)) set_status(error);
            else set_status("Track handle edit committed");
        }
    } else if (capture_ == Capture::TimelineReorder && button == 1) {
        const SpriteAuthoringPanelFrame layout = frame();
        for (std::size_t local = 0U; local < layout.timelineFrames.size(); ++local) {
            if (layout.timelineFrames[local].contains(x, y)) {
                const std::size_t destination = layout.timelineFrameIndices[local];
                if (!workspace_.reorder_timeline_selection(destination, &error) && !error.empty()) set_status(error);
                keep_timeline_primary_visible();
                break;
            }
        }
    }
    capture_ = Capture::NoCapture;
    timelineDragIndex_.reset();
    return true;
}

bool EditorSpriteAuthoringPanel::pointer_wheel(float steps, int x, int y, std::uint32_t) {
    if (!open_) return false;
    const SpriteAuthoringPanelFrame layout = frame();
    if (layout.timeline.contains(x, y)) {
        const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
        if (clip != nullptr && !clip->frames.empty()) {
            const std::size_t visible = std::max<std::size_t>(1U, layout.timelineFrames.size());
            const std::size_t maximumStart = clip->frames.size() > visible ? clip->frames.size() - visible : 0U;
            const std::int64_t delta = steps > 0.0F ? -3 : 3;
            const std::int64_t adjusted = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(workspace_.timeline().firstVisibleSequenceIndex) + delta,
                0, static_cast<std::int64_t>(maximumStart));
            workspace_.timeline().firstVisibleSequenceIndex = static_cast<std::size_t>(adjusted);
        }
        return true;
    }
    if (layout.paletteMode && layout.paletteSwatchArea.contains(x, y)) {
        const std::size_t entries = paletteSession_.asset().entry_count();
        const std::int64_t delta = steps > 0.0F ? -8 : 8;
        const std::int64_t adjusted = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(paletteFirstVisibleColor_) + delta,
            0, static_cast<std::int64_t>(entries > 0U ? entries - 1U : 0U));
        paletteFirstVisibleColor_ = static_cast<std::size_t>(adjusted);
        return true;
    }
    if (!layout.canvas.contains(x, y)) return layout.panel.contains(x, y);
    const float factor = steps > 0.0F ? 1.25F : 0.8F;
    workspace_.zoom_at(factor, {static_cast<float>(x - layout.canvas.x),
                                static_cast<float>(y - layout.canvas.y)});
    return true;
}

bool EditorSpriteAuthoringPanel::key_down(std::string_view key, bool control, bool shift, bool alt) {
    if (!open_) return false;
    std::string error;
    if (trackSearchEditing_) {
        if (key == "escape") {
            trackSearchEditing_ = false;
            set_status("Track search cancelled");
            return true;
        }
        if (key == "enter") {
            trackSearchEditing_ = false;
            set_status("Track search applied: " + trackSearchQuery_);
            return true;
        }
        if (key == "backspace") {
            if (!trackSearchQuery_.empty()) trackSearchQuery_.pop_back();
            return true;
        }
        if (!control && !alt && key.size() == 1U) {
            const unsigned char character = static_cast<unsigned char>(key.front());
            if (std::isalnum(character) != 0 || character == '_' || character == ' ' || character == '-') {
                if (trackSearchQuery_.size() < 128U) trackSearchQuery_.push_back(static_cast<char>(character));
                return true;
            }
        }
    }
    if (key == "escape") { if (freeSliceMode_) freeSliceMode_ = false; else close(); return true; }
    if (control && key == "s") { if (!save(&error)) set_status(error); return true; }
    if (tracksInspectorMode_ && control && key == "f") {
        trackSearchEditing_ = true;
        set_status("Type to search event/property/track fields; Enter accepts");
        return true;
    }
    if (tracksInspectorMode_ && control && (key == "c" || key == "v" || key == "m" || key == "r")) {
        const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
        if (clip == nullptr || !workspace_.timeline().primarySequenceIndex) return true;
        std::size_t first = *workspace_.timeline().primarySequenceIndex;
        std::size_t last = first;
        if (!workspace_.timeline().selectedSequenceIndices.empty()) {
            const auto [minimum, maximum] = std::minmax_element(
                workspace_.timeline().selectedSequenceIndices.begin(),
                workspace_.timeline().selectedSequenceIndices.end());
            first = *minimum;
            last = *maximum;
        }
        SpriteTrackProductionEditor tools(workspace_.session());
        bool changed = false;
        if (key == "c") changed = tools.copy_range(clip->name, first, last, trackClipboard_, &error);
        else if (key == "v") changed = tools.paste_range(clip->name,
            *workspace_.timeline().primarySequenceIndex, trackClipboard_,
            shift ? SpriteTrackPasteMode::ReplaceDestinationRange : SpriteTrackPasteMode::Merge, &error);
        else if (key == "m") changed = tools.mirror_range_x(clip->name, first, last, &error);
        else {
            const std::size_t destinationLast = shift
                ? std::min(clip->frames.size() - 1U, last + 1U)
                : last > first ? last - 1U : last;
            changed = tools.bulk_retime(clip->name, first, last, first, destinationLast, &error);
        }
        set_status(changed ? (key == "c" ? "Track range copied" : key == "v" ? "Track range pasted" :
                              key == "m" ? "Track range mirrored" : "Track range retimed") : error);
        return true;
    }
    if (control && key == "z") {
        if (paletteInspectorMode_ && paletteOpen_) {
            const bool changed = shift ? paletteSession_.redo(&error) : paletteSession_.undo(&error);
            set_status(changed ? (shift ? "Palette redo" : "Palette undo") : error);
        } else {
            const bool changed = shift ? workspace_.session().redo() : workspace_.session().undo();
            set_status(changed ? (shift ? "Redo" : "Undo") : "No sprite history entry");
        }
        return true;
    }
    if (control && key == "y") {
        if (paletteInspectorMode_ && paletteOpen_) {
            set_status(paletteSession_.redo(&error) ? "Palette redo" : error);
        } else {
            set_status(workspace_.session().redo() ? "Redo" : "No redo entry");
        }
        return true;
    }
    if (key == "space") {
        workspace_.timeline().playing = !workspace_.timeline().playing;
        static_cast<void>(workspace_.session().set_preview_playing(workspace_.timeline().playing));
        return true;
    }
    if (tracksInspectorMode_ && workspace_.session().document().selectedTrack) {
        if (key == "delete") {
            const auto selected = *workspace_.session().document().selectedTrack;
            if (!workspace_.session().remove_track_by_id(
                    workspace_.timeline().clip, selected.kind, selected.id, &error)) set_status(error);
            else set_status("Selected track item removed");
            return true;
        }
        if (control && key == "d") {
            const auto selected = *workspace_.session().document().selectedTrack;
            if (workspace_.timeline().selectedSequenceIndices.size() > 1U) {
                const auto [minimum, maximum] = std::minmax_element(
                    workspace_.timeline().selectedSequenceIndices.begin(),
                    workspace_.timeline().selectedSequenceIndices.end());
                SpriteTrackProductionEditor tools(workspace_.session());
                const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(*maximum - *minimum + 1U);
                if (!tools.duplicate_range(workspace_.timeline().clip, *minimum, *maximum,
                                           offset, &error)) set_status(error);
                else set_status("Multiframe track range duplicated");
            } else {
                SpriteTrackId duplicate{};
                if (!workspace_.session().duplicate_track_item(
                        workspace_.timeline().clip, selected.kind, selected.id, 1, &duplicate, &error))
                    set_status(error);
                else set_status("Track item duplicated one frame later");
            }
            return true;
        }
        if (key == "m") {
            const auto selected = *workspace_.session().document().selectedTrack;
            if (!workspace_.session().mirror_track_item_x(
                    workspace_.timeline().clip, selected.kind, selected.id, &error)) set_status(error);
            else set_status("Track item mirrored across the pivot");
            return true;
        }
        if (key == "left" || key == "right" || key == "up" || key == "down") {
            const int axis = (key == "left" || key == "right") ? 0 : 1;
            const float delta = (key == "left" || key == "down") ? -1.0F : 1.0F;
            if (!nudge_selected_track(axis, delta, shift, alt, &error)) set_status(error);
            else set_status(alt ? "Track rotation adjusted" : shift ? "Track size adjusted" :
                            "Track position/value adjusted");
            return true;
        }
    }
    if (key == "left") { select_relative_frame(-1); return true; }
    if (key == "right") { select_relative_frame(1); return true; }
    if (key == "f") { workspace_.reset_view(); return true; }
    if (key == "g") { workspace_.canvas().showPixelGrid = !workspace_.canvas().showPixelGrid; return true; }
    if (key == "delete") {
        const SpriteClip* clip = find_sprite_clip(workspace_.session().asset(), workspace_.timeline().clip);
        if (clip != nullptr && clip->frames.size() > 1U && workspace_.timeline().primarySequenceIndex) {
            SpriteClip replacement = *clip;
            replacement.frames.erase(replacement.frames.begin() +
                static_cast<std::ptrdiff_t>(*workspace_.timeline().primarySequenceIndex));
            if (!workspace_.session().update_clip(clip->name, std::move(replacement), &error)) set_status(error);
            else static_cast<void>(workspace_.select_timeline_frames({}, std::nullopt));
        }
        return true;
    }
    return false;
}

} // namespace dve::editor
