#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "dve/camera_runtime.hpp"
#include "dve/game_ui.hpp"
#include "dve/render/cascaded_shadow_map.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::render;
using namespace dve::ui;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "could not read source contract");
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

void test_shadow_writer_reader_origin_and_vulkan_y() {
    CascadedShadowSettings settings;
    settings.cascadeCount = 2U;
    settings.cascadeResolution = 128U;
    settings.maximumDistanceMeters = 100.0F;
    settings.receiverBiasMeters = 0.0F;
    settings.normalBiasMeters = 0.0F;

    const Float3 cameraPositions[]{{0.0F, 2.0F, 5.0F},
                                   {50.0F, 5.0F, 50.0F},
                                   {500.0F, 20.0F, 300.0F}};
    for (Float3 position : cameraPositions) {
        CameraPose camera;
        camera.position = position;
        camera.target = {position.x, position.y - 1.0F, position.z - 8.0F};
        camera.lens.nearPlaneMeters = 0.1F;
        camera.lens.farPlaneMeters = 500.0F;
        const auto plan = make_cascaded_shadow_plan(camera, {0.4F, 0.8F, 0.2F}, settings);
        require(plan.validate(), "shadow plan did not validate");
        const auto& cascade = plan.cascades.front();

        const Float3 points[]{
            cascade.snappedCenter,
            dve::add(cascade.snappedCenter, dve::multiply(cascade.lightUp, cascade.radiusMeters * 0.35F)),
            dve::add(cascade.snappedCenter, dve::multiply(cascade.lightRight, cascade.radiusMeters * -0.2F))};
        for (Float3 point : points) {
            const Float3 clip = cascaded_shadow_caster_clip_coordinate(cascade, point);
            const Float3 sampled = cascaded_shadow_atlas_coordinate(plan, 0U, point);
            const float localU =
                (sampled.x * static_cast<float>(plan.atlasWidth) -
                 static_cast<float>(cascade.atlas.x)) /
                static_cast<float>(cascade.atlas.width);
            const float localV =
                (sampled.y * static_cast<float>(plan.atlasHeight) -
                 static_cast<float>(cascade.atlas.y)) /
                static_cast<float>(cascade.atlas.height);
            require(close(clip.x, localU * 2.0F - 1.0F, 2.0e-4F),
                    "shadow caster X disagrees with sampler");
            require(close(clip.y, localV * 2.0F - 1.0F, 2.0e-4F),
                    "Vulkan caster Y disagrees with sampler V");
            require(close(clip.z, sampled.z, 2.0e-4F),
                    "shadow caster depth origin disagrees with sampler");
            require(clip.z >= 0.0F && clip.z <= 1.0F,
                    "in-cascade caster depth escaped the clip range");
        }

        const float centreDepth = dve::dot(cascade.snappedCenter, cascade.lightForward);
        const float move = cascade.minimumLightDepth - centreDepth - 10.0F;
        const Float3 nearExtruding =
            dve::add(cascade.snappedCenter, dve::multiply(cascade.lightForward, move));
        const Float3 pancaked = cascaded_shadow_caster_clip_coordinate(cascade, nearExtruding);
        require(close(pancaked.z, 0.0F), "near caster was not depth-pancaked");
    }
}

void test_shadow_shader_contracts() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    const std::string caster = read_text(root / "shaders/live_csm_caster_vs.hlsl");
    const std::string resolve = read_text(root / "shaders/cascaded_shadow_resolve.hlsl");
    require(caster.find("dot(world, gCascadeForward.xyz)") != std::string::npos,
            "caster still uses centre-relative depth");
    require(caster.find("-dot(relative, gCascadeUp.xyz)") != std::string::npos,
            "caster does not account for Vulkan positive-height viewport Y");
    require(caster.find("max(depth, 0.0F)") != std::string::npos,
            "caster has no near-depth pancaking");
    require(resolve.find("gCascadedShadowStaticDepth") != std::string::npos &&
            resolve.find("gCascadedShadowDynamicDepth") != std::string::npos,
            "resolve does not declare both shadow layers");
    require(resolve.find("min(staticVisibility, dynamicVisibility)") != std::string::npos &&
            resolve.find("min(nextStaticVisibility, nextDynamicVisibility)") != std::string::npos,
            "resolve does not combine both layers conservatively");
}

struct UiFixture {
    UiRuntime runtime;
    UiCanvasId canvas{};
    UiWidgetId upperLeft{};
    UiWidgetId upperRight{};
    UiWidgetId lowerLeft{};
    UiWidgetId slider{};
    UiWidgetId input{};
    UiWidgetId list{};
    UiWidgetId boundButton{};
};

UiFixture make_ui_fixture() {
    UiFixture fixture;
    fixture.canvas = fixture.runtime.create_canvas(
        {"Controls", UiCanvasMode::ScreenSpace, {640.0F, 360.0F}});
    require(fixture.canvas != kInvalidUiCanvasId, "failed to create UI canvas");
    UiDocument* document = fixture.runtime.document(fixture.canvas);
    require(document != nullptr, "canvas document missing");
    UiWidget* root = document->widget(document->root());
    root->layout.direction = UiLayoutDirection::Absolute;
    std::string error;
    auto add = [&](UiWidgetKind kind, const char* name, UiVec2 position, UiVec2 size) {
        const UiWidgetId id = document->add_widget(root->id, kind, name, &error);
        require(id != kInvalidUiWidgetId, error);
        UiWidget* widget = document->widget(id);
        widget->layout.position = position;
        widget->layout.preferredSize = size;
        return id;
    };
    fixture.upperLeft = add(UiWidgetKind::Button, "UpperLeft", {20.0F, 20.0F}, {100.0F, 40.0F});
    fixture.upperRight = add(UiWidgetKind::Button, "UpperRight", {180.0F, 20.0F}, {100.0F, 40.0F});
    fixture.lowerLeft = add(UiWidgetKind::Button, "LowerLeft", {20.0F, 100.0F}, {100.0F, 40.0F});
    fixture.boundButton = add(UiWidgetKind::Button, "BoundButton", {180.0F, 100.0F}, {100.0F, 40.0F});
    UiWidget* bound = document->widget(fixture.boundButton);
    bound->binding = "bound.enabled";
    bound->bindingTarget = UiBindingTarget::Enabled;
    fixture.runtime.data().set("bound.enabled", true);

    fixture.slider = add(UiWidgetKind::Slider, "Continuous", {340.0F, 20.0F}, {200.0F, 40.0F});
    UiWidget* slider = document->widget(fixture.slider);
    slider->minimum = 0.0;
    slider->maximum = 100.0;
    slider->value = 50.0;
    slider->step = 0.0;

    fixture.input = add(UiWidgetKind::TextInput, "Name", {20.0F, 190.0F}, {240.0F, 40.0F});
    fixture.list = add(UiWidgetKind::List, "Inventory", {320.0F, 190.0F}, {180.0F, 80.0F});
    fixture.runtime.rebuild();
    return fixture;
}

void focus_at(UiFixture& fixture, float x, float y) {
    require(fixture.runtime.pointer_down(fixture.canvas, x, y), "pointer did not acquire focus");
    require(fixture.runtime.pointer_up(fixture.canvas, x, y), "pointer did not release focus");
    (void)fixture.runtime.take_events();
}

void test_ui_navigation_and_text_editing() {
    UiFixture fixture = make_ui_fixture();
    focus_at(fixture, 40.0F, 40.0F);
    require(fixture.runtime.focused_widget(fixture.canvas) == fixture.upperLeft,
            "upper-left button did not focus");
    require(fixture.runtime.key_down(fixture.canvas, "right"), "spatial right failed");
    require(fixture.runtime.focused_widget(fixture.canvas) == fixture.upperRight,
            "spatial right chose the wrong widget");
    require(fixture.runtime.key_down(fixture.canvas, "left"), "spatial left failed");
    require(fixture.runtime.focused_widget(fixture.canvas) == fixture.upperLeft,
            "spatial left did not return");
    require(fixture.runtime.key_down(fixture.canvas, "down"), "spatial down failed");
    require(fixture.runtime.focused_widget(fixture.canvas) == fixture.lowerLeft,
            "spatial down chose document order instead of screen position");

    focus_at(fixture, 440.0F, 40.0F);
    UiWidget* slider = fixture.runtime.document(fixture.canvas)->widget(fixture.slider);
    require(slider && close(static_cast<float>(slider->value), 50.0F),
            "slider midpoint focus changed the value unexpectedly");
    require(fixture.runtime.key_down(fixture.canvas, "right"),
            "continuous slider ignored keyboard input");
    require(close(static_cast<float>(slider->value), 55.0F),
            "continuous slider did not use a range-relative keyboard step");

    focus_at(fixture, 25.0F, 210.0F);
    UiWidget* input = fixture.runtime.document(fixture.canvas)->widget(fixture.input);
    require(input != nullptr, "text input missing");
    require(fixture.runtime.text_input(fixture.canvas, "ab"), "text input rejected printable text");
    require(fixture.runtime.key_down(fixture.canvas, "left"), "caret did not move left");
    (void)fixture.runtime.take_events();
    require(!fixture.runtime.key_down(fixture.canvas, "space"),
            "space activated a focused text input");
    require(fixture.runtime.take_events().empty(), "space emitted an activation event");
    require(fixture.runtime.text_input(fixture.canvas, " "), "space text event was rejected");
    require(fixture.runtime.text_input(fixture.canvas, "x\ny"), "filtered paste was rejected");
    require(input->text == "a x yb", "caret insertion or newline filtering is wrong");
    require(fixture.runtime.key_down(fixture.canvas, "home"), "home key failed");
    require(fixture.runtime.key_down(fixture.canvas, "delete"), "delete key failed");
    require(input->text == " x yb", "delete did not remove the next UTF-8 code point");
    require(fixture.runtime.key_down(fixture.canvas, "end"), "end key failed");
    require(fixture.runtime.key_down(fixture.canvas, "backspace"), "backspace failed");
    require(input->text == " x y", "backspace did not remove the previous code point");
    require(std::any_of(fixture.runtime.render_primitives().begin(),
                        fixture.runtime.render_primitives().end(),
                        [&](const UiRenderPrimitive& primitive) {
                            return primitive.widget == fixture.input &&
                                   primitive.kind == UiRenderPrimitiveKind::TextCaret;
                        }), "focused text input has no caret primitive");
}

void test_lists_and_resolved_enablement() {
    UiFixture fixture = make_ui_fixture();
    require(fixture.runtime.pointer_down(fixture.canvas, 350.0F, 220.0F),
            "list was not hit-testable");
    require(fixture.runtime.pointer_up(fixture.canvas, 350.0F, 220.0F),
            "list click did not complete");
    auto events = fixture.runtime.take_events();
    require(events.size() == 1U && events.front().widget == fixture.list &&
            events.front().action == "activate", "list click emitted no activation");

    focus_at(fixture, 200.0F, 120.0F);
    require(fixture.runtime.focused_widget(fixture.canvas) == fixture.boundButton,
            "bound button did not focus while enabled");
    fixture.runtime.data().set("bound.enabled", false);
    fixture.runtime.rebuild();
    require(!fixture.runtime.key_down(fixture.canvas, "space"),
            "keyboard activated a binding-disabled widget");
    require(fixture.runtime.take_events().empty(),
            "disabled keyboard activation emitted an event");
    require(!fixture.runtime.pointer_down(fixture.canvas, 200.0F, 120.0F),
            "pointer hit-test disagreed with resolved enablement");
}

void test_new_viewport_inherits_accessibility() {
    GameCameraRuntime runtime;
    CameraAccessibilitySettings settings;
    settings.preset = CameraAccessibilityPreset::ReducedMotion;
    settings.motionScale = 0.5F;
    settings.shakeScale = 0.6F;
    settings.bloomScale = 0.4F;
    settings.maximumDepthOfFieldWeight = 0.3F;
    std::string error;
    require(runtime.set_accessibility(settings, &error), error);
    require(runtime.add_viewport({77U, "Accessible", 1U, 0.0F, 0.0F, 1.0F, 1.0F,
                                  "main", true}, &error), error);
    const CameraDirector* director = runtime.director(77U);
    require(director != nullptr, "new viewport director missing");
    require(director->reduced_motion(), "new viewport lost reduced-motion state");
    require(close(director->shake_scale(), 0.12F),
            "new viewport reverted to full camera shake");
}
}

int main() {
    try {
        test_shadow_writer_reader_origin_and_vulkan_y();
        test_shadow_shader_contracts();
        test_ui_navigation_and_text_editing();
        test_lists_and_resolved_enablement();
        test_new_viewport_inherits_accessibility();
        std::cout << "dve_v2294_shadow_ui_fixes_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v2294_shadow_ui_fixes_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
