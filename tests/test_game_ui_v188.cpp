#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "dve/editor_asset_browser.hpp"
#include "dve/editor_file_workflow.hpp"
#include "dve/game_ui.hpp"

namespace {
using namespace dve;
using namespace dve::ui;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

std::filesystem::path temporary_root() {
    const auto root = std::filesystem::temp_directory_path() / "dve_v188_game_ui_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

UiAsset make_asset() {
    UiAsset asset;
    asset.name = "Pause Menu";
    asset.canvas = {"Pause Menu", UiCanvasMode::ScreenSpace, {640.0F, 360.0F}};
    UiWidget* root = asset.document.widget(asset.document.root());
    root->layout.direction = UiLayoutDirection::Absolute;
    std::string error;
    const UiWidgetId button = asset.document.add_widget(root->id, UiWidgetKind::Button, "Continue", &error);
    require(button != kInvalidUiWidgetId, error);
    UiWidget* buttonWidget = asset.document.widget(button);
    buttonWidget->layout.position = {20.0F, 20.0F};
    buttonWidget->layout.preferredSize = {140.0F, 40.0F};
    buttonWidget->localizationKey = "menu.continue";
    buttonWidget->accessibilityLabel = "Continue game";

    const UiWidgetId slider = asset.document.add_widget(root->id, UiWidgetKind::Slider, "Volume", &error);
    require(slider != kInvalidUiWidgetId, error);
    UiWidget* sliderWidget = asset.document.widget(slider);
    sliderWidget->layout.position = {20.0F, 80.0F};
    sliderWidget->layout.preferredSize = {200.0F, 40.0F};
    sliderWidget->step = 0.1;
    sliderWidget->value = 0.25;

    const UiWidgetId input = asset.document.add_widget(root->id, UiWidgetKind::TextInput, "PlayerName", &error);
    require(input != kInvalidUiWidgetId, error);
    UiWidget* inputWidget = asset.document.widget(input);
    inputWidget->layout.position = {20.0F, 140.0F};
    inputWidget->layout.preferredSize = {200.0F, 40.0F};

    const UiWidgetId progress = asset.document.add_widget(root->id, UiWidgetKind::ProgressBar, "Health", &error);
    require(progress != kInvalidUiWidgetId, error);
    UiWidget* progressWidget = asset.document.widget(progress);
    progressWidget->layout.position = {20.0F, 200.0F};
    progressWidget->layout.preferredSize = {200.0F, 20.0F};
    progressWidget->binding = "health";
    progressWidget->bindingTarget = UiBindingTarget::Value;

    const UiWidgetId modal = asset.document.add_widget(root->id, UiWidgetKind::Panel, "ConfirmModal", &error);
    require(modal != kInvalidUiWidgetId, error);
    UiWidget* modalWidget = asset.document.widget(modal);
    modalWidget->layout.direction = UiLayoutDirection::Absolute;
    modalWidget->layout.position = {300.0F, 20.0F};
    modalWidget->layout.preferredSize = {200.0F, 160.0F};
    modalWidget->modal = true;
    const UiWidgetId modalButton = asset.document.add_widget(modal, UiWidgetKind::Button, "ModalAccept", &error);
    require(modalButton != kInvalidUiWidgetId, error);
    UiWidget* modalButtonWidget = asset.document.widget(modalButton);
    modalButtonWidget->layout.position = {10.0F, 10.0F};
    modalButtonWidget->layout.preferredSize = {120.0F, 40.0F};
    modalButtonWidget->text = "Accept";
    require(asset.document.validate(&error), error);
    return asset;
}

void test_serialization_and_editor_routing(const std::filesystem::path& root) {
    const UiAsset asset = make_asset();
    const auto pathA = root / "pause.dveui";
    const auto pathB = root / "pause_copy.dveui";
    std::string error;
    require(write_dveui(pathA, asset, &error), error);
    const UiAssetReadResult loaded = read_dveui(pathA);
    require(static_cast<bool>(loaded), loaded.error);
    require(loaded.asset.contentHash == ui_asset_content_hash(loaded.asset),
            "serialized UI asset hash did not round-trip");
    require(loaded.asset.document.find_widget("PlayerName") != kInvalidUiWidgetId,
            "serialized UI widget records did not round-trip");
    require(write_dveui(pathB, loaded.asset, &error), error);
    require(bytes(pathA) == bytes(pathB), "UI serialization is not canonical");

    {
        std::ofstream append(pathB, std::ios::binary | std::ios::app);
        append << "unexpected\n";
    }
    require(!read_dveui(pathB), "UI parser accepted trailing data");
    require(!read_dveui(pathA, 16U), "UI parser ignored its byte bound");
    require(dve::editor::classify_editor_asset(pathA) == dve::editor::EditorAssetKind::Ui,
            "asset browser did not classify .dveui");
    const auto route = dve::editor::classify_editor_file(pathA);
    require(route.supported && route.intent == dve::editor::EditorFileIntent::OpenUiAsset,
            "file workflow did not route .dveui");
}

void test_localization() {
    UiLocalizer localizer;
    std::string error;
    UiLocalizationTable english{"en", "", {
        {"menu.continue", "Continue, {player}"},
        {"inventory", "{{plural:count|one=One item|other={count} items}}"},
        {"pronoun", "{{select:gender|female=She|male=He|other=They}} is ready"},
    }};
    UiLocalizationTable french{"fr", "en", {{"menu.continue", "Continuer, {player}"}}};
    require(localizer.register_table(std::move(english), &error), error);
    require(localizer.register_table(std::move(french), &error), error);
    require(localizer.set_locale("fr", &error), error);
    UiDataModel data;
    data.set("player", std::string("Ada"));
    data.set("count", std::int64_t{3});
    data.set("gender", std::string("female"));
    require(localizer.resolve("menu.continue", data) == "Continuer, Ada", "localized placeholder failed");
    require(localizer.resolve("inventory", data) == "3 items", "locale fallback or plural expansion failed");
    require(localizer.resolve("pronoun", data) == "She is ready", "select expansion failed");
    require(localizer.resolve("missing.key", data) == "missing.key" && localizer.missing_keys().size() == 1U,
            "missing localization key diagnostics failed");
    localizer.set_pseudo_localization(true);
    require(localizer.resolve("menu.continue", data).starts_with("[!! "), "pseudo-localization was not applied");
}

void register_runtime_locales(UiRuntime& runtime) {
    std::string error;
    require(runtime.localizer().register_table({"en", "", {{"menu.continue", "Continue, {player}"}}}, &error), error);
    require(runtime.localizer().register_table({"fr", "en", {{"menu.continue", "Continuer, {player}"}}}, &error), error);
    require(runtime.localizer().set_locale("fr", &error), error);
}

void test_runtime_input_render_and_modal(const std::filesystem::path& root) {
    const auto path = root / "runtime.dveui";
    std::string error;
    require(write_dveui(path, make_asset(), &error), error);
    UiRuntime runtime;
    register_runtime_locales(runtime);
    runtime.data().set("player", std::string("Ada"));
    runtime.data().set("health", 0.5);
    const UiCanvasId canvas = runtime.load_asset(path, &error);
    require(canvas != kInvalidUiCanvasId, error);
    runtime.rebuild();
    const UiDocument* document = runtime.document(canvas);
    require(document != nullptr, "loaded UI asset has no document");
    const UiWidgetId button = document->find_widget("Continue");
    const UiWidgetId slider = document->find_widget("Volume");
    const UiWidgetId input = document->find_widget("PlayerName");
    const UiWidgetId modal = document->find_widget("ConfirmModal");
    const UiWidgetId modalButton = document->find_widget("ModalAccept");
    const auto localized = std::find_if(runtime.draw_commands().begin(), runtime.draw_commands().end(),
        [button](const UiDrawCommand& command) { return command.widget == button; });
    require(localized != runtime.draw_commands().end() && localized->text == "Continuer, Ada",
            "localized text did not reach the render packet");
    const auto progress = std::find_if(runtime.render_primitives().begin(), runtime.render_primitives().end(),
        [](const UiRenderPrimitive& primitive) { return primitive.kind == UiRenderPrimitiveKind::ProgressFill; });
    require(progress != runtime.render_primitives().end() && close(progress->rectangle.width, 100.0F),
            "progress renderer primitive did not normalize its bound value");
    require(std::any_of(runtime.render_primitives().begin(), runtime.render_primitives().end(),
        [](const UiRenderPrimitive& primitive) { return primitive.kind == UiRenderPrimitiveKind::SliderThumb; }),
        "slider renderer primitives were not expanded");

    require(runtime.pointer_down(canvas, 30.0F, 30.0F), "pointer did not press the button");
    require(runtime.captured_widget(canvas) == button && runtime.focused_widget(canvas) == button,
            "pointer focus or capture was not established");
    require(runtime.pointer_up(canvas, 30.0F, 30.0F), "pointer did not release the button");
    auto events = runtime.take_events();
    require(events.size() == 1U && events.front().action == "activate", "button activation event was not emitted");
    auto accessibility = runtime.take_accessibility_events();
    require(accessibility.size() == 1U && std::get<std::string>(accessibility.front().value) == "Continue game",
            "accessible focus event did not carry its authored label");

    require(runtime.pointer_down(canvas, 120.0F, 100.0F), "pointer did not capture the slider");
    require(runtime.captured_widget(canvas) == slider, "slider pointer capture is incorrect");
    require(runtime.pointer_move(canvas, 210.0F, 100.0F), "captured slider did not consume pointer motion");
    require(runtime.pointer_up(canvas, 210.0F, 100.0F), "captured slider did not consume pointer release");
    events = runtime.take_events();
    require(std::any_of(events.begin(), events.end(), [](const UiEvent& event) { return event.action == "change"; }) &&
            std::any_of(events.begin(), events.end(), [](const UiEvent& event) { return event.action == "commit"; }),
            "slider change/commit event sequence is incomplete");

    require(runtime.pointer_down(canvas, 30.0F, 150.0F), "text input did not receive focus");
    require(runtime.pointer_up(canvas, 30.0F, 150.0F), "text input did not release capture");
    require(runtime.text_input(canvas, "Ada ") && runtime.text_input(canvas, "\xC3\xA9"),
            "UTF-8 text input failed");
    require(runtime.key_down(canvas, "Backspace"), "UTF-8 backspace failed");
    require(runtime.document(canvas)->widget(input)->text == "Ada ", "backspace split a UTF-8 code point");

    require(runtime.set_modal_root(canvas, modal), "valid modal root was rejected");
    require(runtime.focused_widget(canvas) == modalButton, "modal scope did not acquire its first focusable child");
    require(!runtime.pointer_down(canvas, 30.0F, 30.0F), "modal scope leaked pointer input to the background");
    require(runtime.action(canvas, "ui.accept"), "controller accept did not activate modal focus");
    events = runtime.take_events();
    require(std::any_of(events.begin(), events.end(), [modalButton](const UiEvent& event) {
        return event.widget == modalButton && event.action == "activate";
    }), "controller action did not target the modal widget");
    require(runtime.action(canvas, "ui.cancel"), "controller cancel did not close the modal");
}

void test_transactional_hot_reload(const std::filesystem::path& root) {
    const auto path = root / "hot_reload.dveui";
    UiAsset asset = make_asset();
    std::string error;
    require(write_dveui(path, asset, &error), error);
    UiRuntime runtime;
    register_runtime_locales(runtime);
    runtime.data().set("player", std::string("Ada"));
    const UiCanvasId canvas = runtime.load_asset(path, &error);
    require(canvas != kInvalidUiCanvasId, error);
    require(runtime.pointer_down(canvas, 30.0F, 30.0F), "could not focus hot-reload widget");
    require(runtime.pointer_up(canvas, 30.0F, 30.0F), "could not release hot-reload widget");
    (void)runtime.take_events();
    const UiWidgetId oldFocus = runtime.focused_widget(canvas);

    UiWidget* button = asset.document.widget(asset.document.find_widget("Continue"));
    button->localizationKey.clear();
    button->text = "Reloaded";
    require(write_dveui(path, asset, &error), error);
    const auto advanced = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5);
    std::filesystem::last_write_time(path, advanced);
    require(runtime.reload_changed_assets() == 1U, "changed UI asset was not hot reloaded");
    require(runtime.focused_widget(canvas) == oldFocus &&
            runtime.document(canvas)->widget(oldFocus)->name == "Continue",
            "hot reload did not preserve focus by stable widget name");
    const auto reloaded = std::find_if(runtime.draw_commands().begin(), runtime.draw_commands().end(),
        [oldFocus](const UiDrawCommand& command) { return command.widget == oldFocus; });
    require(reloaded != runtime.draw_commands().end() && reloaded->text == "Reloaded",
            "hot reload did not publish the new document");

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "broken";
    }
    require(!runtime.reload_asset(canvas, &error), "corrupt UI hot reload was accepted");
    require(runtime.document(canvas)->widget(oldFocus)->text == "Reloaded",
            "failed hot reload mutated the live UI document");
}

} // namespace

int main() {
    try {
        const std::filesystem::path root = temporary_root();
        test_serialization_and_editor_routing(root);
        test_localization();
        test_runtime_input_render_and_modal(root);
        test_transactional_hot_reload(root);
        std::cout << "DVE v1.88 gameplay UI tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE v1.88 gameplay UI tests failed: " << exception.what() << '\n';
        return 1;
    }
}
