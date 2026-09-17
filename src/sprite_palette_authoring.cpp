#include "dve/sprite_palette_authoring.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

SpritePaletteAsset default_palette() {
    SpritePaletteAsset palette;
    palette.name = "Palette";
    SpritePaletteBank bank;
    bank.name = "Default";
    bank.colors.resize(16U);
    for (std::size_t index = 0U; index < bank.colors.size(); ++index) {
        const auto value = static_cast<std::uint8_t>(index * 255U / (bank.colors.size() - 1U));
        bank.colors[index] = {value, value, value, 255U};
    }
    palette.banks.push_back(std::move(bank));
    palette.recompute_hash();
    return palette;
}

} // namespace

SpritePaletteAuthoringSession::SpritePaletteAuthoringSession() : asset_(default_palette()) {}

SpritePaletteAuthoringSession::SpritePaletteAuthoringSession(SpritePaletteAsset asset)
    : asset_(std::move(asset)) {
    std::string ignored;
    if (!asset_.validate(&ignored)) asset_ = default_palette();
    asset_.recompute_hash();
    normalize_selection();
}

template <class Edit>
bool SpritePaletteAuthoringSession::apply_edit(Edit&& edit, std::string* error) {
    Snapshot before{asset_, selection_};
    edit(asset_, selection_);
    normalize_selection();
    std::string validation;
    if (!asset_.validate(&validation)) {
        asset_ = std::move(before.asset);
        selection_ = before.selection;
        return fail(error, std::move(validation));
    }
    asset_.recompute_hash();
    undo_.push_back(std::move(before));
    constexpr std::size_t maximumUndo = 128U;
    if (undo_.size() > maximumUndo) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
    return true;
}

bool SpritePaletteAuthoringSession::create(
    std::string name, std::size_t entries, std::string* error) {
    if (name.empty() || name.size() > 256U || entries == 0U ||
        entries > kMaximumSpritePaletteEntries)
        return fail(error, "palette name or entry count is invalid");
    SpritePaletteAsset replacement;
    replacement.name = std::move(name);
    SpritePaletteBank bank;
    bank.name = "Default";
    bank.colors.resize(entries, SpriteColor8{0U, 0U, 0U, 255U});
    replacement.banks.push_back(std::move(bank));
    replacement.recompute_hash();
    asset_ = std::move(replacement);
    selection_ = {};
    path_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = true;
    signatureValid_ = false;
    return true;
}

bool SpritePaletteAuthoringSession::open(
    const std::filesystem::path& path, std::string* error) {
    const SpritePaletteReadResult loaded = read_dvepalette(path);
    if (!loaded) return fail(error, loaded.error);
    asset_ = loaded.asset;
    path_ = path;
    selection_ = {};
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    previewTicks_ = 0U;
    normalize_selection();
    remember_signature();
    return true;
}

bool SpritePaletteAuthoringSession::save(
    const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path target = path.empty() ? path_ : path;
    if (target.empty()) return fail(error, "palette save path is empty");
    asset_.recompute_hash();
    if (!write_dvepalette(target, asset_, error)) return false;
    path_ = target;
    dirty_ = false;
    remember_signature();
    return true;
}

bool SpritePaletteAuthoringSession::select_bank(std::size_t index) noexcept {
    if (index >= asset_.banks.size()) return false;
    selection_.bank = index;
    normalize_selection();
    return true;
}

bool SpritePaletteAuthoringSession::select_color(std::size_t index) noexcept {
    if (selection_.bank >= asset_.banks.size() ||
        index >= asset_.banks[selection_.bank].colors.size()) return false;
    selection_.color = index;
    return true;
}

bool SpritePaletteAuthoringSession::select_cycle(std::optional<std::size_t> index) noexcept {
    if (index && *index >= asset_.cycles.size()) return false;
    selection_.cycle = index;
    return true;
}

bool SpritePaletteAuthoringSession::set_color(
    std::size_t index, SpriteColor8 color, std::string* error) {
    if (selection_.bank >= asset_.banks.size() ||
        index >= asset_.banks[selection_.bank].colors.size())
        return fail(error, "palette color selection is outside the active bank");
    if (asset_.transparentIndex && index == *asset_.transparentIndex && color.a != 0U)
        return fail(error, "the transparent palette entry must keep zero alpha");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection&) {
        palette.banks[selection_.bank].colors[index] = color;
    }, error);
}

bool SpritePaletteAuthoringSession::set_selected_color(
    SpriteColor8 color, std::string* error) {
    return set_color(selection_.color, color, error);
}

bool SpritePaletteAuthoringSession::add_bank(
    std::string name, bool duplicateSelected, std::string* error) {
    if (name.empty() || name.size() > 128U)
        return fail(error, "palette bank name is empty or too long");
    if (asset_.banks.size() >= 256U) return fail(error, "palette bank limit reached");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection& selection) {
        SpritePaletteBank bank;
        bank.name = std::move(name);
        if (duplicateSelected && selection.bank < palette.banks.size())
            bank.colors = palette.banks[selection.bank].colors;
        else bank.colors.resize(palette.entry_count(), SpriteColor8{0U, 0U, 0U, 255U});
        palette.banks.push_back(std::move(bank));
        selection.bank = palette.banks.size() - 1U;
    }, error);
}

bool SpritePaletteAuthoringSession::rename_bank(
    std::size_t index, std::string name, std::string* error) {
    if (index >= asset_.banks.size() || name.empty() || name.size() > 128U)
        return fail(error, "palette bank rename is invalid");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection&) {
        palette.banks[index].name = std::move(name);
    }, error);
}

bool SpritePaletteAuthoringSession::remove_bank(std::size_t index, std::string* error) {
    if (asset_.banks.size() <= 1U) return fail(error, "a palette must retain at least one bank");
    if (index >= asset_.banks.size()) return fail(error, "palette bank selection is invalid");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection& selection) {
        palette.banks.erase(palette.banks.begin() + static_cast<std::ptrdiff_t>(index));
        if (selection.bank >= palette.banks.size()) selection.bank = palette.banks.size() - 1U;
    }, error);
}

bool SpritePaletteAuthoringSession::set_transparent_index(
    std::optional<std::uint16_t> index, std::string* error) {
    if (index && *index >= asset_.entry_count())
        return fail(error, "transparent palette index is outside every bank");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection&) {
        palette.transparentIndex = index;
        if (index) {
            for (SpritePaletteBank& bank : palette.banks) bank.colors[*index].a = 0U;
        }
    }, error);
}

bool SpritePaletteAuthoringSession::add_cycle(
    SpritePaletteCycleTrack track, std::string* error) {
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection& selection) {
        palette.cycles.push_back(std::move(track));
        selection.cycle = palette.cycles.size() - 1U;
    }, error);
}

bool SpritePaletteAuthoringSession::update_cycle(
    std::size_t index, SpritePaletteCycleTrack track, std::string* error) {
    if (index >= asset_.cycles.size()) return fail(error, "palette cycle selection is invalid");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection&) {
        palette.cycles[index] = std::move(track);
    }, error);
}

bool SpritePaletteAuthoringSession::remove_cycle(std::size_t index, std::string* error) {
    if (index >= asset_.cycles.size()) return fail(error, "palette cycle selection is invalid");
    return apply_edit([&](SpritePaletteAsset& palette, SpritePaletteAuthoringSelection& selection) {
        palette.cycles.erase(palette.cycles.begin() + static_cast<std::ptrdiff_t>(index));
        if (palette.cycles.empty()) selection.cycle.reset();
        else if (selection.cycle && *selection.cycle >= palette.cycles.size())
            selection.cycle = palette.cycles.size() - 1U;
    }, error);
}

bool SpritePaletteAuthoringSession::import_rgba8(
    std::string name, std::uint32_t width, std::uint32_t height,
    std::span<const std::byte> rgba8, std::uint8_t alphaThreshold,
    SpritePaletteImportResult& result, std::string* error) {
    result = {};
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U || pixels > std::numeric_limits<std::size_t>::max() / 4U ||
        rgba8.size() != static_cast<std::size_t>(pixels) * 4U)
        return fail(error, "palette import image dimensions do not match its RGBA8 payload");
    if (name.empty()) name = "Imported Palette";
    std::vector<SpriteColor8> colors;
    colors.reserve(kMaximumSpritePaletteEntries);
    bool hasTransparent = false;
    for (std::size_t offset = 0U; offset < rgba8.size(); offset += 4U) {
        SpriteColor8 color{
            std::to_integer<std::uint8_t>(rgba8[offset]),
            std::to_integer<std::uint8_t>(rgba8[offset + 1U]),
            std::to_integer<std::uint8_t>(rgba8[offset + 2U]),
            std::to_integer<std::uint8_t>(rgba8[offset + 3U])};
        if (color.a <= alphaThreshold) {
            ++result.transparentPixels;
            color = {0U, 0U, 0U, 0U};
            hasTransparent = true;
        }
        if (std::find(colors.begin(), colors.end(), color) != colors.end()) continue;
        if (colors.size() < kMaximumSpritePaletteEntries) colors.push_back(color);
        else ++result.discardedColors;
    }
    if (colors.empty()) colors.push_back({0U, 0U, 0U,
                                      static_cast<std::uint8_t>(hasTransparent ? 0U : 255U)});
    if (hasTransparent) {
        const SpriteColor8 transparent{0U, 0U, 0U, 0U};
        const auto found = std::find(colors.begin(), colors.end(), transparent);
        if (found != colors.end()) std::rotate(colors.begin(), found, found + 1);
        else colors.insert(colors.begin(), transparent);
        if (colors.size() > kMaximumSpritePaletteEntries) colors.pop_back();
    }
    SpritePaletteAsset imported;
    imported.name = std::move(name);
    imported.transparentIndex = hasTransparent ? std::optional<std::uint16_t>{0U} : std::nullopt;
    imported.banks.push_back({"Default", std::move(colors)});
    imported.recompute_hash();
    std::string validation;
    if (!imported.validate(&validation)) return fail(error, std::move(validation));
    undo_.push_back({asset_, selection_});
    redo_.clear();
    asset_ = std::move(imported);
    selection_ = {};
    dirty_ = true;
    result.uniqueColors = asset_.entry_count();
    return true;
}

void SpritePaletteAuthoringSession::advance_preview(std::uint64_t ticks) noexcept {
    if (!previewPlaying_) return;
    const std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max() - previewTicks_;
    previewTicks_ += std::min(ticks, remaining);
}

bool SpritePaletteAuthoringSession::resolve_preview(
    SpritePalettePacket& packet, std::string* error) const {
    SpritePaletteResolveDesc desc;
    desc.bank = static_cast<std::uint32_t>(selection_.bank);
    desc.clockTicks = previewTicks_;
    if (!resolve_sprite_palette(asset_, desc, packet, error)) return false;
    packet.paletteAsset = path_.empty() ? asset_.name : path_.generic_string();
    return true;
}

bool SpritePaletteAuthoringSession::undo(std::string* error) {
    if (undo_.empty()) return fail(error, "palette undo history is empty");
    redo_.push_back({asset_, selection_});
    Snapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    asset_ = std::move(snapshot.asset);
    selection_ = snapshot.selection;
    normalize_selection();
    dirty_ = true;
    return true;
}

bool SpritePaletteAuthoringSession::redo(std::string* error) {
    if (redo_.empty()) return fail(error, "palette redo history is empty");
    undo_.push_back({asset_, selection_});
    Snapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    asset_ = std::move(snapshot.asset);
    selection_ = snapshot.selection;
    normalize_selection();
    dirty_ = true;
    return true;
}

SpritePaletteExternalChangeResult SpritePaletteAuthoringSession::poll_external_change(bool force) {
    SpritePaletteExternalChangeResult result;
    if (path_.empty()) return result;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        result.state = SpritePaletteExternalChange::Missing;
        result.message = "Palette file is missing or unreadable";
        return result;
    }
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        result.state = SpritePaletteExternalChange::Missing;
        result.message = "Palette file size is unavailable";
        return result;
    }
    if (!force && signatureValid_ && time == writeTime_ && size == fileSize_) return result;
    if (dirty_) {
        result.state = SpritePaletteExternalChange::Conflict;
        result.message = "Palette changed on disk while local edits are unsaved";
        return result;
    }
    const SpritePaletteReadResult loaded = read_dvepalette(path_);
    if (!loaded) {
        result.state = SpritePaletteExternalChange::Failed;
        result.message = loaded.error;
        return result;
    }
    asset_ = loaded.asset;
    normalize_selection();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    writeTime_ = time;
    fileSize_ = size;
    signatureValid_ = true;
    result.state = SpritePaletteExternalChange::Reloaded;
    result.message = "Reloaded changed palette without modifying sprite source data";
    return result;
}

void SpritePaletteAuthoringSession::normalize_selection() noexcept {
    if (asset_.banks.empty()) selection_ = {};
    else {
        selection_.bank = std::min(selection_.bank, asset_.banks.size() - 1U);
        const std::size_t colors = asset_.banks[selection_.bank].colors.size();
        selection_.color = colors == 0U ? 0U : std::min(selection_.color, colors - 1U);
    }
    if (selection_.cycle && *selection_.cycle >= asset_.cycles.size()) selection_.cycle.reset();
}

void SpritePaletteAuthoringSession::remember_signature() noexcept {
    signatureValid_ = false;
    if (path_.empty()) return;
    std::error_code ec;
    writeTime_ = std::filesystem::last_write_time(path_, ec);
    if (ec) return;
    fileSize_ = std::filesystem::file_size(path_, ec);
    signatureValid_ = !ec;
}

} // namespace dve
