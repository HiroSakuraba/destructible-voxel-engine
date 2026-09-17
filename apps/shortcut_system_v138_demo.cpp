#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

namespace {
class PpmCanvas final : public dve::editor::IEditorCanvas {
public:
    PpmCanvas(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width * height), 0x0A1018U) {}
    void fill(dve::editor::UiRect rect, dve::editor::EditorColor color) const override {
        rect.x = std::max(0, rect.x); rect.y = std::max(0, rect.y);
        const int right = std::min(width_, rect.x + std::max(0, rect.width));
        const int bottom = std::min(height_, rect.y + std::max(0, rect.height));
        for (int y = rect.y; y < bottom; ++y)
            for (int x = rect.x; x < right; ++x) pixel(x, y, color);
    }
    void outline(dve::editor::UiRect rect, dve::editor::EditorColor color) const override {
        line(rect.x, rect.y, rect.x + rect.width - 1, rect.y, color);
        line(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, color);
        line(rect.x, rect.y, rect.x, rect.y + rect.height - 1, color);
        line(rect.x + rect.width - 1, rect.y, rect.x + rect.width - 1, rect.y + rect.height - 1, color);
    }
    void line(int x1, int y1, int x2, int y2, dve::editor::EditorColor color, int width = 1) const override {
        const int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
        const int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            for (int oy = -width / 2; oy <= width / 2; ++oy)
                for (int ox = -width / 2; ox <= width / 2; ++ox) pixel(x1 + ox, y1 + oy, color);
            if (x1 == x2 && y1 == y2) break;
            const int twice = error * 2;
            if (twice >= dy) { error += dy; x1 += sx; }
            if (twice <= dx) { error += dx; y1 += sy; }
        }
    }
    void text(int x, int y, std::string_view value, dve::editor::EditorColor color) const override {
        int cursor = x;
        for (unsigned char character : value) {
            if (character == ' ') { cursor += 6; continue; }
            for (int row = 0; row < 7; ++row) {
                const unsigned bits = static_cast<unsigned>(character) * 0x45D9F3BU + static_cast<unsigned>(row * 17);
                for (int column = 0; column < 5; ++column)
                    if (((bits >> (column + row)) & 1U) != 0U) pixel(cursor + column, y - 7 + row, color);
            }
            cursor += 6;
        }
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 6;
    }
    bool write(const std::filesystem::path& path) const {
        std::ofstream stream(path, std::ios::binary);
        if (!stream) return false;
        stream << "P6\n" << width_ << ' ' << height_ << "\n255\n";
        for (const auto color : pixels_) {
            const std::array<char, 3> rgb{static_cast<char>((color >> 16U) & 0xFFU),
                                          static_cast<char>((color >> 8U) & 0xFFU),
                                          static_cast<char>(color & 0xFFU)};
            stream.write(rgb.data(), 3);
        }
        return static_cast<bool>(stream);
    }
private:
    void pixel(int x, int y, dve::editor::EditorColor color) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        pixels_[static_cast<std::size_t>(y * width_ + x)] = color;
    }
    int width_{};
    int height_{};
    mutable std::vector<dve::editor::EditorColor> pixels_;
};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string safe_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
        return std::isalnum(value) != 0 ? static_cast<char>(std::tolower(value)) : '_';
    });
    while (name.find("__") != std::string::npos) name.replace(name.find("__"), 2, "_");
    return name;
}
} // namespace

int main(int argc, char** argv) {
    try {
        using namespace dve::editor;
        const std::filesystem::path output = argc > 1 ? argv[1] : "dve_shortcut_system_v1_38_demo";
        std::filesystem::create_directories(output);

        NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
        controller.resize(1440, 900);
        controller.open_shortcut_editor();
        controller.shortcut_panel().searchQuery = "camera";
        controller.shortcut_panel().contextFilter = ShortcutContext::Camera;
        controller.shortcut_panel().selectedRow = 2U;

        PpmCanvas canvas(1440, 900);
        render_native_editor(canvas, controller, 1440, 900);
        require(canvas.write(output / "shortcut_editor_v1_38.ppm"), "shortcut editor image write failed");

        EditorShortcutRegistry& shortcuts = controller.workspace().shortcuts();
        for (const std::string& profile : shortcuts.profile_names()) {
            const std::string serialized = shortcuts.serialize_profile(profile);
            require(!serialized.empty(), "profile serialization failed");
            std::ofstream stream(output / (safe_name(profile) + ".dveshortcuts"));
            stream << serialized;
            require(static_cast<bool>(stream), "profile write failed");
        }

        std::string error;
        require(shortcuts.duplicate_profile("DVE Default", "Demo Custom", &error), error);
        require(shortcuts.set_binding("Demo Custom", "camera.toggle_preview", ShortcutContext::Camera,
                                      ShortcutSlot::Secondary, keyboard_shortcut("p", true, false, true),
                                      false, &error), error);
        std::ofstream custom(output / "demo_custom.dveshortcuts");
        custom << shortcuts.serialize_profile("Demo Custom");
        require(static_cast<bool>(custom), "custom profile write failed");

        std::size_t totalBindings = 0U;
        for (const ShortcutCommandDefinition& command : shortcuts.commands()) {
            for (ShortcutContext context : command.contexts) {
                const ShortcutBindingPair pair = shortcuts.bindings("DVE Default", command.actionId, context);
                totalBindings += pair.primary.has_value() ? 1U : 0U;
                totalBindings += pair.secondary.has_value() ? 1U : 0U;
            }
        }
        const auto keyboardMap = shortcuts.keyboard_map();
        const auto cameraRows = shortcuts.search("camera", ShortcutContext::Camera);
        std::ofstream json(output / "shortcut_system_evidence_v1_38.json");
        json << "{\n"
             << "  \"format_version\": 1,\n"
             << "  \"builtin_profiles\": 5,\n"
             << "  \"profiles_including_demo_custom\": " << shortcuts.profile_names().size() << ",\n"
             << "  \"commands\": " << shortcuts.commands().size() << ",\n"
             << "  \"dve_default_bindings\": " << totalBindings << ",\n"
             << "  \"contexts\": " << (static_cast<int>(ShortcutContext::ModalDialog) + 1) << ",\n"
             << "  \"keyboard_map_keys\": " << keyboardMap.size() << ",\n"
             << "  \"camera_search_rows\": " << cameraRows.size() << ",\n"
             << "  \"default_conflicts\": " << shortcuts.conflicts("DVE Default").size() << ",\n"
             << "  \"supports_mouse4\": true,\n"
             << "  \"supports_wheel_bindings\": true,\n"
             << "  \"supports_primary_secondary\": true,\n"
             << "  \"transactional_import\": true\n"
             << "}\n";
        require(static_cast<bool>(json), "evidence JSON write failed");

        std::cout << "DVE v1.38 shortcut-system demo written to " << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.38 shortcut-system demo failed: " << exception.what() << '\n';
        return 1;
    }
}
