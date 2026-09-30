// ContentSource: loose folder vs mounted .dvepak parity, path confinement, limits, pak
// integrity failures, and dve_pack's editor-only (.autosave/) stripping.

#include "dve/content_source.hpp"
#include "dve/v235_foundations.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

std::filesystem::path make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / (std::string("dve_content_source_") + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::vector<std::filesystem::path> all_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) files.push_back(std::filesystem::relative(entry.path(), root));
    }
    return files;
}

// A small project: the house scene under scenes/, a manifest, a nested text file, and editor
// leftovers (.autosave at two depths, editor/) that must never reach a shipped pak.
std::filesystem::path make_project(const std::filesystem::path& base) {
    const auto root = base / "project";
    const auto examples = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples";
    std::filesystem::create_directories(root / "scenes");
    for (const char* file : {"multi_object_house.dvoxscene.json", "multi_object_house_Foundation_3e9.dvox",
                             "multi_object_house_UpperBlock_3ea.dvox", "multi_object_house_Furniture_3eb.dvox"}) {
        std::filesystem::copy_file(examples / file, root / "scenes" / file);
    }
    write_text(root / "game.dvegame", "DVE_GAME 1\nname=Parity\nversion=1.0\nentryScene=scenes/multi_object_house.dvoxscene.json\n");
    write_text(root / "scripts" / "main.lua", "-- empty\n");
    write_text(root / "data" / "deep" / "empty.txt", "");
    write_text(root / ".autosave" / "main.dvescene", "autosave");
    write_text(root / "scenes" / ".autosave" / "nested.dvescene", "autosave");
    write_text(root / "editor" / "layout.txt", "editor-only");
    return root;
}

void test_normalize() {
    using dve::normalize_content_path;
    CHECK(normalize_content_path("a/b.txt") == std::string("a/b.txt"));
    CHECK(normalize_content_path("./a//b/./c.txt") == std::string("a/b/c.txt"));
    CHECK(normalize_content_path("a/b/") == std::string("a/b"));
    std::string error;
    CHECK(!normalize_content_path("", &error) && !error.empty());
    CHECK(!normalize_content_path("/etc/passwd"));
    CHECK(!normalize_content_path("../x"));
    CHECK(!normalize_content_path("a/../../x"));
    CHECK(!normalize_content_path("a/.."));
    CHECK(!normalize_content_path("a\\b"));
    CHECK(!normalize_content_path("C:/x"));
    CHECK(!normalize_content_path(std::string("a\0b", 3)));
    CHECK(!normalize_content_path("a\nb"));
    CHECK(!normalize_content_path("."));
    CHECK(!normalize_content_path(std::string(dve::kMaximumContentPathBytes + 1U, 'a')));
    CHECK(dve::resolve_content_sibling("scenes/main.dvoxscene.json", "house.dvox") == std::string("scenes/house.dvox"));
    CHECK(dve::resolve_content_sibling("main.dvoxscene.json", "sub/house.dvox") == std::string("sub/house.dvox"));
    CHECK(!dve::resolve_content_sibling("scenes/main.json", "../escape.dvox"));
}

void test_editor_only_rules() {
    const dve::DvePakBuildOptions options;
    CHECK(dve::dvepak_path_is_editor_only(".autosave/a.dvescene", options));
    CHECK(dve::dvepak_path_is_editor_only("examples/editor_demo_project/.autosave/scene.dvescene", options));
    CHECK(dve::dvepak_path_is_editor_only("editor/x", options));
    CHECK(!dve::dvepak_path_is_editor_only("scenes/not.autosave.txt", options));
    CHECK(!dve::dvepak_path_is_editor_only("scenes/.autosave", options)); // a file named .autosave is kept
    CHECK(!dve::dvepak_path_is_editor_only("myeditor/x", options));
    dve::DvePakBuildOptions keepAll;
    keepAll.stripEditorOnly = false;
    CHECK(!dve::dvepak_path_is_editor_only(".autosave/a", keepAll));
}

void test_parity_and_errors() {
    const auto base = make_temp_dir("parity");
    const auto root = make_project(base);
    const auto pakPath = base / "game.dvepak";
    const auto inputs = all_files(root);
    dve::DvePakManifest manifest;
    std::string error;
    CHECK(dve::build_dvepak(root, inputs, pakPath, {}, &manifest, &error));

    auto loose = dve::LooseContentSource::open(root, &error);
    auto pak = dve::PakContentSource::open(pakPath, &error);
    CHECK(loose && pak);
    if (!loose || !pak) return;
    CHECK(loose->describe().rfind("loose:", 0) == 0);
    CHECK(pak->describe().rfind("pak:", 0) == 0);

    // Autosave exclusion: present loose, absent from the pak (as are editor/ files).
    const auto looseAll = loose->list();
    const auto pakAll = pak->list();
    CHECK(std::find(looseAll.begin(), looseAll.end(), ".autosave/main.dvescene") != looseAll.end());
    for (const std::string& path : pakAll) {
        CHECK(path.find(".autosave") == std::string::npos);
        CHECK(path.rfind("editor/", 0) != 0);
    }
    CHECK(pakAll.size() == 7U);
    CHECK(std::is_sorted(looseAll.begin(), looseAll.end()));
    CHECK(std::is_sorted(pakAll.begin(), pakAll.end()));

    // Every shipped file reads back byte-identical from both sources; sizes agree.
    std::vector<std::string> expected;
    for (const std::string& path : looseAll) {
        if (!dve::dvepak_path_is_editor_only(path, {})) expected.push_back(path);
    }
    CHECK(expected == pakAll);
    for (const std::string& path : pakAll) {
        dve::ContentError looseError, pakError;
        const auto a = loose->read(path, &looseError);
        const auto b = pak->read(path, &pakError);
        CHECK(a && b);
        if (a && b) CHECK(*a == *b);
        CHECK(loose->exists(path) && pak->exists(path));
        CHECK(loose->size(path) == pak->size(path));
        CHECK(pak->mount().contains(path));
    }
    CHECK(loose->read("data/deep/empty.txt").value_or(std::vector<std::byte>{std::byte{1}}).empty());
    CHECK(pak->read("data/deep/empty.txt").value_or(std::vector<std::byte>{std::byte{1}}).empty());
    CHECK(pak->list("scenes/").size() == 4U);
    CHECK(loose->list("scenes/multi") == pak->list("scenes/multi"));
    // Non-normalized spellings resolve to the same entry.
    CHECK(pak->exists("./scenes//multi_object_house.dvoxscene.json"));
    CHECK(loose->exists("./scenes//multi_object_house.dvoxscene.json"));
    CHECK(!pak->mount().contains("scenes"));
    CHECK(!pak->mount().contains("zzz"));
    CHECK(pak->mount().find("game.dvegame") != nullptr);

    // Invalid paths, missing entries and size limits fail with the same codes on both.
    for (const dve::ContentSource* source : {static_cast<const dve::ContentSource*>(loose.get()),
                                             static_cast<const dve::ContentSource*>(pak.get())}) {
        for (const char* bad : {"../game.dvegame", "/etc/passwd", "scenes\\x.dvox", ""}) {
            dve::ContentError e;
            CHECK(!source->read(bad, &e));
            CHECK(e.code == dve::ContentErrorCode::InvalidPath);
            CHECK(!source->exists(bad));
        }
        dve::ContentError missing;
        CHECK(!source->read("scenes/missing.dvox", &missing));
        CHECK(missing.code == dve::ContentErrorCode::NotFound);
        CHECK(missing.path == "scenes/missing.dvox");
        dve::ContentError limited;
        CHECK(!source->read("game.dvegame", &limited, 8U));
        CHECK(limited.code == dve::ContentErrorCode::LimitExceeded);
        CHECK(source->read_text("game.dvegame").value_or("").rfind("DVE_GAME 1", 0) == 0);
        CHECK(!source->size("nope"));
        CHECK(!source->exists("scenes")); // directories are not content
    }

    // Loose confinement also holds through symlinks.
    std::error_code ec;
    write_text(base / "outside.txt", "secret");
    std::filesystem::create_symlink(base / "outside.txt", root / "link.txt", ec);
    if (!ec) {
        dve::ContentError e;
        CHECK(!loose->read("link.txt", &e));
        CHECK(e.code == dve::ContentErrorCode::InvalidPath);
    }

    // open_content_source dispatches on the location type.
    auto viaDir = dve::open_content_source(root);
    auto viaPak = dve::open_content_source(pakPath);
    CHECK(viaDir && viaDir->describe().rfind("loose:", 0) == 0);
    CHECK(viaPak && viaPak->describe().rfind("pak:", 0) == 0);
    CHECK(!dve::open_content_source(base / "does-not-exist", &error) && !error.empty());
    write_text(base / "garbage.dvepak", "not a package at all, definitely not");
    error.clear();
    CHECK(!dve::PakContentSource::open(base / "garbage.dvepak", &error) && !error.empty());

    std::filesystem::remove_all(base, ec);
}

void test_corrupted_entry_surfaces_integrity_failure() {
    const auto base = make_temp_dir("corrupt");
    const auto root = make_project(base);
    const auto pakPath = base / "game.dvepak";
    dve::DvePakManifest manifest;
    std::string error;
    CHECK(dve::build_dvepak(root, all_files(root), pakPath, {}, &manifest, &error));
    const std::string victim = "scenes/multi_object_house_UpperBlock_3ea.dvox";
    const auto entry = std::find_if(manifest.entries.begin(), manifest.entries.end(),
                                    [&](const dve::DvePakEntry& e) { return e.path == victim; });
    CHECK(entry != manifest.entries.end() && entry->size > 16U);
    if (entry == manifest.entries.end()) return;
    {
        std::fstream file(pakPath, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(static_cast<std::streamoff>(entry->offset + entry->size / 2U));
        char byte{};
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        file.seekp(static_cast<std::streamoff>(entry->offset + entry->size / 2U));
        file.write(&byte, 1);
    }
    auto pak = dve::PakContentSource::open(pakPath, &error);
    CHECK(pak != nullptr); // the directory is intact, so mounting still succeeds
    if (!pak) return;
    dve::ContentError e;
    CHECK(!pak->read(victim, &e));
    CHECK(e.code == dve::ContentErrorCode::IntegrityFailure);
    CHECK(e.path == victim);
    CHECK(e.message.find("integrity") != std::string::npos);
    CHECK(std::string(dve::to_string(e.code)).find("integrity") != std::string::npos);
    CHECK(pak->exists(victim));                    // listing is unaffected...
    CHECK(pak->read("game.dvegame").has_value());  // ...and other entries still verify
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

} // namespace

int main() {
    test_normalize();
    test_editor_only_rules();
    test_parity_and_errors();
    test_corrupted_entry_surfaces_integrity_failure();
    if (failures != 0) {
        std::cerr << failures << " content source check(s) failed\n";
        return 1;
    }
    std::cout << "content source tests passed\n";
    return 0;
}
