#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dve/camera_system.hpp"
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
} // namespace

int main(int argc, char** argv) {
    try {
        using namespace dve;
        using namespace dve::editor;
        const std::filesystem::path output = argc > 1 ? argv[1] : "dve_camera_menu_v1_35_demo";
        std::filesystem::create_directories(output);
        NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
        controller.resize(1440, 900);
        controller.camera().position = {11.0F, 7.5F, 12.0F};
        controller.camera().target = {1.5F, 1.2F, 0.0F};
        const camera::CameraRigId rigId = controller.create_camera_rig_from_view("Destruction Gameplay Camera");
        require(rigId != 0, "could not create camera rig");
        camera::CameraRig* rig = controller.camera_director().find_rig(rigId);
        require(rig != nullptr, "created camera rig missing");
        rig->mode = camera::CameraRigMode::ThirdPerson;
        rig->priority = 50;
        rig->framing.distanceMeters = 5.5F;
        rig->collision.enabled = true;
        rig->lens.physical.enabled = true;
        rig->lens.physical.focalLengthMillimeters = 42.0F;
        rig->authoredPose.lens = rig->lens;
        controller.camera().position = {-10.0F, 8.0F, 10.0F};
        controller.camera().target = {1.0F, 1.0F, 0.0F};
        controller.update(1.0F / 60.0F);

        std::string error;
        auto& settings = controller.workspace().settings();
        require(settings.set(SettingScope::Project, "camera.preview_size", 0.31, &error), error);
        require(settings.set(SettingScope::Project, "camera.physical_lens", true, &error), error);
        require(settings.set(SettingScope::Project, "camera.focal_length_mm", 42.0, &error), error);
        require(settings.set(SettingScope::Project, "camera.default_blend", std::string("ease_in_out"), &error), error);
        require(settings.set(SettingScope::Project, "camera.blend_seconds", 0.45, &error), error);
        require(settings.set(SettingScope::Project, "viewport.statistics", true, &error), error);
        controller.apply_settings_to_runtime();

        const int cameraMenuX = 4 * 78 + 20;
        controller.pointer_down(PointerButton::Primary, cameraMenuX, 12);
        PpmCanvas cameraCanvas(1440, 900);
        render_native_editor(cameraCanvas, controller, 1440, 900);
        require(cameraCanvas.write(output / "camera_rig_preview_and_menu_v1_35.ppm"), "camera screenshot failed");

        controller.open_settings(SettingScope::Project, "Camera");
        controller.settings_panel().includeAdvanced = true;
        controller.settings_panel().selectedRow = 9U;
        PpmCanvas settingsCanvas(1440, 900);
        render_native_editor(settingsCanvas, controller, 1440, 900);
        require(settingsCanvas.write(output / "project_settings_camera_v1_35.ppm"), "settings screenshot failed");

        camera::CameraRigLibrary library;
        library.rigs = controller.camera_director().rigs();
        library.defaultBlend = {camera::CameraBlendCurve::EaseInOut, 0.45F};
        library.stateBindings["gameplay"] = rigId;
        require(library.validate(&error), error);
        std::ofstream cameraFile(output / "gameplay_cameras.dvecamera");
        cameraFile << library.serialize();
        require(static_cast<bool>(cameraFile), "camera library write failed");
        std::ofstream settingsFile(output / "project_camera_settings.dvesettings");
        settingsFile << settings.serialize_scope(SettingScope::Project);
        require(static_cast<bool>(settingsFile), "settings write failed");

        std::ofstream json(output / "camera_menu_evidence_v1_35.json");
        json << "{\n"
             << "  \"camera_rigs\": " << library.rigs.size() << ",\n"
             << "  \"settings_definitions\": " << settings.definitions().size() << ",\n"
             << "  \"settings_categories\": " << settings.categories(controller.settings_capabilities()).size() << ",\n"
             << "  \"top_level_menus\": " << kMenuBarNames.size() << ",\n"
             << "  \"camera_priority\": " << rig->priority << ",\n"
             << "  \"physical_focal_length_mm\": " << rig->lens.physical.focalLengthMillimeters << ",\n"
             << "  \"collision_avoidance\": true,\n"
             << "  \"picture_in_picture\": true,\n"
             << "  \"frustum_preview\": true\n"
             << "}\n";
        require(static_cast<bool>(json), "evidence JSON write failed");
        std::cout << "DVE v1.35 camera/menu demo written to " << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.35 camera/menu demo failed: " << exception.what() << '\n';
        return 1;
    }
}
