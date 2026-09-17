#ifdef _WIN32

#include "dve/gpu/d3d12_brickmap_backend.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include <Windows.h>

namespace dve::gpu {
namespace {

void set_error(std::string* error, const char* message, HRESULT hr = S_OK) {
    if (error == nullptr) return;
    std::ostringstream stream;
    stream << message;
    if (FAILED(hr)) stream << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ')';
    *error = stream.str();
}

[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    const std::size_t remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

[[nodiscard]] D3D12_RESOURCE_DESC buffer_desc(std::size_t bytes) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = static_cast<UINT64>(std::max<std::size_t>(bytes, 4U));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

} // namespace

D3D12BrickmapBackend::~D3D12BrickmapBackend() {
    wait_idle();
    release_mapped_upload();
    if (fenceEvent_ != nullptr) CloseHandle(static_cast<HANDLE>(fenceEvent_));
}

bool D3D12BrickmapBackend::create_queue(
    D3D12_COMMAND_LIST_TYPE type,
    ComPtr<ID3D12CommandQueue>& output,
    std::string* error) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    const HRESULT hr = device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { set_error(error, "CreateCommandQueue failed", hr); return false; }
    return true;
}

bool D3D12BrickmapBackend::initialize(std::string* error) {
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) { set_error(error, "CreateDXGIFactory2 failed", hr); return false; }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory_->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            adapter_ = candidate;
            featureReport_.adapterName = desc.Description;
            featureReport_.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            break;
        }
    }
    if (adapter_ == nullptr) { set_error(error, "No hardware D3D12 FL12_0 adapter found"); return false; }
    hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) { set_error(error, "D3D12CreateDevice failed", hr); return false; }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
        featureReport_.shaderModel = shaderModel.HighestShaderModel;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)))) {
        featureReport_.waveOps = options1.WaveOps != FALSE;
        featureReport_.waveLaneMinimum = options1.WaveLaneCountMin;
        featureReport_.waveLaneMaximum = options1.WaveLaneCountMax;
    }

    if (!create_queue(D3D12_COMMAND_LIST_TYPE_DIRECT, directQueue_, error) ||
        !create_queue(D3D12_COMMAND_LIST_TYPE_COMPUTE, computeQueue_, error) ||
        !create_queue(D3D12_COMMAND_LIST_TYPE_COPY, copyQueue_, error)) return false;
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyAllocator_));
    if (FAILED(hr)) { set_error(error, "CreateCommandAllocator failed", hr); return false; }
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator_.Get(), nullptr,
                                    IID_PPV_ARGS(&copyList_));
    if (FAILED(hr)) { set_error(error, "CreateCommandList failed", hr); return false; }
    copyList_->Close();
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_));
    if (FAILED(hr)) { set_error(error, "CreateFence failed", hr); return false; }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr) { set_error(error, "CreateEvent failed"); return false; }
    return true;
}

bool D3D12BrickmapBackend::create_buffer(
    std::size_t bytes,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    ComPtr<ID3D12Resource>& output,
    std::string* error) {
    const D3D12_HEAP_PROPERTIES heap = heap_properties(heapType);
    const D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
    const HRESULT hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { set_error(error, "CreateCommittedResource buffer failed", hr); return false; }
    return true;
}

void D3D12BrickmapBackend::release_mapped_upload() noexcept {
    if (uploadBuffer_ != nullptr && mappedUpload_ != nullptr) uploadBuffer_->Unmap(0, nullptr);
    mappedUpload_ = nullptr;
}

bool D3D12BrickmapBackend::reserve_scene_capacity(
    std::size_t indexBytes,
    std::size_t recordBytes,
    std::size_t materialBytes,
    std::string* error) {
    if (device_ == nullptr) { set_error(error, "backend not initialized"); return false; }
    wait_idle();
    release_mapped_upload();

    indexCapacity_ = std::max<std::size_t>(indexBytes, 4U);
    recordCapacity_ = std::max<std::size_t>(recordBytes, 4U);
    materialCapacity_ = std::max<std::size_t>(materialBytes, 4U);
    const std::size_t indexOffset = 0;
    const std::size_t recordOffset = align_up(indexCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const std::size_t materialOffset = align_up(recordOffset + recordCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    uploadCapacity_ = materialOffset + materialCapacity_;
    readbackCapacity_ = uploadCapacity_;

    if (!create_buffer(indexCapacity_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, indexBuffer_, error) ||
        !create_buffer(recordCapacity_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, recordBuffer_, error) ||
        !create_buffer(materialCapacity_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, materialBuffer_, error) ||
        !create_buffer(uploadCapacity_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, uploadBuffer_, error) ||
        !create_buffer(readbackCapacity_, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readbackBuffer_, error)) {
        return false;
    }
    (void)indexOffset;
    D3D12_RANGE noRead{0, 0};
    const HRESULT hr = uploadBuffer_->Map(0, &noRead, reinterpret_cast<void**>(&mappedUpload_));
    if (FAILED(hr)) { set_error(error, "upload buffer Map failed", hr); return false; }
    return true;
}

bool D3D12BrickmapBackend::execute_copy_and_wait(std::string* error) {
    HRESULT hr = copyList_->Close();
    if (FAILED(hr)) { set_error(error, "copy list Close failed", hr); return false; }
    ID3D12CommandList* lists[]{copyList_.Get()};
    copyQueue_->ExecuteCommandLists(1, lists);
    const std::uint64_t fence = nextFenceValue_++;
    hr = copyQueue_->Signal(copyFence_.Get(), fence);
    if (FAILED(hr)) { set_error(error, "copy queue Signal failed", hr); return false; }
    if (copyFence_->GetCompletedValue() < fence) {
        hr = copyFence_->SetEventOnCompletion(fence, static_cast<HANDLE>(fenceEvent_));
        if (FAILED(hr)) { set_error(error, "SetEventOnCompletion failed", hr); return false; }
        WaitForSingleObject(static_cast<HANDLE>(fenceEvent_), INFINITE);
    }
    return true;
}

bool D3D12BrickmapBackend::upload_scene(
    const PackedBrickmapScene& scene,
    std::string* error) {
    const std::size_t indexBytes = scene.index_grid().size_bytes();
    const std::size_t recordBytes = scene.records().size_bytes();
    const std::size_t materialBytes = scene.material_arena().size_bytes();
    if (indexBytes > indexCapacity_ || recordBytes > recordCapacity_ || materialBytes > materialCapacity_) {
        set_error(error, "scene exceeds reserved D3D12 buffer capacity");
        return false;
    }
    const std::size_t indexOffset = 0;
    const std::size_t recordOffset = align_up(indexCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const std::size_t materialOffset = align_up(recordOffset + recordCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (indexBytes != 0) std::memcpy(mappedUpload_ + indexOffset, scene.index_grid().data(), indexBytes);
    if (recordBytes != 0) std::memcpy(mappedUpload_ + recordOffset, scene.records().data(), recordBytes);
    if (materialBytes != 0) std::memcpy(mappedUpload_ + materialOffset, scene.material_arena().data(), materialBytes);

    HRESULT hr = copyAllocator_->Reset();
    if (FAILED(hr)) { set_error(error, "copy allocator Reset failed", hr); return false; }
    hr = copyList_->Reset(copyAllocator_.Get(), nullptr);
    if (FAILED(hr)) { set_error(error, "copy list Reset failed", hr); return false; }
    if (indexBytes != 0) copyList_->CopyBufferRegion(indexBuffer_.Get(), 0, uploadBuffer_.Get(), indexOffset, indexBytes);
    if (recordBytes != 0) copyList_->CopyBufferRegion(recordBuffer_.Get(), 0, uploadBuffer_.Get(), recordOffset, recordBytes);
    if (materialBytes != 0) copyList_->CopyBufferRegion(materialBuffer_.Get(), 0, uploadBuffer_.Get(), materialOffset, materialBytes);
    return execute_copy_and_wait(error);
}

bool D3D12BrickmapBackend::readback_matches(
    const PackedBrickmapScene& scene,
    std::string* error) {
    const std::size_t indexBytes = scene.index_grid().size_bytes();
    const std::size_t recordBytes = scene.records().size_bytes();
    const std::size_t materialBytes = scene.material_arena().size_bytes();
    const std::size_t indexOffset = 0;
    const std::size_t recordOffset = align_up(indexCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const std::size_t materialOffset = align_up(recordOffset + recordCapacity_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    HRESULT hr = copyAllocator_->Reset();
    if (FAILED(hr)) { set_error(error, "copy allocator Reset failed", hr); return false; }
    hr = copyList_->Reset(copyAllocator_.Get(), nullptr);
    if (FAILED(hr)) { set_error(error, "copy list Reset failed", hr); return false; }
    if (indexBytes != 0) copyList_->CopyBufferRegion(readbackBuffer_.Get(), indexOffset, indexBuffer_.Get(), 0, indexBytes);
    if (recordBytes != 0) copyList_->CopyBufferRegion(readbackBuffer_.Get(), recordOffset, recordBuffer_.Get(), 0, recordBytes);
    if (materialBytes != 0) copyList_->CopyBufferRegion(readbackBuffer_.Get(), materialOffset, materialBuffer_.Get(), 0, materialBytes);
    if (!execute_copy_and_wait(error)) return false;

    const std::size_t mapEnd = materialOffset + materialBytes;
    D3D12_RANGE readRange{0, mapEnd};
    void* mappedPointer = nullptr;
    hr = readbackBuffer_->Map(0, &readRange, &mappedPointer);
    if (FAILED(hr)) { set_error(error, "readback buffer Map failed", hr); return false; }
    const auto* mapped = static_cast<const std::byte*>(mappedPointer);
    const bool matches =
        (indexBytes == 0 || std::memcmp(mapped + indexOffset, scene.index_grid().data(), indexBytes) == 0) &&
        (recordBytes == 0 || std::memcmp(mapped + recordOffset, scene.records().data(), recordBytes) == 0) &&
        (materialBytes == 0 || std::memcmp(mapped + materialOffset, scene.material_arena().data(), materialBytes) == 0);
    D3D12_RANGE noWrite{0, 0};
    readbackBuffer_->Unmap(0, &noWrite);
    if (!matches) set_error(error, "D3D12 readback did not match packed CPU scene");
    return matches;
}

void D3D12BrickmapBackend::wait_idle() noexcept {
    if (copyQueue_ == nullptr || copyFence_ == nullptr || fenceEvent_ == nullptr) return;
    const std::uint64_t fence = nextFenceValue_++;
    if (FAILED(copyQueue_->Signal(copyFence_.Get(), fence))) return;
    if (copyFence_->GetCompletedValue() < fence &&
        SUCCEEDED(copyFence_->SetEventOnCompletion(fence, static_cast<HANDLE>(fenceEvent_)))) {
        WaitForSingleObject(static_cast<HANDLE>(fenceEvent_), INFINITE);
    }
}

} // namespace dve::gpu

#endif // _WIN32
