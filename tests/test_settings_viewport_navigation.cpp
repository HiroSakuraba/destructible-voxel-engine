#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "dve/editor_platform_bridge.hpp"

using namespace dve;
using namespace dve::editor;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(Float3 a, Float3 b) { return std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z) < 1e-4F; }
void set(NativeEditorController& c, std::string_view id, SettingValue value) {
    std::string error;
    if (!c.workspace().settings().set(SettingScope::Session, id, std::move(value), &error))
        throw std::runtime_error(error);
    c.update(0);
}
struct CaptureHost final : platform::IApplicationHost {
    bool captured{}, supported{true}; int attempts{};
    platform::HostBackend backend() const noexcept override { return platform::HostBackend::Headless; }
    bool create_window(const platform::WindowDesc&, std::string*) override { return true; }
    void destroy_window() noexcept override { captured=false; }
    bool has_window() const noexcept override { return true; }
    bool poll_event(platform::PlatformEvent&) override { return false; }
    platform::WindowMetrics window_metrics() const noexcept override { return {}; }
    platform::NativeWindowHandle native_window_handle() const noexcept override { return {}; }
    void set_window_title(std::string_view) override {}
    bool set_relative_mouse_mode(bool enabled, std::string* error = nullptr) override {
        if (enabled) ++attempts;
        if (enabled && !supported) { if (error) *error="unsupported fixture"; return false; }
        captured=enabled; return true;
    }
    bool relative_mouse_mode() const noexcept override { return captured; }
    void set_clipboard_text(std::string_view) override {}
    std::string clipboard_text() const override { return {}; }
    platform::FileDialogToken request_file_dialog(const platform::FileDialogRequest&) override { return 0; }
    std::optional<platform::FileDialogResult> take_file_dialog_result(platform::FileDialogToken) override { return {}; }
    double monotonic_seconds() const noexcept override { return 0; }
    void sleep_for(std::chrono::milliseconds) override {}
};
void profile_bindings() {
    EditorShortcutRegistry r;
    r.install_builtin_commands(); r.install_builtin_profiles();
    const auto original = r.serialize_profile("DVE Default");
    const ShortcutContext contexts[]{ShortcutContext::Viewport};
    struct Case { const char* style; const char* input; bool ctrl, shift, alt; const char* action; };
    const Case cases[]{
        {"dve","mouse2",false,false,false,"viewport.look"},
        {"dve","mouse1",false,false,true,"viewport.orbit"},
        {"dve","mouse3",false,false,false,"viewport.pan"},
        {"dve","mouse2",false,false,true,"viewport.dolly"},
        {"unity","mouse1",true,false,true,"viewport.pan"},
        {"unity","mouse2",false,false,false,"viewport.look"},
        {"unreal","mouse3",false,false,true,"viewport.pan"},
        {"unreal","mouse2",false,false,true,"viewport.dolly"},
        {"blender","mouse3",false,false,false,"viewport.orbit"},
        {"blender","mouse3",false,true,false,"viewport.pan"},
        {"blender","mouse3",true,false,false,"viewport.dolly"},
        {"blender","mouse2",false,false,false,"viewport.look"},
    };
    for (const auto& test : cases) {
        require(r.set_navigation_style(test.style), "profile rejected");
        const auto resolution=r.resolve(mouse_shortcut(test.input,test.ctrl,test.shift,test.alt,ShortcutActivation::Hold),contexts);
        require(resolution && resolution->actionId==test.action,"published gesture routed incorrectly");
        require(r.conflicts("DVE Default").empty(),"navigation preset introduced a conflict");
        require(r.serialize_profile("DVE Default")==original,"navigation preset rewrote authored profile");
    }
    require(!r.set_navigation_style("invalid") && r.navigation_style()=="blender", "invalid style changed map");
    std::string error;
    const auto custom=mouse_shortcut("mouse4",false,false,false,ShortcutActivation::Hold);
    require(r.set_binding("DVE Default","viewport.orbit",ShortcutContext::Viewport,ShortcutSlot::Primary,custom,true,&error),"custom bind failed");
    require(r.set_navigation_style("unity"),"style switch failed");
    require(r.bindings("viewport.orbit",ShortcutContext::Viewport).primary==custom,"style overwrote edited slot");
    require(r.set_binding("DVE Default","viewport.pan",ShortcutContext::Viewport,ShortcutSlot::Primary,std::nullopt,true,&error),"unbind failed");
    require(r.set_navigation_style("blender"),"style switch failed");
    require(!r.bindings("viewport.pan",ShortcutContext::Viewport).primary,"style revived unbound slot");
    require(r.set_binding("DVE Default","viewport.pan",ShortcutContext::Viewport,ShortcutSlot::Secondary,std::nullopt,true,&error),"secondary unbind failed");
    require(r.set_navigation_style("unity"),"style switch failed");
    require(!r.bindings("viewport.pan",ShortcutContext::Viewport).secondary,"style revived explicitly empty secondary slot");
    require(r.duplicate_profile("DVE Default","My profile",&error),"duplicate failed");
    require(r.set_active_profile("My profile",&error),"custom profile failed");
    require(r.bindings("viewport.orbit",ShortcutContext::Viewport).primary==custom,"custom profile not preserved");
    require(r.reset_profile("DVE Default",&error) && r.set_active_profile("DVE Default",&error),"reset failed");
    const auto middle=mouse_shortcut("mouse3",false,false,false,ShortcutActivation::Hold);
    require(r.set_binding("DVE Default","viewport.pan",ShortcutContext::Viewport,ShortcutSlot::Primary,middle,true,&error),"explicit default bind failed");
    require(r.set_navigation_style("blender"),"style switch failed");
    const auto reserved=r.resolve(middle,contexts);
    require(reserved&&reserved->actionId=="viewport.pan","preset stole an explicitly reserved default gesture");
}
void real_gestures() {
    auto c=std::make_unique<NativeEditorController>();
    set(*c,"camera.input_smoothing",0.0);
    const auto vp=c->layout().viewport; const int x=vp.x+vp.width/2,y=vp.y+vp.height/2;
    struct Case { const char* style; PointerButton button; unsigned mods; const char* action; };
    const Case cases[]{
        {"dve",PointerButton::Secondary,0,"look"},
        {"dve",PointerButton::Primary,4,"orbit"},
        {"dve",PointerButton::Auxiliary,0,"pan"},
        {"dve",PointerButton::Secondary,4,"dolly"},
        {"unity",PointerButton::Primary,6,"pan"},
        {"unreal",PointerButton::Auxiliary,4,"pan"},
        {"blender",PointerButton::Auxiliary,0,"orbit"},
        {"blender",PointerButton::Auxiliary,1,"pan"},
        {"blender",PointerButton::Auxiliary,2,"dolly"},
    };
    for(const auto& test:cases) {
        set(*c,"camera.navigation_style",std::string(test.style));
        const auto initial=c->camera();
        c->pointer_down(test.button,x,y,test.mods);
        require(c->navigation_pointer_active(),"gesture did not begin navigation");
        const auto contexts=c->active_shortcut_contexts();
        const bool fly=std::find(contexts.begin(),contexts.end(),ShortcutContext::FlyNavigation)!=contexts.end();
        require(fly==(std::string_view(test.action)=="look"),"orbit/pan/dolly accidentally enabled fly keys");
        c->pointer_move(x+20,y+15,test.mods);
        require(!near(initial.position,c->camera().position)||!near(initial.target,c->camera().target),"gesture did not move camera");
        if(std::string_view(test.action)=="orbit") require(near(initial.target,c->camera().target),"orbit moved pivot");
        if(std::string_view(test.action)=="look") require(near(initial.position,c->camera().position),"look translated camera");
        if(std::string_view(test.action)=="pan")
            require(near(subtract(c->camera().position,initial.position),subtract(c->camera().target,initial.target)),"pan changed viewing direction");
        c->pointer_up(test.button,x+20,y+15,test.mods);
        require(!c->navigation_pointer_active(),"release retained navigation");
    }
    c->pointer_down(PointerButton::Secondary,x,y);
    c->key_down("w",false,false,false); c->update(0.1F);
    const auto pose=c->camera();
    set(*c,"camera.navigation_style",std::string("dve"));
    require(!c->navigation_pointer_active() && near(pose.position,c->camera().position),"profile switch moved pose or retained capture");
    c->update(0.1F);
    require(near(pose.position,c->camera().position),"profile switch left held fly movement");
    std::string error;
    auto& shortcuts=c->workspace().shortcuts();
    require(shortcuts.duplicate_profile("DVE Default","User navigation",&error),"custom navigation profile failed");
    require(shortcuts.set_active_profile("User navigation",&error),"custom navigation selection failed");
    set(*c,"camera.navigation_style",std::string("blender"));
    require(shortcuts.active_profile()=="User navigation","navigation setting replaced active custom profile");
}
void capture_lifecycle() {
    auto c=std::make_unique<NativeEditorController>();
    set(*c,"camera.input_smoothing",0.0);
    CaptureHost host;
    {
        EditorPlatformBridge b(*c,&host); b.set_ui_zoom(2);
        const auto vp=c->layout().viewport;
        platform::PlatformEvent down; down.type=platform::EventType::PointerButtonDown;
        down.button=platform::PointerButton::Secondary; down.x=(vp.x+vp.width/2)*2; down.y=(vp.y+vp.height/2)*2;
        auto selection=down; selection.button=platform::PointerButton::Primary;
        b.handle_event(selection); require(!host.captured,"ordinary selection captured pointer");
        selection.type=platform::EventType::PointerButtonUp; b.handle_event(selection);
        b.handle_event(down); require(host.captured,"viewport look did not capture");
        const auto initial=c->camera();
        platform::PlatformEvent motion; motion.type=platform::EventType::PointerMove;
        motion.relativeMotion=true; motion.deltaX=0.25F; motion.deltaY=-0.5F;
        b.handle_event(motion);
        require(!near(initial.target,c->camera().target),"fractional raw deltas disappeared at UI zoom");
        platform::PlatformEvent escape; escape.type=platform::EventType::KeyDown; escape.key="escape";
        b.handle_event(escape); require(!host.captured&&!c->navigation_pointer_active(),"Escape retained capture");
        const auto released=c->camera(); b.handle_event(motion);
        require(near(released.target,c->camera().target),"stale raw event moved released camera");
        b.handle_event(down);
        platform::PlatformEvent lost; lost.type=platform::EventType::WindowFocusLost;
        b.handle_event(lost); require(!host.captured&&!c->navigation_pointer_active(),"focus loss retained navigation");
        b.handle_event(down); c->open_settings(SettingScope::User); b.sync_pointer_capture();
        require(!host.captured&&!c->navigation_pointer_active(),"modal retained capture");
        c->close_settings(false);
        b.handle_event(down); set(*c,"input.raw_mouse",false); b.sync_pointer_capture();
        require(!host.captured&&!c->navigation_pointer_active(),"raw setting change retained capture");
        b.handle_event(down); require(!host.captured&&c->navigation_pointer_active(),"ordinary mode disabled navigation");
        c->open_settings(SettingScope::User); b.sync_pointer_capture(); c->close_settings(false);
        require(!c->navigation_pointer_active(),"ordinary mode resumed held navigation after modal");
        c->clear_navigation_input(); set(*c,"input.raw_mouse",true);
        host.supported=false; const int attempts=host.attempts;
        b.handle_event(down); for(int i=0;i<10;++i)b.update(0.01F);
        require(!host.captured&&host.attempts==attempts+1,"failed capture retried every frame");
        motion.relativeMotion=false; motion.x=down.x+40; motion.y=down.y+20;
        const auto fallback=c->camera(); b.handle_event(motion);
        require(!near(fallback.target,c->camera().target),"unsupported capture lost ordinary fallback");
        c->clear_navigation_input(); b.sync_pointer_capture(); host.supported=true; b.handle_event(down);
        require(host.captured,"new gesture did not retry supported capture");
    }
    require(!host.captured,"bridge destruction leaked capture");
}
}
int main() {
    try { profile_bindings(); real_gestures(); capture_lifecycle(); std::cout<<"viewport navigation: PASS\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
