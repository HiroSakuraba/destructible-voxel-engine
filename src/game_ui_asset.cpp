#include "dve/game_ui.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve::ui {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template<class T> void hash_integer(std::uint64_t& hash, T value) noexcept {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_double(std::uint64_t& hash, double value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint64_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (char raw : value) hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
}

void hash_vec(std::uint64_t& hash, UiVec2 value) noexcept {
    hash_float(hash, value.x); hash_float(hash, value.y);
}

void hash_edges(std::uint64_t& hash, UiEdges value) noexcept {
    hash_float(hash, value.left); hash_float(hash, value.top);
    hash_float(hash, value.right); hash_float(hash, value.bottom);
}

void hash_transform(std::uint64_t& hash, const RigidTransform& value) noexcept {
    hash_float(hash, value.position.x); hash_float(hash, value.position.y); hash_float(hash, value.position.z);
    const Quaternion rotation = normalize(value.rotation);
    hash_float(hash, rotation.x); hash_float(hash, rotation.y); hash_float(hash, rotation.z); hash_float(hash, rotation.w);
}

bool valid_canvas(const UiCanvasDesc& canvas) noexcept {
    const Quaternion q = canvas.worldTransform.rotation;
    const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return !canvas.name.empty() && canvas.name.size() <= 255U &&
        static_cast<unsigned>(canvas.mode) <= static_cast<unsigned>(UiCanvasMode::WorldSpace) &&
        std::isfinite(canvas.logicalSize.x) && std::isfinite(canvas.logicalSize.y) &&
        canvas.logicalSize.x > 0.0F && canvas.logicalSize.y > 0.0F &&
        std::isfinite(canvas.metersPerLogicalUnit) && canvas.metersPerLogicalUnit > 0.0F &&
        std::isfinite(canvas.worldTransform.position.x) && std::isfinite(canvas.worldTransform.position.y) &&
        std::isfinite(canvas.worldTransform.position.z) && std::isfinite(norm) && norm > 1.0e-12F;
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create UI asset directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary UI asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete UI asset");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (!ec) return true;
    std::filesystem::remove(temporary, ec);
    return fail(error, "could not publish UI asset: " + ec.message());
}

} // namespace

bool validate_ui_asset(const UiAsset& asset, std::string* error) {
    if (asset.name.empty() || asset.name.size() > 255U)
        return fail(error, "UI asset name is invalid");
    if (!valid_canvas(asset.canvas)) return fail(error, "UI asset canvas is invalid");
    return asset.document.validate(error);
}

std::uint64_t ui_asset_content_hash(const UiAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_string(hash, asset.canvas.name);
    hash_byte(hash, static_cast<std::uint8_t>(asset.canvas.mode));
    hash_vec(hash, asset.canvas.logicalSize);
    hash_transform(hash, asset.canvas.worldTransform);
    hash_float(hash, asset.canvas.metersPerLogicalUnit);
    hash_integer(hash, asset.canvas.layer);
    hash_byte(hash, asset.canvas.visible ? 1U : 0U);
    hash_integer(hash, asset.document.root());
    hash_integer(hash, asset.document.next_id());
    hash_integer(hash, static_cast<std::uint64_t>(asset.document.widgets().size()));
    for (const auto& [id, widget] : asset.document.widgets()) {
        hash_integer(hash, id); hash_integer(hash, widget.parent);
        hash_byte(hash, static_cast<std::uint8_t>(widget.kind));
        hash_string(hash, widget.name); hash_string(hash, widget.text);
        hash_string(hash, widget.localizationKey); hash_string(hash, widget.accessibilityLabel);
        hash_string(hash, widget.imageAsset); hash_string(hash, widget.binding);
        hash_byte(hash, static_cast<std::uint8_t>(widget.bindingTarget));
        hash_byte(hash, static_cast<std::uint8_t>(widget.layout.direction));
        hash_vec(hash, widget.layout.position); hash_vec(hash, widget.layout.preferredSize);
        hash_vec(hash, widget.layout.minimumSize); hash_vec(hash, widget.layout.maximumSize);
        hash_edges(hash, widget.layout.margin); hash_edges(hash, widget.layout.padding);
        hash_float(hash, widget.layout.spacing); hash_float(hash, widget.layout.flexGrow);
        hash_float(hash, widget.color.r); hash_float(hash, widget.color.g);
        hash_float(hash, widget.color.b); hash_float(hash, widget.color.a);
        hash_double(hash, widget.value); hash_double(hash, widget.minimum);
        hash_double(hash, widget.maximum); hash_double(hash, widget.step);
        hash_byte(hash, widget.visible ? 1U : 0U); hash_byte(hash, widget.enabled ? 1U : 0U);
        hash_byte(hash, widget.focusable ? 1U : 0U); hash_byte(hash, widget.modal ? 1U : 0U);
        hash_integer(hash, static_cast<std::uint64_t>(widget.children.size()));
        for (UiWidgetId child : widget.children) hash_integer(hash, child);
    }
    return hash;
}

bool write_dveui(const std::filesystem::path& path, const UiAsset& asset, std::string* error) {
    if (!validate_ui_asset(asset, error)) return false;
    const std::uint64_t hash = ui_asset_content_hash(asset);
    if (asset.contentHash != 0U && asset.contentHash != hash)
        return fail(error, "UI asset content hash is stale");
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    const Quaternion canvasRotation = normalize(asset.canvas.worldTransform.rotation);
    stream << "DVE_UI 1\nname " << std::quoted(asset.name) << "\n";
    stream << "canvas " << std::quoted(asset.canvas.name) << ' ' << static_cast<unsigned>(asset.canvas.mode) << ' '
           << asset.canvas.logicalSize.x << ' ' << asset.canvas.logicalSize.y << ' '
           << asset.canvas.worldTransform.position.x << ' ' << asset.canvas.worldTransform.position.y << ' '
           << asset.canvas.worldTransform.position.z << ' ' << canvasRotation.x << ' ' << canvasRotation.y << ' '
           << canvasRotation.z << ' ' << canvasRotation.w << ' ' << asset.canvas.metersPerLogicalUnit << ' '
           << asset.canvas.layer << ' ' << (asset.canvas.visible ? 1 : 0) << "\n";
    stream << "document " << asset.document.root() << ' ' << asset.document.next_id() << ' '
           << asset.document.widgets().size() << "\n";
    for (const auto& [id, widget] : asset.document.widgets()) {
        stream << "widget " << id << ' ' << widget.parent << ' ' << static_cast<unsigned>(widget.kind) << ' '
               << static_cast<unsigned>(widget.bindingTarget) << ' ' << static_cast<unsigned>(widget.layout.direction) << ' '
               << (widget.visible ? 1 : 0) << ' ' << (widget.enabled ? 1 : 0) << ' '
               << (widget.focusable ? 1 : 0) << ' ' << (widget.modal ? 1 : 0) << ' '
               << widget.value << ' ' << widget.minimum << ' ' << widget.maximum << ' ' << widget.step << ' '
               << std::quoted(widget.name) << ' ' << std::quoted(widget.text) << ' '
               << std::quoted(widget.localizationKey) << ' ' << std::quoted(widget.accessibilityLabel) << ' '
               << std::quoted(widget.imageAsset) << ' ' << std::quoted(widget.binding) << "\n";
        stream << "layout " << widget.layout.position.x << ' ' << widget.layout.position.y << ' '
               << widget.layout.preferredSize.x << ' ' << widget.layout.preferredSize.y << ' '
               << widget.layout.minimumSize.x << ' ' << widget.layout.minimumSize.y << ' '
               << widget.layout.maximumSize.x << ' ' << widget.layout.maximumSize.y << ' '
               << widget.layout.margin.left << ' ' << widget.layout.margin.top << ' '
               << widget.layout.margin.right << ' ' << widget.layout.margin.bottom << ' '
               << widget.layout.padding.left << ' ' << widget.layout.padding.top << ' '
               << widget.layout.padding.right << ' ' << widget.layout.padding.bottom << ' '
               << widget.layout.spacing << ' ' << widget.layout.flexGrow << "\n";
        stream << "color " << widget.color.r << ' ' << widget.color.g << ' '
               << widget.color.b << ' ' << widget.color.a << "\n";
        stream << "children " << widget.children.size();
        for (UiWidgetId child : widget.children) stream << ' ' << child;
        stream << "\n";
    }
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

UiAssetReadResult read_dveui(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    UiAssetReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) {
        result.error = ec ? "could not stat UI asset" : "UI asset exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open UI asset"; return result; }
    std::string token, magic;
    unsigned version{};
    if (!(stream >> magic >> version) || magic != "DVE_UI" || version != 1U ||
        !(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name))) {
        result.error = "unsupported or invalid UI asset header";
        return result;
    }
    unsigned mode{}, canvasVisible{};
    UiCanvasDesc& canvas = result.asset.canvas;
    if (!(stream >> token) || token != "canvas" || !(stream >> std::quoted(canvas.name) >> mode >>
          canvas.logicalSize.x >> canvas.logicalSize.y >> canvas.worldTransform.position.x >>
          canvas.worldTransform.position.y >> canvas.worldTransform.position.z >>
          canvas.worldTransform.rotation.x >> canvas.worldTransform.rotation.y >>
          canvas.worldTransform.rotation.z >> canvas.worldTransform.rotation.w >>
          canvas.metersPerLogicalUnit >> canvas.layer >> canvasVisible) ||
        mode > static_cast<unsigned>(UiCanvasMode::WorldSpace) || canvasVisible > 1U) {
        result.error = "invalid UI canvas record";
        return result;
    }
    canvas.mode = static_cast<UiCanvasMode>(mode);
    canvas.visible = canvasVisible != 0U;
    canvas.worldTransform.rotation = normalize(canvas.worldTransform.rotation);
    UiWidgetId root{}, nextId{};
    std::size_t widgetCount{};
    if (!(stream >> token >> root >> nextId >> widgetCount) || token != "document" ||
        widgetCount == 0U || widgetCount > 65536U) {
        result.error = "invalid UI document record";
        return result;
    }
    std::map<UiWidgetId, UiWidget> widgets;
    for (std::size_t index = 0U; index < widgetCount; ++index) {
        UiWidget widget;
        unsigned kind{}, bindingTarget{}, direction{}, visible{}, enabled{}, focusable{}, modal{};
        if (!(stream >> token >> widget.id >> widget.parent >> kind >> bindingTarget >> direction >>
              visible >> enabled >> focusable >> modal >> widget.value >> widget.minimum >> widget.maximum >>
              widget.step >> std::quoted(widget.name) >> std::quoted(widget.text) >>
              std::quoted(widget.localizationKey) >> std::quoted(widget.accessibilityLabel) >>
              std::quoted(widget.imageAsset) >> std::quoted(widget.binding)) || token != "widget" ||
            kind > static_cast<unsigned>(UiWidgetKind::TextInput) ||
            bindingTarget > static_cast<unsigned>(UiBindingTarget::Enabled) ||
            direction > static_cast<unsigned>(UiLayoutDirection::Overlay) || visible > 1U || enabled > 1U ||
            focusable > 1U || modal > 1U || widgets.contains(widget.id)) {
            result.error = "invalid UI widget record";
            return result;
        }
        widget.kind = static_cast<UiWidgetKind>(kind);
        widget.bindingTarget = static_cast<UiBindingTarget>(bindingTarget);
        widget.layout.direction = static_cast<UiLayoutDirection>(direction);
        widget.visible = visible != 0U; widget.enabled = enabled != 0U;
        widget.focusable = focusable != 0U; widget.modal = modal != 0U;
        if (!(stream >> token >> widget.layout.position.x >> widget.layout.position.y >>
              widget.layout.preferredSize.x >> widget.layout.preferredSize.y >>
              widget.layout.minimumSize.x >> widget.layout.minimumSize.y >>
              widget.layout.maximumSize.x >> widget.layout.maximumSize.y >>
              widget.layout.margin.left >> widget.layout.margin.top >> widget.layout.margin.right >>
              widget.layout.margin.bottom >> widget.layout.padding.left >> widget.layout.padding.top >>
              widget.layout.padding.right >> widget.layout.padding.bottom >> widget.layout.spacing >>
              widget.layout.flexGrow) || token != "layout" ||
            !(stream >> token >> widget.color.r >> widget.color.g >> widget.color.b >> widget.color.a) ||
            token != "color") {
            result.error = "invalid UI widget layout or color record";
            return result;
        }
        std::size_t childCount{};
        if (!(stream >> token >> childCount) || token != "children" || childCount > widgetCount) {
            result.error = "invalid UI widget child count";
            return result;
        }
        widget.children.resize(childCount);
        for (UiWidgetId& child : widget.children)
            if (!(stream >> child)) { result.error = "invalid UI widget child record"; return result; }
        widgets.emplace(widget.id, std::move(widget));
    }
    if (!result.asset.document.replace_records(root, nextId, std::move(widgets), &result.error)) return result;
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing UI asset content hash";
        return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing UI asset data"; return result; }
    if (!validate_ui_asset(result.asset, &result.error)) return result;
    if (result.asset.contentHash != ui_asset_content_hash(result.asset))
        result.error = "UI asset content hash mismatch";
    return result;
}

} // namespace dve::ui
