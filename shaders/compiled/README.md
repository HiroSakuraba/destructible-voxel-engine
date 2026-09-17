# DVE checked-in sprite SPIR-V

These Vulkan 1.0 modules are generated from the adjacent
`shaders/vulkan_validation/sprite_2d.*.glsl` sources with `@webgpu/glslang` 0.0.15. They are runtime
artifacts, not handwritten test bytecode.

- `sprite_2d.vert.spv`: 1,164 bytes; SHA-256
  `2640fd0160e936d7385aaa0413d53e8c3be2b539e5518d442ddcb9ecb658a742`
- `sprite_2d.frag.spv`: 668 bytes; SHA-256
  `8e3066f07ac1514eb7f112920f86d6416d7958db9d499e20026999638ac9f299`

The C++ loader independently checks size, word alignment, complete reads, and SPIR-V magic. Shader
contract validation covers the canonical HLSL declarations; compilation of these GLSL modules is
part of the v1.93 release evidence.
