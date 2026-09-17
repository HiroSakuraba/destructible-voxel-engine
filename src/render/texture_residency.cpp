#include "dve/render/texture_residency.hpp"

#include "dve/render/polygon_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string_view text) {
    if (error) error->assign(text);
}

std::size_t mip_bytes(const PolygonTextureMipChain& chain) noexcept {
    std::size_t total{};
    for (const auto& level : chain.levels) total += level.rgba8.size();
    return total;
}

float srgb_to_linear(std::uint8_t value) noexcept {
    const float encoded = static_cast<float>(value) / 255.0F;
    return encoded <= 0.04045F ? encoded / 12.92F
                               : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

std::uint8_t linear_to_srgb(float value) noexcept {
    value = std::clamp(value, 0.0F, 1.0F);
    const float encoded = value <= 0.0031308F ? value * 12.92F
                                              : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::clamp(encoded * 255.0F + 0.5F, 0.0F, 255.0F));
}

std::uint8_t average_byte(const PolygonTextureMip& source, std::uint32_t x, std::uint32_t y,
                          std::uint32_t channel) noexcept {
    std::uint32_t sum{};
    for (std::uint32_t offsetY = 0; offsetY < 2U; ++offsetY) {
        for (std::uint32_t offsetX = 0; offsetX < 2U; ++offsetX) {
            const std::uint32_t sourceX = std::min(source.width - 1U, x * 2U + offsetX);
            const std::uint32_t sourceY = std::min(source.height - 1U, y * 2U + offsetY);
            sum += source.rgba8[(static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4U + channel];
        }
    }
    return static_cast<std::uint8_t>((sum + 2U) / 4U);
}
} // namespace

PolygonTextureMipChain generate_semantic_texture_mips(const PolygonImage& image,
                                                       TextureSemantic semantic) {
    PolygonTextureMipChain chain;
    const std::size_t required = static_cast<std::size_t>(image.width) * image.height * 4U;
    if (image.width == 0U || image.height == 0U || image.rgba8.size() != required) return chain;
    chain.levels.push_back({image.width, image.height, image.rgba8});
    while (chain.levels.back().width > 1U || chain.levels.back().height > 1U) {
        const PolygonTextureMip& source = chain.levels.back();
        PolygonTextureMip next;
        next.width = std::max(1U, source.width / 2U);
        next.height = std::max(1U, source.height / 2U);
        next.rgba8.resize(static_cast<std::size_t>(next.width) * next.height * 4U);
        for (std::uint32_t y = 0U; y < next.height; ++y) {
            for (std::uint32_t x = 0U; x < next.width; ++x) {
                const std::size_t destination = (static_cast<std::size_t>(y) * next.width + x) * 4U;
                if (semantic == TextureSemantic::Normal) {
                    float nx{}, ny{}, nz{};
                    for (std::uint32_t offsetY = 0; offsetY < 2U; ++offsetY) {
                        for (std::uint32_t offsetX = 0; offsetX < 2U; ++offsetX) {
                            const std::uint32_t sourceX = std::min(source.width - 1U, x * 2U + offsetX);
                            const std::uint32_t sourceY = std::min(source.height - 1U, y * 2U + offsetY);
                            const std::size_t pixel =
                                (static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4U;
                            nx += static_cast<float>(source.rgba8[pixel]) / 127.5F - 1.0F;
                            ny += static_cast<float>(source.rgba8[pixel + 1U]) / 127.5F - 1.0F;
                            nz += static_cast<float>(source.rgba8[pixel + 2U]) / 127.5F - 1.0F;
                        }
                    }
                    const float magnitude = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (magnitude > 1.0e-8F) {
                        nx /= magnitude; ny /= magnitude; nz /= magnitude;
                    } else {
                        nx = 0.0F; ny = 0.0F; nz = 1.0F;
                    }
                    const auto encode = [](float value) {
                        return static_cast<std::uint8_t>(std::clamp((value * 0.5F + 0.5F) * 255.0F + 0.5F,
                                                                    0.0F, 255.0F));
                    };
                    next.rgba8[destination] = encode(nx);
                    next.rgba8[destination + 1U] = encode(ny);
                    next.rgba8[destination + 2U] = encode(nz);
                    next.rgba8[destination + 3U] = average_byte(source, x, y, 3U);
                } else if (semantic == TextureSemantic::BaseColor || semantic == TextureSemantic::Emissive) {
                    for (std::uint32_t channel = 0U; channel < 3U; ++channel) {
                        float sum{};
                        for (std::uint32_t offsetY = 0; offsetY < 2U; ++offsetY) {
                            for (std::uint32_t offsetX = 0; offsetX < 2U; ++offsetX) {
                                const std::uint32_t sourceX = std::min(source.width - 1U, x * 2U + offsetX);
                                const std::uint32_t sourceY = std::min(source.height - 1U, y * 2U + offsetY);
                                const std::size_t pixel =
                                    (static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4U;
                                sum += srgb_to_linear(source.rgba8[pixel + channel]);
                            }
                        }
                        next.rgba8[destination + channel] = linear_to_srgb(sum * 0.25F);
                    }
                    next.rgba8[destination + 3U] = average_byte(source, x, y, 3U);
                } else {
                    for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                        next.rgba8[destination + channel] = average_byte(source, x, y, channel);
                }
            }
        }
        chain.levels.push_back(std::move(next));
    }
    return chain;
}

struct TextureResidencyManager::Pending {
    TextureUploadRequest request;
    PolygonTextureMipChain mipChain;
    std::size_t bytes{};
    std::uint64_t sourceHash{};
};

TextureResidencyManager::TextureResidencyManager(rhi::IDevice& device, std::size_t budgetBytes)
    : device_(device) {
    stats_.budgetBytes = std::max<std::size_t>(budgetBytes, 4U);
}

TextureResidencyManager::~TextureResidencyManager() { clear(); }

std::uint64_t TextureResidencyManager::image_hash(const PolygonImage& image) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](std::uint8_t value) mutable {
        hash ^= value; hash *= 1099511628211ULL;
    };
    for (unsigned char c : image.name) mix(c);
    for (unsigned char c : image.mimeType) mix(c);
    for (unsigned shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(image.width >> shift));
    for (unsigned shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(image.height >> shift));
    for (std::uint8_t value : image.rgba8) mix(value);
    return hash;
}

bool TextureResidencyManager::enqueue(TextureUploadRequest request, std::string* error) {
    if (request.assetId == 0U || request.image.width == 0U || request.image.height == 0U ||
        request.image.rgba8.size() != static_cast<std::size_t>(request.image.width) *
                                      request.image.height * 4U) {
        set_error(error, "texture residency request is invalid"); return false;
    }
    Pending pending;
    pending.sourceHash = image_hash(request.image);
    pending.request = std::move(request);
    if (pending.request.generateMips)
        pending.mipChain = generate_semantic_texture_mips(pending.request.image, pending.request.semantic);
    else pending.mipChain.levels.push_back({pending.request.image.width, pending.request.image.height,
                                            pending.request.image.rgba8});
    pending.bytes = mip_bytes(pending.mipChain);
    if (pending.mipChain.levels.empty() || pending.bytes == 0U) {
        set_error(error, "texture residency mip generation failed"); return false;
    }
    std::scoped_lock lock(mutex_);
    const auto queued = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& item) {
        return item.request.assetId == pending.request.assetId;
    });
    if (queued != pending_.end()) {
        if (pending.request.mode != TextureUploadMode::ReplaceIfChanged) {
            ++stats_.duplicateRequests;
            set_error(error, "texture asset id is already queued");
            return false;
        }
        if (queued->sourceHash == pending.sourceHash &&
            queued->request.semantic == pending.request.semantic) {
            ++stats_.unchangedRequests;
            return true;
        }
        stats_.pendingBytes -= queued->bytes;
        stats_.pendingBytes += pending.bytes;
        *queued = std::move(pending);
        ++stats_.requestsQueued;
        ++stats_.pendingRequestsReplaced;
        return true;
    }
    if (const auto resident = resident_.find(pending.request.assetId); resident != resident_.end()) {
        if (pending.request.mode != TextureUploadMode::ReplaceIfChanged) {
            ++stats_.duplicateRequests;
            set_error(error, "texture asset id is already resident");
            return false;
        }
        if (resident->second.sourceHash == pending.sourceHash &&
            resident->second.semantic == pending.request.semantic) {
            ++stats_.unchangedRequests;
            resident->second.lastUseSerial = serial_++;
            return true;
        }
    }
    stats_.pendingBytes += pending.bytes;
    ++stats_.requestsQueued;
    pending_.push_back(std::move(pending));
    stats_.pendingCount = pending_.size();
    return true;
}

bool TextureResidencyManager::evict_until_fits(std::size_t incomingBytes,
                                               std::uint64_t replacingAssetId,
                                               std::string* error) {
    const auto replacement = resident_.find(replacingAssetId);
    const std::size_t replacedBytes = replacement == resident_.end() ? 0U : replacement->second.residentBytes;
    const auto projected = [&] { return stats_.residentBytes - replacedBytes + incomingBytes; };
    while (projected() > stats_.budgetBytes && !resident_.empty()) {
        auto victim = resident_.end();
        for (auto candidate = resident_.begin(); candidate != resident_.end(); ++candidate) {
            if (candidate->first == replacingAssetId) continue;
            if (victim == resident_.end() || candidate->second.lastUseSerial < victim->second.lastUseSerial)
                victim = candidate;
        }
        if (victim == resident_.end()) break;
        if (!device_.destroy_texture_view(victim->second.view, error) ||
            !device_.destroy_texture(victim->second.texture, error)) return false;
        stats_.residentBytes -= victim->second.residentBytes;
        resident_.erase(victim);
        ++stats_.evictions;
    }
    if (projected() > stats_.budgetBytes) {
        set_error(error, "texture exceeds the entire residency budget"); return false;
    }
    return true;
}

std::size_t TextureResidencyManager::pump(std::size_t maximumUploadBytes, std::string* error) {
    std::size_t uploaded{};
    for (;;) {
        Pending pending;
        {
            std::scoped_lock lock(mutex_);
            if (pending_.empty()) break;
            if (uploaded != 0U && uploaded + pending_.front().bytes > maximumUploadBytes) break;
            pending = std::move(pending_.front());
            pending_.pop_front();
            stats_.pendingBytes -= pending.bytes;
            stats_.pendingCount = pending_.size();
        }
        if (pending.bytes > maximumUploadBytes && uploaded != 0U) {
            std::scoped_lock lock(mutex_);
            stats_.pendingBytes += pending.bytes;
            pending_.push_front(std::move(pending));
            stats_.pendingCount = pending_.size();
            break;
        }
        const bool replacing = resident_.contains(pending.request.assetId);
        if (!evict_until_fits(pending.bytes, pending.request.assetId, error)) {
            ++stats_.uploadsFailed;
            if (replacing) ++stats_.failedReplacementsPreserved;
            continue;
        }
        if (pending.request.compression != TextureCompressionPreference::Uncompressed)
            ++stats_.compressionFallbacks; // RHI currently exposes only uncompressed RGBA8.
        rhi::TextureDesc desc;
        desc.format = rhi::TextureFormat::RGBA8Unorm;
        desc.width = pending.mipChain.levels.front().width;
        desc.height = pending.mipChain.levels.front().height;
        desc.mipLevels = static_cast<std::uint32_t>(pending.mipChain.levels.size());
        desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination |
                     rhi::TextureUsage::CopySource;
        desc.initialState = rhi::ResourceState::CopyDestination;
        desc.debugName = pending.request.image.name;
        std::string localError;
        const rhi::TextureHandle texture = device_.create_texture(desc, &localError);
        bool ok = static_cast<bool>(texture);
        if (ok) {
            for (std::uint32_t mip = 0U; mip < desc.mipLevels; ++mip) {
                const auto& level = pending.mipChain.levels[mip];
                ok = device_.write_texture(texture, mip, 0U,
                    std::as_bytes(std::span<const std::uint8_t>(level.rgba8)),
                    static_cast<std::size_t>(level.width) * 4U, &localError);
                if (!ok) break;
            }
        }
        rhi::TextureViewHandle view;
        if (ok) {
            const auto commands = device_.begin_commands(rhi::QueueKind::Graphics,
                                                          "texture residency transition", &localError);
            ok = static_cast<bool>(commands) &&
                 device_.transition_texture(commands, texture, rhi::ResourceState::CopyDestination,
                                            rhi::ResourceState::ShaderRead, &localError) &&
                 static_cast<bool>(device_.submit(commands, &localError));
        }
        if (ok) {
            view = device_.create_texture_view({texture, 0U, desc.mipLevels, 0U, 1U,
                                                pending.request.image.name + " view"}, &localError);
            ok = static_cast<bool>(view);
        }
        if (!ok) {
            if (view) (void)device_.destroy_texture_view(view, nullptr);
            if (texture) (void)device_.destroy_texture(texture, nullptr);
            ++stats_.uploadsFailed;
            if (replacing) ++stats_.failedReplacementsPreserved;
            if (error) *error = localError;
            continue;
        }
        ResidentTexture record;
        record.texture = texture; record.view = view; record.width = desc.width;
        record.height = desc.height; record.mipLevels = desc.mipLevels;
        record.residentBytes = pending.bytes; record.sourceHash = pending.sourceHash;
        record.semantic = pending.request.semantic; record.lastUseSerial = serial_++;
        if (auto existing = resident_.find(pending.request.assetId); existing != resident_.end()) {
            const ResidentTexture old = existing->second;
            record.generation = old.generation + 1U;
            existing->second = record;
            stats_.residentBytes = stats_.residentBytes - old.residentBytes + pending.bytes;
            ++stats_.residentTexturesReplaced;
            std::string cleanupError;
            const bool viewDestroyed = device_.destroy_texture_view(old.view, &cleanupError);
            const bool textureDestroyed = device_.destroy_texture(old.texture, &cleanupError);
            if ((!viewDestroyed || !textureDestroyed) && error) *error = cleanupError;
        } else {
            resident_.emplace(pending.request.assetId, record);
            stats_.residentBytes += pending.bytes;
        }
        stats_.residentCount = resident_.size();
        ++stats_.uploadsCompleted;
        uploaded += pending.bytes;
        if (uploaded >= maximumUploadBytes) break;
    }
    return uploaded;
}

const ResidentTexture* TextureResidencyManager::find(std::uint64_t assetId) const noexcept {
    const auto it = resident_.find(assetId);
    return it == resident_.end() ? nullptr : &it->second;
}

std::optional<TextureResidencyReference> TextureResidencyManager::reference(
    std::uint64_t assetId) const noexcept {
    const auto it = resident_.find(assetId);
    if (it == resident_.end()) return std::nullopt;
    return TextureResidencyReference{assetId, it->second.generation, it->second.sourceHash};
}

bool TextureResidencyManager::reference_valid(
    const TextureResidencyReference& referenceValue) const noexcept {
    const auto it = resident_.find(referenceValue.assetId);
    return it != resident_.end() &&
           it->second.generation == referenceValue.generation &&
           it->second.sourceHash == referenceValue.sourceHash;
}

bool TextureResidencyManager::touch(std::uint64_t assetId) noexcept {
    const auto it = resident_.find(assetId);
    if (it == resident_.end()) return false;
    it->second.lastUseSerial = serial_++;
    return true;
}

bool TextureResidencyManager::remove(std::uint64_t assetId, std::string* error) {
    const auto it = resident_.find(assetId); if (it == resident_.end()) return false;
    if (!device_.destroy_texture_view(it->second.view, error) ||
        !device_.destroy_texture(it->second.texture, error)) return false;
    stats_.residentBytes -= it->second.residentBytes;
    resident_.erase(it); stats_.residentCount = resident_.size();
    return true;
}

void TextureResidencyManager::clear() noexcept {
    for (auto& [id, texture] : resident_) {
        (void)id;
        (void)device_.destroy_texture_view(texture.view, nullptr);
        (void)device_.destroy_texture(texture.texture, nullptr);
    }
    resident_.clear();
    std::scoped_lock lock(mutex_);
    pending_.clear();
    stats_.residentBytes = 0U; stats_.pendingBytes = 0U;
    stats_.residentCount = 0U; stats_.pendingCount = 0U;
}

void TextureResidencyManager::set_budget(std::size_t bytes) noexcept {
    stats_.budgetBytes = std::max<std::size_t>(bytes, 4U);
}

TextureResidencyStats TextureResidencyManager::stats() const noexcept {
    TextureResidencyStats copy = stats_;
    copy.residentCount = resident_.size();
    std::scoped_lock lock(mutex_);
    copy.pendingCount = pending_.size();
    return copy;
}

} // namespace dve::render
