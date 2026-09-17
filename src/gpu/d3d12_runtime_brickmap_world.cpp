#ifdef _WIN32

#include "dve/gpu/d3d12_brickmap_backend.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <Windows.h>

namespace dve::gpu {
namespace {

void set_runtime_error(std::string* error, const char* message, HRESULT hr = S_OK) {
    if (error == nullptr) return;
    std::ostringstream stream;
    stream << message;
    if (FAILED(hr)) stream << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ')';
    *error = stream.str();
}

[[nodiscard]] std::size_t align_up_runtime(std::size_t value, std::size_t alignment) noexcept {
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES runtime_heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1U;
    properties.VisibleNodeMask = 1U;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_DESC runtime_buffer_desc(std::size_t bytes) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(std::max<std::size_t>(bytes, 4U));
    desc.Height = 1U;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1U, 0U};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

} // namespace

D3D12RuntimeBrickmapWorld::D3D12RuntimeBrickmapWorld(
    const D3D12RuntimeBrickmapWorldConfig& config)
    : config_(config), persistent_(config.heaps) {
    counters_.uploadRingBytes = config_.uploadRingBytes;
}

D3D12RuntimeBrickmapWorld::~D3D12RuntimeBrickmapWorld() {
    wait_idle();
    if (uploadRing_ != nullptr && mappedUpload_ != nullptr) uploadRing_->Unmap(0, nullptr);
    mappedUpload_ = nullptr;
    if (fenceEvent_ != nullptr) CloseHandle(static_cast<HANDLE>(fenceEvent_));
    fenceEvent_ = nullptr;
}

bool D3D12RuntimeBrickmapWorld::initialize(std::string* error) {
    if (device_ != nullptr) return true;
    UINT factoryFlags = 0U;
    if (config_.enableDebugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        set_runtime_error(error, "CreateDXGIFactory2 failed", hr);
        return false;
    }

    for (UINT index = 0U;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory_->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) continue;
        if (SUCCEEDED(D3D12CreateDevice(
                candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                __uuidof(ID3D12Device), nullptr))) {
            adapter_ = candidate;
            featureReport_.adapterName = desc.Description;
            featureReport_.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            break;
        }
    }
    if (adapter_ == nullptr) {
        set_runtime_error(error, "No hardware D3D12 feature-level 12.0 adapter found");
        return false;
    }
    hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) {
        set_runtime_error(error, "D3D12CreateDevice failed", hr);
        return false;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    if (SUCCEEDED(device_->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
        featureReport_.shaderModel = shaderModel.HighestShaderModel;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device_->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)))) {
        featureReport_.waveOps = options1.WaveOps != FALSE;
        featureReport_.waveLaneMinimum = options1.WaveLaneCountMin;
        featureReport_.waveLaneMaximum = options1.WaveLaneCountMax;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue_));
    if (FAILED(hr)) {
        set_runtime_error(error, "CreateCommandQueue(COPY) failed", hr);
        return false;
    }
    hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyAllocator_));
    if (FAILED(hr)) {
        set_runtime_error(error, "CreateCommandAllocator(COPY) failed", hr);
        return false;
    }
    hr = device_->CreateCommandList(
        0U, D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator_.Get(), nullptr,
        IID_PPV_ARGS(&copyList_));
    if (FAILED(hr)) {
        set_runtime_error(error, "CreateCommandList(COPY) failed", hr);
        return false;
    }
    (void)copyList_->Close();
    hr = device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) {
        set_runtime_error(error, "CreateFence failed", hr);
        return false;
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr) {
        set_runtime_error(error, "CreateEvent failed");
        return false;
    }
    if (!create_shared_buffers(error)) return false;
    return true;
}

bool D3D12RuntimeBrickmapWorld::create_shared_buffers(std::string* error) {
    const auto create = [&](std::size_t bytes, D3D12_HEAP_TYPE heapType,
                            D3D12_RESOURCE_STATES state,
                            ComPtr<ID3D12Resource>& output) -> bool {
        const D3D12_HEAP_PROPERTIES heap = runtime_heap_properties(heapType);
        const D3D12_RESOURCE_DESC desc = runtime_buffer_desc(bytes);
        const HRESULT hr = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(hr)) {
            set_runtime_error(error, "CreateCommittedResource failed", hr);
            return false;
        }
        return true;
    };
    if (!create(config_.heaps.indexHeapBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, indexBuffer_) ||
        !create(config_.heaps.recordHeapBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, recordBuffer_) ||
        !create(config_.heaps.materialHeapBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, materialBuffer_) ||
        !create(config_.uploadRingBytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, uploadRing_) ||
        !create(config_.readbackBytes, D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST, readbackBuffer_)) {
        return false;
    }
    D3D12_RANGE noRead{0U, 0U};
    const HRESULT hr = uploadRing_->Map(0U, &noRead, reinterpret_cast<void**>(&mappedUpload_));
    if (FAILED(hr)) {
        set_runtime_error(error, "upload ring Map failed", hr);
        return false;
    }
    return true;
}

ID3D12Resource* D3D12RuntimeBrickmapWorld::resource_for(
    PersistentBrickmapHeapKind heap) const noexcept {
    switch (heap) {
    case PersistentBrickmapHeapKind::Index: return indexBuffer_.Get();
    case PersistentBrickmapHeapKind::Record: return recordBuffer_.Get();
    case PersistentBrickmapHeapKind::Material: return materialBuffer_.Get();
    }
    return nullptr;
}

bool D3D12RuntimeBrickmapWorld::wait_for_fence(
    std::uint64_t value,
    std::string* error) const {
    if (value == 0U || fence_->GetCompletedValue() >= value) return true;
    const HRESULT hr = fence_->SetEventOnCompletion(value, static_cast<HANDLE>(fenceEvent_));
    if (FAILED(hr)) {
        set_runtime_error(error, "SetEventOnCompletion failed", hr);
        return false;
    }
    WaitForSingleObject(static_cast<HANDLE>(fenceEvent_), INFINITE);
    return true;
}

bool D3D12RuntimeBrickmapWorld::execute_and_signal(
    std::uint64_t& fenceValue,
    std::string* error) {
    HRESULT hr = copyList_->Close();
    if (FAILED(hr)) {
        set_runtime_error(error, "copy command list Close failed", hr);
        record_device_removed();
        return false;
    }
    ID3D12CommandList* lists[]{copyList_.Get()};
    copyQueue_->ExecuteCommandLists(1U, lists);
    fenceValue = nextFenceValue_++;
    hr = copyQueue_->Signal(fence_.Get(), fenceValue);
    if (FAILED(hr)) {
        set_runtime_error(error, "copy queue Signal failed", hr);
        record_device_removed();
        return false;
    }
    lastSubmittedFence_ = fenceValue;
    ++counters_.submittedCopyBatches;
    return true;
}

bool D3D12RuntimeBrickmapWorld::upload_pending(
    RuntimeBrickmapHandle handle,
    std::string* error) {
    const auto ranges = persistent_.pending_upload_ranges(handle);
    if (ranges.empty()) return true;
    std::size_t totalBytes{};
    for (const PersistentRuntimeBrickmapUploadRange& range : ranges) {
        totalBytes = align_up_runtime(totalBytes, 16U);
        if (range.bytes > config_.uploadRingBytes - std::min(totalBytes, config_.uploadRingBytes)) {
            set_runtime_error(error, "one object dirty upload exceeds D3D12 upload ring capacity");
            return false;
        }
        totalBytes += range.bytes;
    }
    if (totalBytes > config_.uploadRingBytes) {
        set_runtime_error(error, "dirty upload batch exceeds D3D12 upload ring capacity");
        return false;
    }

    // One allocator/list is deliberately serialized in this first executable backend. The ring
    // remains persistently mapped and fence-owned; later versions can shard allocators by frame.
    if (!wait_for_fence(lastSubmittedFence_, error)) return false;
    if (uploadHead_ + totalBytes > config_.uploadRingBytes) {
        uploadHead_ = 0U;
        ++counters_.ringWrapWaits;
    }
    HRESULT hr = copyAllocator_->Reset();
    if (FAILED(hr)) {
        set_runtime_error(error, "copy allocator Reset failed", hr);
        record_device_removed();
        return false;
    }
    hr = copyList_->Reset(copyAllocator_.Get(), nullptr);
    if (FAILED(hr)) {
        set_runtime_error(error, "copy list Reset failed", hr);
        record_device_removed();
        return false;
    }

    std::size_t sourceOffset = uploadHead_;
    for (const PersistentRuntimeBrickmapUploadRange& range : ranges) {
        sourceOffset = align_up_runtime(sourceOffset, 16U);
        const auto bytes = persistent_.heap_bytes(range.heap, range.destinationOffset, range.bytes);
        if (bytes.size() != range.bytes) {
            set_runtime_error(error, "persistent upload range is outside CPU heap storage");
            return false;
        }
        std::memcpy(mappedUpload_ + sourceOffset, bytes.data(), bytes.size());
        copyList_->CopyBufferRegion(
            resource_for(range.heap), static_cast<UINT64>(range.destinationOffset),
            uploadRing_.Get(), static_cast<UINT64>(sourceOffset),
            static_cast<UINT64>(range.bytes));
        sourceOffset += range.bytes;
        counters_.uploadedBytes += range.bytes;
    }
    std::uint64_t fenceValue{};
    if (!execute_and_signal(fenceValue, error)) return false;
    uploadHead_ += totalBytes;
    if (objectFences_.size() <= handle) objectFences_.resize(static_cast<std::size_t>(handle) + 1U);
    objectFences_[handle] = fenceValue;
    (void)persistent_.clear_pending_upload_ranges(handle);
    return true;
}

RuntimeBrickmapHandle D3D12RuntimeBrickmapWorld::create_object(
    const RuntimeBrickmapCreateDesc& desc) {
    if (device_ == nullptr) return kInvalidRuntimeBrickmapHandle;
    const RuntimeBrickmapHandle handle = persistent_.create_object(desc);
    if (handle == kInvalidRuntimeBrickmapHandle) return handle;
    std::string error;
    if (!upload_pending(handle, &error)) {
        (void)persistent_.destroy_object(handle);
        return kInvalidRuntimeBrickmapHandle;
    }
    return handle;
}

bool D3D12RuntimeBrickmapWorld::destroy_object(RuntimeBrickmapHandle handle) {
    return persistent_.destroy_object(handle);
}

bool D3D12RuntimeBrickmapWorld::update_object(
    RuntimeBrickmapHandle handle,
    const RuntimeBrickmapUpdateDesc& desc) {
    if (device_ == nullptr) return false;
    PersistentRuntimeBrickmapWorld previous = persistent_;
    if (!persistent_.update_object(handle, desc)) return false;
    std::string error;
    if (!upload_pending(handle, &error)) {
        persistent_ = std::move(previous);
        return false;
    }
    return true;
}

std::optional<std::uint64_t> D3D12RuntimeBrickmapWorld::readback_hash(
    RuntimeBrickmapHandle handle) const {
    return const_cast<D3D12RuntimeBrickmapWorld*>(this)->readback_hash_impl(handle, nullptr);
}

std::optional<std::uint64_t> D3D12RuntimeBrickmapWorld::readback_hash_impl(
    RuntimeBrickmapHandle handle,
    std::string* error) {
    ++counters_.readbackCalls;
    const auto view = persistent_.object_view(handle);
    if (!view) return std::nullopt;
    const std::uint64_t uploadFence = handle < objectFences_.size() ? objectFences_[handle] : 0U;
    if (!wait_for_fence(uploadFence, error)) {
        ++counters_.readbackFailures;
        return std::nullopt;
    }
    const std::size_t indexOffset = 0U;
    const std::size_t recordOffset = align_up_runtime(view->indexBytes, 16U);
    const std::size_t materialOffset = align_up_runtime(recordOffset + view->recordBytes, 16U);
    const std::size_t totalBytes = materialOffset + view->materialBytes;
    if (totalBytes > config_.readbackBytes) {
        set_runtime_error(error, "one object readback exceeds D3D12 readback buffer capacity");
        ++counters_.readbackFailures;
        return std::nullopt;
    }

    HRESULT hr = copyAllocator_->Reset();
    if (FAILED(hr)) {
        set_runtime_error(error, "readback allocator Reset failed", hr);
        ++counters_.readbackFailures;
        record_device_removed();
        return std::nullopt;
    }
    hr = copyList_->Reset(copyAllocator_.Get(), nullptr);
    if (FAILED(hr)) {
        set_runtime_error(error, "readback list Reset failed", hr);
        ++counters_.readbackFailures;
        record_device_removed();
        return std::nullopt;
    }
    if (view->indexBytes != 0U) copyList_->CopyBufferRegion(
        readbackBuffer_.Get(), indexOffset, indexBuffer_.Get(),
        view->indexAllocation.offset, view->indexBytes);
    if (view->recordBytes != 0U) copyList_->CopyBufferRegion(
        readbackBuffer_.Get(), recordOffset, recordBuffer_.Get(),
        view->recordAllocation.offset, view->recordBytes);
    if (view->materialBytes != 0U) copyList_->CopyBufferRegion(
        readbackBuffer_.Get(), materialOffset, materialBuffer_.Get(),
        view->materialAllocation.offset, view->materialBytes);
    std::uint64_t readbackFence{};
    if (!execute_and_signal(readbackFence, error) || !wait_for_fence(readbackFence, error)) {
        ++counters_.readbackFailures;
        return std::nullopt;
    }

    D3D12_RANGE readRange{0U, totalBytes};
    void* mappedPointer = nullptr;
    hr = readbackBuffer_->Map(0U, &readRange, &mappedPointer);
    if (FAILED(hr)) {
        set_runtime_error(error, "readback Map failed", hr);
        ++counters_.readbackFailures;
        record_device_removed();
        return std::nullopt;
    }
    const auto* mapped = static_cast<const std::byte*>(mappedPointer);
    const auto indexBytes = persistent_.heap_bytes(
        PersistentBrickmapHeapKind::Index, view->indexAllocation.offset, view->indexBytes);
    const auto recordBytes = persistent_.heap_bytes(
        PersistentBrickmapHeapKind::Record, view->recordAllocation.offset, view->recordBytes);
    const auto materialBytes = persistent_.heap_bytes(
        PersistentBrickmapHeapKind::Material, view->materialAllocation.offset, view->materialBytes);
    const bool matches =
        (indexBytes.empty() || std::memcmp(mapped + indexOffset, indexBytes.data(), indexBytes.size()) == 0) &&
        (recordBytes.empty() || std::memcmp(mapped + recordOffset, recordBytes.data(), recordBytes.size()) == 0) &&
        (materialBytes.empty() || std::memcmp(mapped + materialOffset, materialBytes.data(), materialBytes.size()) == 0);
    D3D12_RANGE noWrite{0U, 0U};
    readbackBuffer_->Unmap(0U, &noWrite);
    if (!matches) {
        set_runtime_error(error, "D3D12 persistent heap readback differs from CPU publication model");
        ++counters_.readbackFailures;
        return std::nullopt;
    }
    return view->expectedReadbackHash;
}

void D3D12RuntimeBrickmapWorld::wait_idle() noexcept {
    if (copyQueue_ == nullptr || fence_ == nullptr || fenceEvent_ == nullptr) return;
    std::uint64_t fenceValue = nextFenceValue_++;
    if (FAILED(copyQueue_->Signal(fence_.Get(), fenceValue))) {
        record_device_removed();
        return;
    }
    lastSubmittedFence_ = fenceValue;
    (void)wait_for_fence(fenceValue, nullptr);
}

void D3D12RuntimeBrickmapWorld::record_device_removed() noexcept {
    if (device_ != nullptr) counters_.deviceRemovedReason = device_->GetDeviceRemovedReason();
}

D3D12RuntimeBrickmapWorldStats D3D12RuntimeBrickmapWorld::stats() const noexcept {
    D3D12RuntimeBrickmapWorldStats result = counters_;
    result.persistent = persistent_.stats();
    result.uploadRingBytes = config_.uploadRingBytes;
    result.uploadRingHead = uploadHead_;
    if (device_ != nullptr) result.deviceRemovedReason = device_->GetDeviceRemovedReason();
    return result;
}

} // namespace dve::gpu

#endif // _WIN32
