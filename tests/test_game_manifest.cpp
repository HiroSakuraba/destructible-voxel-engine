// game.dvegame parser/validator: accepted forms, canonical round trip, and rejection cases.

#include "dve/content_source.hpp"
#include "dve/game_manifest.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

const std::string kMinimal =
    "DVE_GAME 1\nname=Demo\nversion=1.0.0\nentryScene=scenes/main.dvoxscene.json\n";

void expect_reject(const std::string& text, const std::string& needle, int line) {
    std::string error;
    const auto parsed = dve::GameManifest::parse(text, &error);
    if (parsed || error.find(needle) == std::string::npos) {
        std::cerr << "FAIL line " << line << ": expected rejection containing '" << needle << "', got "
                  << (parsed ? "success" : "'" + error + "'") << "\n";
        ++failures;
    }
}
#define REJECT(text, needle) expect_reject((text), (needle), __LINE__)

void test_accepts() {
    std::string error;
    const auto minimal = dve::GameManifest::parse(kMinimal, &error);
    CHECK(minimal.has_value());
    if (minimal) {
        CHECK(minimal->name == "Demo");
        CHECK(minimal->version == "1.0.0");
        CHECK(minimal->entryScene == "scenes/main.dvoxscene.json");
        CHECK(!minimal->startupScript && !minimal->settings && !minimal->camera);
        CHECK(minimal->inputBindings.empty());
        CHECK(minimal->startup_script_or_default() == "scripts/main.lua");
    }

    const std::string full =
        "\xEF\xBB\xBF# leading comment\r\n"
        "\r\n"
        "DVE_GAME 1\r\n"
        "name = My Game#2  \r\n"           // "#" not preceded by whitespace is part of the value
        "version=0.3.1-beta+7\r\n"
        "entryScene=scenes/main.dvoxscene.json   # trailing comment\r\n"
        "startupScript=scripts/boot.lua\r\n"
        "settings=game_settings.txt\r\n"
        "camera=0,1.5,6 -> 0,0.5,0\r\n"
        "bind.move_x=key:a/-1,key:d/+1,gamepad:leftx\r\n"
        "bind.jump=key:space";
    const auto parsed = dve::GameManifest::parse(full, &error);
    CHECK(parsed.has_value());
    if (!parsed) { std::cerr << "full manifest error: " << error << "\n"; return; }
    CHECK(parsed->name == "My Game#2");
    CHECK(parsed->version == "0.3.1-beta+7");
    CHECK(parsed->startupScript == std::string("scripts/boot.lua"));
    CHECK(parsed->startup_script_or_default() == "scripts/boot.lua");
    CHECK(parsed->settings == std::string("game_settings.txt"));
    CHECK(parsed->camera.has_value());
    if (parsed->camera) {
        CHECK(parsed->camera->eye.y == 1.5F && parsed->camera->eye.z == 6.0F);
        CHECK(parsed->camera->target.y == 0.5F);
    }
    CHECK(parsed->inputBindings.size() == 2U);
    CHECK(parsed->inputBindings.at("move_x") == "key:a/-1,key:d/+1,gamepad:leftx");
    CHECK(parsed->inputBindings.at("jump") == "key:space");

    // Canonical serialization round-trips exactly.
    const std::string text = parsed->serialize();
    CHECK(text.rfind("DVE_GAME 1\n", 0) == 0);
    const auto again = dve::GameManifest::parse(text, &error);
    CHECK(again.has_value());
    if (again) {
        CHECK(again->serialize() == text);
        CHECK(again->name == parsed->name && again->entryScene == parsed->entryScene);
        CHECK(again->camera && again->camera->eye.x == parsed->camera->eye.x);
        CHECK(again->inputBindings == parsed->inputBindings);
    }
}

void test_rejects() {
    REJECT("", "must start with 'DVE_GAME 1'");
    REJECT("# only comments\n", "must start with 'DVE_GAME 1'");
    REJECT("name=Demo\n" + kMinimal, "must start with 'DVE_GAME 1'");
    REJECT("DVE_GAME 2\nname=Demo\nversion=1\nentryScene=a.dvoxscene.json\n", "unsupported game manifest version");
    REJECT("DVE_GAME\nname=Demo\n", "header must be");
    REJECT("DVE_GAME one\n", "header must be");
    REJECT("DVE_GAMES 1\n", "header must be");
    REJECT(kMinimal + "unknownKey=1\n", "unknown key 'unknownKey'");
    REJECT(kMinimal + "name=Other\n", "duplicate key 'name'");
    REJECT(kMinimal + "just some words\n", "is not key=value");
    REJECT(kMinimal + "=value\n", "empty key");
    REJECT(kMinimal + "settings=\n", "empty value");
    REJECT("DVE_GAME 1\nversion=1\nentryScene=a.dvoxscene.json\n", "missing required key 'name'");
    REJECT("DVE_GAME 1\nname=A\nentryScene=a.dvoxscene.json\n", "missing required key 'version'");
    REJECT("DVE_GAME 1\nname=A\nversion=1\n", "missing required key 'entryScene'");
    REJECT("DVE_GAME 1\nname=A\nversion=1 2\nentryScene=a.dvoxscene.json\n", "'version' must be");
    REJECT("DVE_GAME 1\nname=A\nversion=" + std::string(65, '1') + "\nentryScene=a.dvoxscene.json\n", "'version' must be");
    REJECT("DVE_GAME 1\nname=" + std::string(129, 'n') + "\nversion=1\nentryScene=a.dvoxscene.json\n", "'name' must be");
    REJECT("DVE_GAME 1\nname=A\nversion=1\nentryScene=scenes/main.dvescene\n", "must name a .dvoxscene.json");
    REJECT("DVE_GAME 1\nname=A\nversion=1\nentryScene=../main.dvoxscene.json\n", "not a valid content path");
    REJECT("DVE_GAME 1\nname=A\nversion=1\nentryScene=/abs/main.dvoxscene.json\n", "not a valid content path");
    REJECT("DVE_GAME 1\nname=A\nversion=1\nentryScene=scenes\\main.dvoxscene.json\n", "not a valid content path");
    REJECT("DVE_GAME 1\nname=A\nversion=1\nentryScene=scenes//main.dvoxscene.json\n", "normalized content path");
    REJECT(kMinimal + "startupScript=scripts/main.py\n", "must name a .lua file");
    REJECT(kMinimal + "settings=../settings.txt\n", "not a valid content path");
    REJECT(kMinimal + "camera=0,1,2\n", "camera must be");
    REJECT(kMinimal + "camera=0,1 -> 0,0,0\n", "camera must be");
    REJECT(kMinimal + "camera=0,1,2 -> 0,0,x\n", "camera must be");
    REJECT(kMinimal + "camera=0,1,2 -> 0,0,0,0\n", "camera must be");
    REJECT(kMinimal + "camera=nan,1,2 -> 0,0,0\n", "camera must be");
    REJECT(kMinimal + "camera=1,1,1 -> 1,1,1\n", "eye and target must differ");
    REJECT(kMinimal + "bind.=key:a\n", "binding action");
    REJECT(kMinimal + "bind.move x=key:a\n", "binding action");
    REJECT(kMinimal + "bind.jump=" + std::string(1025, 'k') + "\n", "binding for 'jump'");
    REJECT(kMinimal + "name2=\x01\n", "control character");
    REJECT(kMinimal + "x=" + std::string(dve::kMaximumGameManifestLineBytes, 'x') + "\n", "line length limit");
    std::string manyBindings = kMinimal;
    for (std::size_t i = 0; i <= dve::kMaximumGameManifestBindings; ++i) manyBindings += "bind.a" + std::to_string(i) + "=k\n";
    REJECT(manyBindings, "too many bind.* entries");
    std::string huge = kMinimal;
    while (huge.size() <= dve::kMaximumGameManifestBytes) huge += "# padding padding padding padding padding padding\n";
    REJECT(huge, "64 KiB size limit");
}

void test_content_integration() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("dve_game_manifest_" + std::to_string(stamp));
    std::filesystem::create_directories(root / "scenes");
    std::string error;
    auto source = dve::LooseContentSource::open(root, &error);
    CHECK(source != nullptr);
    if (!source) return;
    CHECK(!dve::load_game_manifest(*source, &error));
    CHECK(error.find("game.dvegame") != std::string::npos);
    { std::ofstream(root / "game.dvegame") << "DVE_GAME 1\nname=A\n"; }
    CHECK(!dve::load_game_manifest(*source, &error));
    CHECK(error.find("missing required key 'version'") != std::string::npos);
    { std::ofstream(root / "game.dvegame") << kMinimal << "startupScript=scripts/main.lua\n"; }
    const auto manifest = dve::load_game_manifest(*source, &error);
    CHECK(manifest.has_value());
    if (manifest) {
        CHECK(!dve::check_game_manifest_content(*manifest, *source, &error));
        CHECK(error.find("entry scene") != std::string::npos);
        { std::ofstream(root / "scenes" / "main.dvoxscene.json") << "{}"; }
        CHECK(!dve::check_game_manifest_content(*manifest, *source, &error));
        CHECK(error.find("startup script") != std::string::npos);
        std::filesystem::create_directories(root / "scripts");
        { std::ofstream(root / "scripts" / "main.lua") << "-- ok\n"; }
        CHECK(dve::check_game_manifest_content(*manifest, *source, &error));
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    test_accepts();
    test_rejects();
    test_content_integration();
    if (failures != 0) {
        std::cerr << failures << " game manifest check(s) failed\n";
        return 1;
    }
    std::cout << "game manifest tests passed\n";
    return 0;
}
