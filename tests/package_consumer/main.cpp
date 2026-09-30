// Boots the sample game headless through the installed dve package and renders a few frames.
#include <dve/build_config.hpp>   // #errors if this TU's feature defines differ from the build's
#include <dve/content_source.hpp>
#include <dve/player/frame_image.hpp>
#include <dve/player/player_app.hpp>
#include <dve/player/player_renderer.hpp>
#include <dve/version.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tiny_game <game.dvepak> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? std::stoi(argv[2]) : 10;
    std::printf("dve_version=%s git=%s defines=%s\n", dve::kVersionString, dve::kGitDescribe,
                DVE_BUILD_CONFIG_DEFINES);

    std::string error;
    auto content = dve::open_content_source(argv[1], &error);
    if (!content) {
        std::fprintf(stderr, "content: %s\n", error.c_str());
        return 3;
    }
    auto app = dve::player::PlayerApp::boot(std::move(content), {}, &error);
    if (!app) {
        std::fprintf(stderr, "boot: %s\n", error.c_str());
        return 3;
    }
    // The library and this translation unit must agree on the build options.
    if (dve::player::PlayerApp::scripting_compiled_in() != (DVE_BUILD_CONFIG_DVE_HAVE_LUA == 1)) {
        std::fprintf(stderr, "Lua mismatch between the library and dve/build_config.hpp\n");
        return 1;
    }

    dve::player::MemoryFrameBlitter blitter;
    dve::player::CpuPlayerRendererOptions options;
    options.threadCount = 2;
    auto renderer = dve::player::make_cpu_player_renderer(&blitter, options);
    if (!renderer->resize(96, 54, &error)) {
        std::fprintf(stderr, "resize: %s\n", error.c_str());
        return 1;
    }
    std::vector<dve::GameRenderObject> objects;
    for (int frame = 0; frame < frames; ++frame) {
        app->tick(1.0F / 60.0F);
        if (!renderer->render(app->render_view(objects, 96.0F / 54.0F), &error) || !renderer->present(&error)) {
            std::fprintf(stderr, "render: %s\n", error.c_str());
            return 1;
        }
    }
    const dve::player::Rgba8Image* image = renderer->readback();
    if (image == nullptr || !image->valid() || blitter.frames() != static_cast<std::uint64_t>(frames)) {
        std::fprintf(stderr, "no frame\n");
        return 1;
    }
    std::printf("scene=%s objects=%zu physics=%.*s scripts=%d frames=%d frame_hash=%s\n",
                app->scene_name().c_str(), app->world().object_count(),
                static_cast<int>(app->physics_backend().size()), app->physics_backend().data(),
                app->script_loaded() ? 1 : 0, frames,
                dve::player::format_image_hash(dve::player::hash_image(*image)).c_str());
    if (app->world().object_count() == 0) {
        std::fprintf(stderr, "scene has no objects\n");
        return 1;
    }
    std::printf("dve_package_consumer: PASS\n");
    return 0;
}
