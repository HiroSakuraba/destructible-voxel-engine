# Compressed texture loading

DVE can load a single 2D block-compressed image from DDS or KTX2 and upload its mip chain without expanding it to RGBA8. The initial supported formats are BC1 (linear and sRGB), BC3 (linear and sRGB), and BC5 (linear). The Vulkan backend uses native Vulkan block formats and reports support through `texture_format_capabilities`; the Null backend stores the exact compressed bytes for deterministic tests.

```cpp
dve::CompressedTextureAsset source;
std::string error;
if (!dve::load_compressed_texture("Assets/rock.ktx2", source, &error)) {
    // Report the import error in the editor.
}

dve::CompressedTextureResource gpuTexture;
if (!dve::upload_compressed_texture(device, source, gpuTexture, &error)) {
    // Decode to RGBA8 or choose a supported cooked variant as fallback.
}
// Bind gpuTexture.view; release both handles with destroy_compressed_texture.
```

The reader rejects arrays, cubemaps, volume textures, unsupported block formats, malformed/truncated mip payloads, and KTX2 supercompression. This keeps format selection explicit: ASTC, ETC2, Basis Universal transcoding, broad DDS variants, and wiring the resource into polygon/glTF authoring remain future work. Polygon texture baking continues to use decoded RGBA8 so CPU material sampling remains unchanged.
