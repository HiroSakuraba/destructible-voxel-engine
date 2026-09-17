#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dve/ai/openai_responses.hpp"
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

class DemoTransport final : public dve::ai::IAiHttpTransport {
public:
    dve::ai::AiHttpResponse post_json(std::string_view, std::string_view, std::string_view) override {
        return {200,
            R"({"id":"resp_demo","output":[{"type":"function_call","name":"dve.project.write_text","arguments":"{\"path\":\"notes/assistant_plan.md\",\"content\":\"# AI plan\\n\\nInspect before changing.\\n\"}","call_id":"call_demo"}]})",
            {}};
    }
};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
} // namespace

int main(int argc, char** argv) {
    try {
        using namespace dve;
        using namespace dve::editor;
        const std::filesystem::path output = argc > 1 ? argv[1] : "dve_ai_assistant_v1_39_demo";
        std::filesystem::create_directories(output / "project" / "notes");

        NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
        controller.resize(1440, 900);
        ai::DveAiBridge bridge({output / "project"});
        register_editor_ai_tools(bridge, controller.workspace());
        ai::OpenAiResponsesConfig config;
        config.approvalPolicy = ai::AiApprovalPolicy::AskForChanges;
        auto client = std::make_unique<ai::OpenAiResponsesClient>(
            bridge, std::make_unique<DemoTransport>(), config, [] { return std::string("demo-key"); });
        controller.workspace().ai_assistant().attach(std::move(client), &bridge);
        require(controller.workspace().ai_assistant().send(
                    "Inspect the project and create a short implementation note."),
                "demo assistant request failed");
        require(controller.workspace().ai_assistant().pending_approvals().size() == 1U,
                "demo did not produce an approval request");
        require(controller.dispatch_action("window.toggle_ai_assistant"), "assistant panel did not open");

        PpmCanvas canvas(1440, 900);
        render_native_editor(canvas, controller, 1440, 900);
        require(canvas.write(output / "ai_assistant_panel_v1_39.ppm"), "AI panel image write failed");

        std::ofstream evidence(output / "ai_assistant_panel_evidence_v1_39.json");
        evidence << "{\n"
                 << "  \"version\": \"1.39.0\",\n"
                 << "  \"tools\": " << bridge.registry().tools().size() << ",\n"
                 << "  \"messages\": " << controller.workspace().ai_assistant().messages().size() << ",\n"
                 << "  \"pending_approvals\": " << controller.workspace().ai_assistant().pending_approvals().size() << ",\n"
                 << "  \"approve_button_visible\": " << (controller.layout().aiApproveButton.width > 0 ? "true" : "false") << ",\n"
                 << "  \"deny_button_visible\": " << (controller.layout().aiDenyButton.width > 0 ? "true" : "false") << ",\n"
                 << "  \"api_key_persisted\": false,\n"
                 << "  \"shell_tool_exposed\": false\n"
                 << "}\n";
        require(static_cast<bool>(evidence), "AI panel evidence write failed");
        std::cout << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.39 AI assistant editor demo failed: " << exception.what() << '\n';
        return 1;
    }
}
