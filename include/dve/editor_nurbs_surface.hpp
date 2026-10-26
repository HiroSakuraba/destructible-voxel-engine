#pragma once
#include "dve/editor_nurbs.hpp"
#include "dve/nurbs_trimming.hpp"
namespace dve::editor {
inline constexpr std::string_view kNurbsSurfaceFeatures="surface_features";
struct NurbsSurfaceCurve {
    std::uint64_t id{};
    NurbsSource uv; // rational curve, x/y are normalized UV, z == 0
    EditorObjectId projectionSource{}; // retained source relationship; 0 = authored UV
    unsigned samples{65};
    bool operator==(const NurbsSurfaceCurve&)const=default;
};
struct NurbsSurfaceFeatures {
    std::vector<NurbsSurfaceCurve> curves;
    std::optional<NurbsTrimRegion> trim;
    bool operator==(const NurbsSurfaceFeatures&)const=default;
};
[[nodiscard]] std::string encode_nurbs_surface_features(const NurbsSurfaceFeatures&);
[[nodiscard]] std::optional<NurbsSurfaceFeatures> decode_nurbs_surface_features(std::string_view,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSurfaceFeatures> editor_nurbs_surface_features(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] bool validate_nurbs_surface_features(const NurbsSurfaceFeatures&,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSurfaceCurve> project_editor_nurbs_surface_curve(const EditorDocument&,EditorObjectId surface,EditorObjectId curve,std::uint64_t curveId,unsigned samples=65,std::string* error=nullptr);
[[nodiscard]] std::optional<std::vector<NurbsUV>> sample_nurbs_uv_curve(const NurbsSurfaceCurve&,bool closed,std::string* error=nullptr);
// Derived on demand: each UV sample is evaluated on the current surface; guide
// segments are display approximations, never substituted for surface geometry.
[[nodiscard]] std::optional<std::vector<Float3>> evaluate_editor_nurbs_surface_curve(const EditorDocument&,EditorObjectId,std::uint64_t,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsDisplayGrid> editor_nurbs_surface_grid(const EditorObject&,const NurbsSource&,std::string*,std::stop_token={});
// Staging helper: pass an isolated candidate document. A failed rebuild may
// modify that candidate; feature commands publish only after full validation.
[[nodiscard]] bool rebuild_editor_nurbs_network_display(EditorDocument&,EditorObjectId,std::string* error=nullptr);
[[nodiscard]] std::unique_ptr<IEditorCommand> set_nurbs_surface_features_command(EditorObjectId,const NurbsSurfaceFeatures& before,const NurbsSurfaceFeatures& after);
[[nodiscard]] std::unique_ptr<IEditorCommand> project_nurbs_surface_curve_command(EditorObjectId surface,EditorObjectId curve,unsigned samples=65);
[[nodiscard]] std::unique_ptr<IEditorCommand> refresh_nurbs_surface_curve_command(EditorObjectId,std::uint64_t curve);
[[nodiscard]] std::unique_ptr<IEditorCommand> trim_nurbs_surface_command(EditorObjectId,std::optional<NurbsTrimRegion>);
[[nodiscard]] std::unique_ptr<IEditorCommand> trim_nurbs_surface_curve_command(EditorObjectId,std::uint64_t curve);
[[nodiscard]] std::unique_ptr<IEditorCommand> select_nurbs_trim_region_command(EditorObjectId,NurbsUV,bool add=false);
}
