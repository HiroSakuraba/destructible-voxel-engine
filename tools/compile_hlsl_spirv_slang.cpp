// Build helper (not part of the CMake build; needs the Slang SDK): HLSL compute shader -> SPIR-V
// for DVE's Vulkan RHI, through Slang's command-line front end. Used for shaders/compiled/ao:
//   g++ -std=c++17 -I<slang>/include tools/compile_hlsl_spirv_slang.cpp -o compile <slang>/libslang-compiler.so
//   ./compile shaders/resolve_ao.hlsl shaders shaders/compiled/ao/resolve_ao.spv
//   register(bN) -> binding N, register(tN) -> binding 16 + N, register(uN) -> binding 32 + N, set 0
//   (the same as dxc -spirv -fvk-b-shift 0 0 -fvk-t-shift 16 0 -fvk-u-shift 32 0), entry main.
#include <slang.h>
#include <cstdio>
#include <fstream>
#include <string>
int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: compile_spirv FILE INCLUDE_DIR OUT.spv\n"); return 2; }
    slang::IGlobalSession* global = nullptr;
    if (SLANG_FAILED(slang::createGlobalSession(&global))) return 3;
    SlangCompileRequest* request = nullptr;
    global->createCompileRequest(&request);
    const char* args[] = {argv[1], "-I", argv[2], "-target", "spirv", "-profile", "spirv_1_3", "-stage", "compute",
                          "-entry", "main", "-fvk-b-shift", "0", "0", "-fvk-t-shift", "16", "0",
                          "-fvk-u-shift", "32", "0", "-fvk-s-shift", "48", "0", "-o", argv[3]};
    if (SLANG_FAILED(request->processCommandLineArguments(args, int(sizeof(args) / sizeof(args[0]))))) {
        std::fprintf(stderr, "%s", request->getDiagnosticOutput()); return 4;
    }
    const SlangResult result = request->compile();
    std::fprintf(stderr, "%s", request->getDiagnosticOutput());
    if (SLANG_FAILED(result)) return 5;
    size_t size = 0;
    const void* code = request->getEntryPointCode(0, &size);
    std::ofstream out(argv[3], std::ios::binary);
    out.write(static_cast<const char*>(code), static_cast<std::streamsize>(size));
    std::printf("%s: %zu bytes SPIR-V\n", argv[3], size);
    return 0;
}
