# DASHR GPU smoke test

The Null RHI tests check renderer setup and command recording. The Vulkan smoke
test exercises the compiled DASHR shaders, atlas raster pass, edge-fill compute
pass, camera shell draw, and displaced shadow depth pass on a real Vulkan device.

## Run it

The test needs Microsoft's DirectX Shader Compiler (`dxc`), a Vulkan development
toolchain, and a device that supports sampled, render-target, and storage
`RGBA16F` textures.

```sh
python3 scripts/compile_shaders.py --backend spirv --out build/shaders
cmake -S . -B build -DDVE_BUILD_TESTS=ON -DDVE_BUILD_RHI=ON \
  -DDVE_ENABLE_VULKAN_BUFFER_BACKEND=ON
cmake --build build --target dve_vulkan_dashr_tests
DVE_REQUIRE_DASHR_GPU=1 DVE_DASHR_SHADER_DIR="$PWD/build/shaders" \
  ctest --test-dir build --output-on-failure -R '^dve_vulkan_dashr_tests$'
```

`DVE_DASHR_SHADER_DIR` must point to the `--out` directory containing shader
subdirectories such as `dashr_atlas_vs/dashr_atlas_vs.spv`. Without that
variable, or without Vulkan, the test reports a skip so it can run in ordinary
CPU-only test environments. Set `DVE_REQUIRE_DASHR_GPU=1` to make either
missing prerequisite fail the run.

The smoke test does not yet exercise PBR material descriptors or the complete
live scene renderer's cascaded-shadow integration. Those remain separate GPU
validation work.
