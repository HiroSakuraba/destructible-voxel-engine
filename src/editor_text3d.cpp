#include "dve/editor_text3d.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace dve::editor {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool relative_is_confined(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative) if (component == "..") return false;
    return true;
}

std::filesystem::path canonical_existing_or_parent(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

std::vector<std::filesystem::path> candidate_license_files(const std::filesystem::path& font) {
    const auto parent = font.parent_path();
    const auto stem = font.stem().string();
    return {
        std::filesystem::path(font.string() + ".license"),
        std::filesystem::path(font.string() + ".license.txt"),
        parent / (stem + ".LICENSE"),
        parent / (stem + ".LICENSE.txt"),
        parent / (stem + ".license.txt"),
        parent / "LICENSE",
        parent / "LICENSE.txt",
        parent / "OFL.txt",
    };
}
} // namespace

Text3DFontDependencyReport inspect_text3d_font_dependency(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& fontPath) {
    Text3DFontDependencyReport report;
    report.requestedPath = fontPath;
    const auto root = canonical_existing_or_parent(projectRoot.empty() ? "." : projectRoot);
    const auto combined = fontPath.is_absolute() ? fontPath : root / fontPath;
    report.resolvedPath = canonical_existing_or_parent(combined);
    std::error_code ec;
    report.projectRelativePath = std::filesystem::relative(report.resolvedPath, root, ec);
    report.insideProject = !ec && relative_is_confined(report.projectRelativePath);
    report.exists = std::filesystem::is_regular_file(report.resolvedPath, ec) && !ec;
    report.supportedTrueType = lower(report.resolvedPath.extension().string()) == ".ttf";
    std::set<std::filesystem::path> unique;
    for (const auto& candidate : candidate_license_files(report.resolvedPath)) {
        if (std::filesystem::is_regular_file(candidate, ec) && !ec && unique.insert(candidate).second)
            report.licenseFiles.push_back(candidate);
        ec.clear();
    }
    report.licenseVisible = !report.licenseFiles.empty();
    if (!report.exists) report.diagnostics.push_back("Font file does not exist.");
    if (!report.insideProject) report.diagnostics.push_back("Font must be inside the project root before packaging.");
    if (!report.supportedTrueType) report.diagnostics.push_back("v1.50 authoring currently supports glyf-based .ttf fonts only.");
    if (!report.licenseVisible) report.diagnostics.push_back("No adjacent font license file was found; packaging must show this warning.");
    report.packageReady = report.exists && report.insideProject && report.supportedTrueType && report.licenseVisible;
    return report;
}

std::vector<Text3DFontDependencyReport> collect_text3d_font_dependencies(
    const EditorDocument& document, const std::filesystem::path& projectRoot) {
    std::vector<Text3DFontDependencyReport> result;
    std::set<std::filesystem::path> seen;
    for (const auto& [id, object] : document.objects()) {
        (void)id;
        if (!object.text3d || object.textFontAsset.empty()) continue;
        const auto report = inspect_text3d_font_dependency(projectRoot, object.textFontAsset);
        const auto key = report.resolvedPath.lexically_normal();
        if (seen.insert(key).second) result.push_back(report);
    }
    return result;
}

Text3DFontPackagingResult package_text3d_font_dependencies(
    const EditorDocument& document, const std::filesystem::path& projectRoot,
    const std::filesystem::path& packageRoot, bool requireVisibleLicense) {
    Text3DFontPackagingResult result;
    const auto root = canonical_existing_or_parent(projectRoot.empty() ? "." : projectRoot);
    const auto destinationRoot = canonical_existing_or_parent(packageRoot);
    std::error_code ec;
    std::filesystem::create_directories(destinationRoot, ec);
    if (ec) { result.error = "Could not create 3D text package root: " + ec.message(); return result; }
    for (const auto& dependency : collect_text3d_font_dependencies(document, root)) {
        if (!dependency.exists || !dependency.insideProject || !dependency.supportedTrueType) {
            result.error = dependency.diagnostics.empty()
                ? "A 3D text font dependency is not packageable" : dependency.diagnostics.front();
            return result;
        }
        if (requireVisibleLicense && !dependency.licenseVisible) {
            result.error = "Font dependency has no visible license file: " +
                           dependency.projectRelativePath.generic_string();
            return result;
        }
        std::vector<std::filesystem::path> sources{dependency.resolvedPath};
        sources.insert(sources.end(), dependency.licenseFiles.begin(), dependency.licenseFiles.end());
        for (const auto& source : sources) {
            const auto canonicalSource = canonical_existing_or_parent(source);
            const auto relative = std::filesystem::relative(canonicalSource, root, ec);
            if (ec || !relative_is_confined(relative)) {
                result.error = "3D text dependency escapes the project root: " + source.generic_string();
                return result;
            }
            const auto destination = destinationRoot / relative;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec) { result.error = "Could not create dependency directory: " + ec.message(); return result; }
            std::filesystem::copy_file(canonicalSource, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { result.error = "Could not package 3D text dependency: " + ec.message(); return result; }
            result.copiedFiles.push_back(relative);
        }
        if (!dependency.licenseVisible)
            result.warnings.push_back("Font license was not visible for " + dependency.projectRelativePath.generic_string());
    }
    std::sort(result.copiedFiles.begin(), result.copiedFiles.end());
    result.copiedFiles.erase(std::unique(result.copiedFiles.begin(), result.copiedFiles.end()),
                             result.copiedFiles.end());
    result.success = true;
    return result;
}

bool EditorText3DAuthoringSession::confined_path(const std::filesystem::path& root,
                                                  const std::filesystem::path& path,
                                                  std::filesystem::path& resolved,
                                                  std::filesystem::path& relative) {
    const auto canonicalRoot = canonical_existing_or_parent(root.empty() ? "." : root);
    resolved = canonical_existing_or_parent(path.is_absolute() ? path : canonicalRoot / path);
    std::error_code ec;
    relative = std::filesystem::relative(resolved, canonicalRoot, ec);
    return !ec && relative_is_confined(relative);
}

bool EditorText3DAuthoringSession::begin_create(std::filesystem::path projectRoot,
                                                 std::filesystem::path fontPath,
                                                 std::string textUtf8,
                                                 Text3DCookOptions options) {
    cancel();
    active_ = true;
    projectRoot_ = std::move(projectRoot);
    fontPath_ = std::move(fontPath);
    textUtf8_ = std::move(textUtf8);
    options_ = options;
    options_.objectId = 1U;
    outputAssetPath_ = std::filesystem::path("assets/text/untitled.dtext");
    return refresh();
}

bool EditorText3DAuthoringSession::begin_edit(std::filesystem::path projectRoot,
                                               const EditorObject& object) {
    cancel();
    if (!object.text3d) return false;
    active_ = true;
    targetObject_ = object.id;
    projectRoot_ = std::move(projectRoot);
    fontPath_ = object.textFontAsset;
    outputAssetPath_ = object.sourceAsset;
    textUtf8_ = object.text3d->textUtf8;
    options_.objectId = object.id;
    options_.style = object.text3d->style;
    before_.asset = object.text3d;
    before_.fontAsset = object.textFontAsset;
    before_.sourceAsset = object.sourceAsset;
    return refresh();
}

void EditorText3DAuthoringSession::cancel() noexcept {
    active_ = false;
    targetObject_.reset();
    projectRoot_.clear();
    fontPath_.clear();
    outputAssetPath_.clear();
    textUtf8_.clear();
    options_ = {};
    before_ = {};
    preview_ = {};
}

void EditorText3DAuthoringSession::set_font_path(std::filesystem::path path) { fontPath_ = std::move(path); }
void EditorText3DAuthoringSession::set_output_asset_path(std::filesystem::path path) { outputAssetPath_ = std::move(path); }
void EditorText3DAuthoringSession::set_text(std::string textUtf8) { textUtf8_ = std::move(textUtf8); }
void EditorText3DAuthoringSession::set_style(Text3DStyle style) { options_.style = style; }
void EditorText3DAuthoringSession::set_face_material(std::uint32_t id, Float4 color) {
    options_.style.faceMaterialId = id; options_.style.faceColor = color;
}
void EditorText3DAuthoringSession::set_side_material(std::uint32_t id, Float4 color) {
    options_.style.sideMaterialId = id; options_.style.sideColor = color;
}

bool EditorText3DAuthoringSession::refresh() {
    preview_ = {};
    if (!active_) { preview_.error = "3D text authoring session is not active"; return false; }
    preview_.dependency = inspect_text3d_font_dependency(projectRoot_, fontPath_);
    if (!preview_.dependency.exists || !preview_.dependency.insideProject ||
        !preview_.dependency.supportedTrueType) {
        preview_.error = preview_.dependency.diagnostics.empty()
            ? "Font dependency is not usable" : preview_.dependency.diagnostics.front();
        return false;
    }
    Text3DCookOptions cookOptions = options_;
    cookOptions.objectId = targetObject_.value_or(1U);
    const auto cooked = cook_text3d(preview_.dependency.resolvedPath, textUtf8_, cookOptions);
    if (!cooked) { preview_.error = cooked.error; return false; }
    preview_.asset = cooked.asset;
    preview_.warnings = cooked.warnings;
    preview_.warnings.insert(preview_.warnings.end(), preview_.dependency.diagnostics.begin(),
                             preview_.dependency.diagnostics.end());
    return true;
}

CommandResult EditorText3DAuthoringSession::commit(EditorWorkspace& workspace,
                                                    std::string objectName,
                                                    RigidTransform transform) {
    if (!active_) return CommandResult::fail("3D text authoring session is not active");
    if (!preview_ && !refresh()) return CommandResult::fail(preview_.error);

    CookedText3DAsset asset = *preview_.asset;
    std::filesystem::path resolvedOutput;
    std::filesystem::path relativeOutput;
    if (!outputAssetPath_.empty()) {
        if (!confined_path(projectRoot_, outputAssetPath_, resolvedOutput, relativeOutput) ||
            lower(resolvedOutput.extension().string()) != ".dtext") {
            return CommandResult::fail("3D text output must be a project-relative .dtext path");
        }
    }

    const bool editing = targetObject_.has_value();
    EditorObjectId objectId{};
    if (editing) {
        const EditorObject* object = workspace.document().find_object(*targetObject_);
        if (!object || !object->text3d) return CommandResult::fail("3D text target no longer exists");
        objectId = *targetObject_;
    } else {
        objectId = workspace.document().allocate_object_id();
    }
    asset.objectId = objectId;
    asset.sideMesh.objectId = objectId;
    asset.sideMesh.contentHash = polygon_asset_content_hash(asset.sideMesh);
    asset.contentHash = text3d_content_hash(asset);

    // Prepare and validate the external .dtext file before changing the document. The final file
    // replacement is rolled back if the editor command is rejected, keeping the scene and source
    // asset transactionally aligned without putting font bytes in the scene journal.
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool replacedOutput = false;
    bool hadPreviousOutput = false;
    if (!resolvedOutput.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(resolvedOutput.parent_path(), ec);
        if (ec) return CommandResult::fail("Could not create the 3D text asset directory: " + ec.message());
        temporary = std::filesystem::path(resolvedOutput.string() + ".dve_tmp");
        backup = std::filesystem::path(resolvedOutput.string() + ".dve_backup");
        std::filesystem::remove(temporary, ec);
        ec.clear();
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::string writeError;
        if (!write_dtext(temporary, asset, &writeError))
            return CommandResult::fail("3D text source asset write failed: " + writeError);
        const auto roundTrip = read_dtext(temporary);
        if (!roundTrip || roundTrip.asset.contentHash != asset.contentHash) {
            std::filesystem::remove(temporary, ec);
            return CommandResult::fail("3D text source asset failed post-write validation");
        }
        hadPreviousOutput = std::filesystem::exists(resolvedOutput, ec) && !ec;
        ec.clear();
        if (hadPreviousOutput) {
            std::filesystem::rename(resolvedOutput, backup, ec);
            if (ec) {
                std::filesystem::remove(temporary, ec);
                return CommandResult::fail("Could not stage the previous 3D text asset: " + ec.message());
            }
        }
        std::filesystem::rename(temporary, resolvedOutput, ec);
        if (ec) {
            if (hadPreviousOutput) {
                std::error_code restoreError;
                std::filesystem::rename(backup, resolvedOutput, restoreError);
            }
            std::filesystem::remove(temporary, ec);
            return CommandResult::fail("Could not replace the 3D text source asset: " + ec.message());
        }
        replacedOutput = true;
    }

    CommandResult result;
    if (editing) {
        Text3DObjectState after{asset, preview_.dependency.projectRelativePath, relativeOutput};
        result = workspace.commands().execute(
            workspace.document(),
            std::make_unique<ReplaceText3DObjectCommand>(objectId, before_, after));
    } else {
        EditorObject object(objectId, std::move(objectName));
        object.transform = transform;
        object.flags.structural = false;
        object.flags.decorative = true;
        object.textFontAsset = preview_.dependency.projectRelativePath;
        object.sourceAsset = relativeOutput;
        object.text3d = std::move(asset);
        object.voxels = std::make_unique<VoxelObject>(objectId);
        result = workspace.commands().execute(
            workspace.document(), std::make_unique<AddObjectCommand>(std::move(object), "Create 3D text"));
    }

    if (!result.success) {
        if (replacedOutput) {
            std::error_code ec;
            std::filesystem::remove(resolvedOutput, ec);
            if (hadPreviousOutput) {
                ec.clear();
                std::filesystem::rename(backup, resolvedOutput, ec);
            }
        }
        return result;
    }

    if (replacedOutput && hadPreviousOutput) {
        std::error_code ec;
        std::filesystem::remove(backup, ec);
    }
    if (!editing) workspace.select_object(objectId);
    cancel();
    return CommandResult::ok();
}

} // namespace dve::editor
