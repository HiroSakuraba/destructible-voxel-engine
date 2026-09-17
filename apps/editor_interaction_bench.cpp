#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#include "dve/editor_native.hpp"

namespace {
using namespace dve;
using namespace dve::editor;
using Clock = std::chrono::steady_clock;

struct Percentiles { double p50{}, p95{}, p99{}, maximum{}; };

Percentiles summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    auto sample = [&](double q) {
        if (values.empty()) return 0.0;
        const std::size_t index = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1));
        return values[index];
    };
    return {sample(0.50), sample(0.95), sample(0.99), values.empty() ? 0.0 : values.back()};
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path output = argc > 1 ? argv[1] : "editor_interaction_benchmark_v1_13.json";
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(1600, 900);
        controller.frame_selection();

        std::mt19937 generator(12012U);
        std::uniform_int_distribution<int> x(controller.layout().viewport.x,
                                              controller.layout().viewport.x + controller.layout().viewport.width - 1);
        std::uniform_int_distribution<int> y(controller.layout().viewport.y,
                                              controller.layout().viewport.y + controller.layout().viewport.height - 1);
        std::vector<double> pickTimes;
        pickTimes.reserve(500);
        std::size_t hits = 0;
        for (int i = 0; i < 500; ++i) {
            const auto start = Clock::now();
            const auto hit = pick_editor_document(controller.workspace().document(),
                make_viewport_ray(controller.camera(), controller.layout().viewport,
                                  static_cast<float>(x(generator)), static_cast<float>(y(generator))));
            const auto end = Clock::now();
            pickTimes.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            if (hit) ++hits;
        }

        std::vector<double> listTimes;
        listTimes.reserve(50);
        std::size_t drawItems = 0;
        for (int i = 0; i < 50; ++i) {
            const auto start = Clock::now();
            const auto items = controller.draw_items();
            const auto end = Clock::now();
            drawItems = items.size();
            listTimes.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }

        std::vector<double> searchTimes;
        searchTimes.reserve(1000);
        std::size_t searchResults = 0;
        static constexpr std::string_view searchQueries[]{
            "move", "rotate", "save", "collision", "anchor", "viewport", "scale", "simulate"};
        for (int i = 0; i < 1000; ++i) {
            const auto start = Clock::now();
            const auto matches = controller.workspace().menus().search(
                searchQueries[static_cast<std::size_t>(i) % std::size(searchQueries)], 8);
            const auto end = Clock::now();
            searchResults += matches.size();
            searchTimes.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }

        if (!controller.dispatch_action("edit.select_all"))
            throw std::runtime_error("select-all action failed");
        std::vector<double> diagnosticTimes;
        diagnosticTimes.reserve(200);
        std::uint64_t diagnosedVoxels = 0;
        for (int i = 0; i < 200; ++i) {
            const auto start = Clock::now();
            const EditorSelectionDiagnostics diagnostics = controller.selection_diagnostics();
            const auto end = Clock::now();
            diagnosedVoxels = diagnostics.occupiedVoxels;
            diagnosticTimes.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }

        std::vector<double> transformTimes;
        transformTimes.reserve(200);
        for (int i = 0; i < 200; ++i) {
            const auto start = Clock::now();
            const CommandResult move = controller.nudge_selection({0.1F, 0.0F, 0.0F});
            const CommandResult undo = controller.workspace().commands().undo(
                controller.workspace().document());
            const auto end = Clock::now();
            if (!move.success || !undo.success)
                throw std::runtime_error("batch transform benchmark command failed");
            transformTimes.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }

        const Percentiles pick = summarize(std::move(pickTimes));
        const Percentiles build = summarize(std::move(listTimes));
        const Percentiles search = summarize(std::move(searchTimes));
        const Percentiles diagnostics = summarize(std::move(diagnosticTimes));
        const Percentiles transform = summarize(std::move(transformTimes));
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("could not open benchmark output");
        stream << std::fixed << std::setprecision(4)
               << "{\n"
               << "  \"objects\": " << controller.workspace().document().objects().size() << ",\n"
               << "  \"occupied_voxels\": ";
        std::uint64_t voxels = 0;
        for (const auto& [id, object] : controller.workspace().document().objects()) {
            (void)id;
            voxels += object.voxels->occupied_voxel_count();
        }
        stream << voxels << ",\n"
               << "  \"draw_items\": " << drawItems << ",\n"
               << "  \"pick_queries\": 500,\n"
               << "  \"pick_hits\": " << hits << ",\n"
               << "  \"pick_us\": {\"p50\": " << pick.p50 << ", \"p95\": " << pick.p95
               << ", \"p99\": " << pick.p99 << ", \"max\": " << pick.maximum << "},\n"
               << "  \"draw_list_us\": {\"p50\": " << build.p50 << ", \"p95\": " << build.p95
               << ", \"p99\": " << build.p99 << ", \"max\": " << build.maximum << "},\n"
               << "  \"command_search_queries\": 1000,\n"
               << "  \"command_search_results\": " << searchResults << ",\n"
               << "  \"command_search_us\": {\"p50\": " << search.p50 << ", \"p95\": " << search.p95
               << ", \"p99\": " << search.p99 << ", \"max\": " << search.maximum << "},\n"
               << "  \"diagnosed_voxels\": " << diagnosedVoxels << ",\n"
               << "  \"selection_diagnostics_us\": {\"p50\": " << diagnostics.p50 << ", \"p95\": " << diagnostics.p95
               << ", \"p99\": " << diagnostics.p99 << ", \"max\": " << diagnostics.maximum << "},\n"
               << "  \"batch_translate_undo_us\": {\"p50\": " << transform.p50 << ", \"p95\": " << transform.p95
               << ", \"p99\": " << transform.p99 << ", \"max\": " << transform.maximum << "}\n"
               << "}\n";
        std::cout << "dve_editor_interaction_bench: PASS\n" << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_interaction_bench: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
