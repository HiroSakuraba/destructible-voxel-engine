// SDL canvas UI-zoom contract (compiled against the deterministic SDL shim, so it runs
// without a display): debug-text fallback magnification and logical text metrics.
#include "dve/editor_sdl_canvas.hpp"
#include "dve/editor_ui_zoom.hpp"
#include <SDL3/SDL.h>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace dve::editor;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        require(sdl_debug_text_magnification(1.0F) == 1, "100% keeps the native 8x8 debug font");
        require(sdl_debug_text_magnification(1.25F) == 2 && sdl_debug_text_magnification(1.5F) == 2 &&
                sdl_debug_text_magnification(1.75F) == 2, "125-175% use 2x debug glyphs");
        require(sdl_debug_text_magnification(2.0F) == 3, "200% uses 3x debug glyphs (24 px)");
        require(sdl_debug_text_magnification(4.0F) == 6, "HiDPI 200% scales further");

        // An invalid window handle still yields a canvas object whose metrics can be queried.
        SdlEditorCanvas canvas(dve::platform::NativeWindowHandle{});
        std::string error;
        require(!canvas.set_vsync(true, &error) && !error.empty(), "invalid canvas accepted vsync");
        SDLTest_Reset();
        SDL_Window* window = SDL_CreateWindow("vsync test", 640, 480, 0);
        {
            SdlEditorCanvas validCanvas({dve::platform::HostBackend::SDL3, window});
            require(validCanvas.set_vsync(true, &error) && SDLTest_RenderVSync() == 1,
                    "enabled vsync did not reach SDL renderer");
            require(validCanvas.set_vsync(false, &error) && SDLTest_RenderVSync() == 0,
                    "disabled vsync did not reach SDL renderer");
            SDLTest_SetVSyncSupported(false);
            error.clear();
            require(!validCanvas.set_vsync(true, &error) && !error.empty() && validCanvas.valid(),
                    "unsupported vsync did not report failure without destroying canvas");
            require(SDLTest_RenderVSyncCalls() == 3, "unexpected vsync setter calls");
        }
        SDL_DestroyWindow(window);
        require(!canvas.valid(), "canvas without a window must be invalid");
        require(!canvas.scalable_text() && canvas.text_backend() == "debug_text",
                "contract build has no SDL_ttf and must report the debug-text fallback");
        canvas.set_ui_zoom(1.0F);
        require(canvas.text_width("abcd") == 32, "100% width = 4 glyphs x 8 px");
        canvas.set_ui_zoom(2.0F);
        require(canvas.render_scale() == 2.0F, "render scale follows zoom");
        require(canvas.text_width("abcd") == 48, "200% width = 4 x 24 physical px / 2 = 48 logical px");
        canvas.set_ui_zoom(1.5F, 2.0F);
        require(canvas.render_scale() == 3.0F, "render scale combines zoom and pixel density");
        canvas.set_ui_zoom(9.0F);
        require(canvas.render_scale() == kUiZoomMax, "canvas clamps zoom to the shared maximum");
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_sdl_canvas_zoom_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "dve_editor_sdl_canvas_zoom_tests: PASS\n";
    return 0;
}
