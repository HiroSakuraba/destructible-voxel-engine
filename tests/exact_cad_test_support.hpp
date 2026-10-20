#pragma once

// Shared by the exact CAD feature tests (CAD Detail, edit guard, Measure, STEP export): a unit cube
// GLB like brepkernel writes, a scratch folder, small file helpers, and a fake brepkernel command
// that the editor can run through DVE_BREPKERNEL.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace exact_cad_test {
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("dve-cad-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

inline void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Unit cube [0,1]^3 as a GLB: 24 vertices (hard edges), outward normals, CCW triangles.
inline void write_cube_glb(const fs::path& path) {
    struct V { float p[3], n[3]; };
    std::vector<V> vertices;
    std::vector<std::uint32_t> indices;
    const float faces[6][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    for (const auto& f : faces) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const float su[4] = {-.5F, .5F, .5F, -.5F}, sv[4] = {-.5F, -.5F, .5F, .5F};
        for (int k = 0; k < 4; ++k) {
            V v{};
            for (int i = 0; i < 3; ++i) {
                v.p[i] = .5F + .5F * f[0][i] + su[k] * f[1][i] + sv[k] * f[2][i];
                v.n[i] = f[0][i];
            }
            vertices.push_back(v);
        }
        for (const std::uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) indices.push_back(base + i);
    }
    std::string bin;
    for (const auto& v : vertices) bin.append(reinterpret_cast<const char*>(v.p), sizeof v.p);
    for (const auto& v : vertices) bin.append(reinterpret_cast<const char*>(v.n), sizeof v.n);
    bin.append(reinterpret_cast<const char*>(indices.data()), indices.size() * 4);
    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" << bin.size()
         << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":288},{"buffer":0,"byteOffset":288,"byteLength":288},)"
         << R"({"buffer":0,"byteOffset":576,"byteLength":144}],"accessors":[)"
         << R"({"bufferView":0,"componentType":5126,"count":24,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)"
         << R"({"bufferView":1,"componentType":5126,"count":24,"type":"VEC3"},)"
         << R"({"bufferView":2,"componentType":5125,"count":36,"type":"SCALAR"}],)"
         << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],)"
         << R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::string text = json.str();
    while (text.size() % 4) text += ' ';
    while (bin.size() % 4) bin += '\0';
    const auto u32 = [](std::string& out, std::uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    std::string glb;
    u32(glb, 0x46546C67U);
    u32(glb, 2);
    u32(glb, static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    u32(glb, static_cast<std::uint32_t>(text.size()));
    u32(glb, 0x4E4F534AU);
    glb += text;
    u32(glb, static_cast<std::uint32_t>(bin.size()));
    u32(glb, 0x004E4942U);
    glb += bin;
    write_text(path, glb);
}


// A /bin/sh stand-in for brepkernel. `mesh` copies `templateGlb` and reports the deflection it was
// given; `describe`, `query` and `export` answer with the JSON in `describeJson` / `queryJson` /
// writing `exportText` to the output. Every call appends its arguments to `log`.
inline fs::path write_fake_brepkernel(const fs::path& dir, const fs::path& templateGlb, const std::string& describeJson = "{}",
                                      const std::string& queryJson = "[]", const std::string& exportReport = "{}") {
    write_text(dir / "describe.json", describeJson);
    write_text(dir / "query.json", queryJson);
    write_text(dir / "export.json", exportReport);
    const fs::path script = dir / "fake_brepkernel.sh";
    write_text(script, "#!/bin/sh\n"
        "echo \"$@\" >> '" + (dir / "log.txt").string() + "'\n"
        "cmd=\"$1\"; shift; out=''; report=''; defl='0.002'\n"
        "while [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\"; shift;; --report) report=\"$2\"; shift;; --deflection) defl=\"$2\"; shift;; --parts) cp \"$2\" '" + (dir / "parts_seen.json").string() + "'; shift;; esac; shift; done\n"
        "case \"$cmd\" in\n"
        "mesh) cp '" + templateGlb.string() + "' \"$out\"; printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"ok\",\"mesh\":{\"linear_deflection\":%s,\"angular_deflection\":0.35,\"triangles\":12}}' \"$defl\" > \"$report\";;\n"
        "describe) printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"ok\",\"description\":' > \"$report\"; cat '" + (dir / "describe.json").string() + "' >> \"$report\"; printf '}' >> \"$report\";;\n"
        "query) printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"ok\",\"results\":' > \"$report\"; cat '" + (dir / "query.json").string() + "' >> \"$report\"; printf '}' >> \"$report\";;\n"
        "export) printf 'ISO-10303-21;' > \"$out\"; printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"ok\",\"export\":' > \"$report\"; cat '" + (dir / "export.json").string() + "' >> \"$report\"; printf '}' >> \"$report\";;\n"
        "*) printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"error\",\"error\":\"unknown command\"}' > \"$report\"; exit 1;;\n"
        "esac\n");
    return script;
}

}  // namespace exact_cad_test
