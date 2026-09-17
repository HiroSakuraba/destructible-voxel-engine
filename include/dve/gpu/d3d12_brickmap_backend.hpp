#pragma once

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "dve/packed_brickmap.hpp"
#include "dve/runtime_brickmap_world.hpp"

namespace dve::gpu {

struct D3D12FeatureReport {
    std::wstring adapterName{};
    std::uint64_t dedicatedVideoMemory{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_12_0};
    D3D_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    bool waveOps{};
    std::uint32_t waveLaneMinimum{};
    std::uint32_t waveLaneMaximum{};
};

// Windows validation backend for WP-5R.1. It owns one direct, compute, and copy
// queue, persistent DEFAULT-heap scene buffers, a persistently mapped UPLOAD
// heap, and a READBACK heap used by the oracle smoke test. No resource is
// created in upload_scene() unless capacity growth is explicitly required.
class D3D12BrickmapBackend {
public:
    D3D12BrickmapBackend() = default;
    ~D3D12BrickmapBackend();

    D3D12BrickmapBackend(const D3D12BrickmapBackend&) = delete;
    D3D12BrickmapBackend& operator=(const D3D12BrickmapBackend&) = delete;

    [[nodiscard]] bool initialize(std::string* error = nullptr);
    [[nodiscard]] bool reserve_scene_capacity(
        std::size_t indexBytes,
        std::size_t recordBytes,
        std::size_t materialBytes,
        std::string* error = nullptr);
    [[nodiscard]] bool upload_scene(
        const PackedBrickmapScene& scene,
        std::string* error = nullptr);
    [[nodiscard]] bool readback_matches(
        const PackedBrickmapScene& scene,
        std::string* error = nullptr);
    void wait_idle() noexcept;

    [[nodiscard]] const D3D12FeatureReport& feature_report() const noexcept { return featureReport_; }
    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* direct_queue() const noexcept { return directQueue_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* compute_queue() const noexcept { return computeQueue_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* copy_queue() const noexcept { return copyQueue_.Get(); }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<IDXGIFactory6> factory_{};
    ComPtr<IDXGIAdapter1> adapter_{};
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> directQueue_{};
    ComPtr<ID3D12CommandQueue> computeQueue_{};
    ComPtr<ID3D12CommandQueue> copyQueue_{};
    ComPtr<ID3D12CommandAllocator> copyAllocator_{};
    ComPtr<ID3D12GraphicsCommandList> copyList_{};
    ComPtr<ID3D12Fence> copyFence_{};
    void* fenceEvent_{};
    std::uint64_t nextFenceValue_{1};

    ComPtr<ID3D12Resource> indexBuffer_{};
    ComPtr<ID3D12Resource> recordBuffer_{};
    ComPtr<ID3D12Resource> materialBuffer_{};
    ComPtr<ID3D12Resource> uploadBuffer_{};
    ComPtr<ID3D12Resource> readbackBuffer_{};
    std::byte* mappedUpload_{};
    std::size_t indexCapacity_{};
    std::size_t recordCapacity_{};
    std::size_t materialCapacity_{};
    std::size_t uploadCapacity_{};
    std::size_t readbackCapacity_{};
    D3D12FeatureReport featureReport_{};

    [[nodiscard]] bool create_queue(D3D12_COMMAND_LIST_TYPE type, ComPtr<ID3D12CommandQueue>& output,
                                    std::string* error);
    [[nodiscard]] bool create_buffer(std::size_t bytes, D3D12_HEAP_TYPE heapType,
                                     D3D12_RESOURCE_STATES initialState,
                                     ComPtr<ID3D12Resource>& output, std::string* error);
    [[nodiscard]] bool execute_copy_and_wait(std::string* error);
    void release_mapped_upload() noexcept;
};


struct D3D12RuntimeBrickmapWorldConfig {
    PersistentRuntimeBrickmapWorldConfig heaps{};
    std::size_t uploadRingBytes{64ULL * 1024ULL * 1024ULL};
    std::size_t readbackBytes{64ULL * 1024ULL * 1024ULL};
    bool enableDebugLayer{true};
};

struct D3D12RuntimeBrickmapWorldStats {
    PersistentRuntimeBrickmapWorldStats persistent{};
    std::size_t uploadRingBytes{};
    std::size_t uploadRingHead{};
    std::uint64_t submittedCopyBatches{};
    std::uint64_t uploadedBytes{};
    std::uint64_t readbackCalls{};
    std::uint64_t readbackFailures{};
    std::uint64_t ringWrapWaits{};
    HRESULT deviceRemovedReason{S_OK};
};

// Windows implementation of the runtime publication capability over three shared persistent
// DEFAULT-heap buffers and one persistently mapped fence-owned upload ring. The cross-platform
// PersistentRuntimeBrickmapWorld supplies identical stable allocation and dirty-range rules.
class D3D12RuntimeBrickmapWorld final : public IRuntimeBrickmapWorld {
public:
    explicit D3D12RuntimeBrickmapWorld(
        const D3D12RuntimeBrickmapWorldConfig& config = {});
    ~D3D12RuntimeBrickmapWorld() override;

    D3D12RuntimeBrickmapWorld(const D3D12RuntimeBrickmapWorld&) = delete;
    D3D12RuntimeBrickmapWorld& operator=(const D3D12RuntimeBrickmapWorld&) = delete;

    [[nodiscard]] bool initialize(std::string* error = nullptr);
    [[nodiscard]] RuntimeBrickmapHandle create_object(
        const RuntimeBrickmapCreateDesc& desc) override;
    bool destroy_object(RuntimeBrickmapHandle handle) override;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash(
        RuntimeBrickmapHandle handle) const override;
    bool update_object(
        RuntimeBrickmapHandle handle,
        const RuntimeBrickmapUpdateDesc& desc) override;
    [[nodiscard]] bool supports_incremental_update() const noexcept override { return true; }

    void wait_idle() noexcept;
    [[nodiscard]] D3D12RuntimeBrickmapWorldStats stats() const noexcept;
    [[nodiscard]] const D3D12FeatureReport& feature_report() const noexcept {
        return featureReport_;
    }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    D3D12RuntimeBrickmapWorldConfig config_{};
    PersistentRuntimeBrickmapWorld persistent_{};
    D3D12FeatureReport featureReport_{};
    ComPtr<IDXGIFactory6> factory_{};
    ComPtr<IDXGIAdapter1> adapter_{};
    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> copyQueue_{};
    ComPtr<ID3D12CommandAllocator> copyAllocator_{};
    ComPtr<ID3D12GraphicsCommandList> copyList_{};
    ComPtr<ID3D12Fence> fence_{};
    ComPtr<ID3D12Resource> indexBuffer_{};
    ComPtr<ID3D12Resource> recordBuffer_{};
    ComPtr<ID3D12Resource> materialBuffer_{};
    ComPtr<ID3D12Resource> uploadRing_{};
    ComPtr<ID3D12Resource> readbackBuffer_{};
    std::byte* mappedUpload_{};
    void* fenceEvent_{};
    std::size_t uploadHead_{};
    std::uint64_t nextFenceValue_{1U};
    std::uint64_t lastSubmittedFence_{};
    std::vector<std::uint64_t> objectFences_{};
    mutable D3D12RuntimeBrickmapWorldStats counters_{};

    [[nodiscard]] bool create_shared_buffers(std::string* error);
    [[nodiscard]] bool upload_pending(RuntimeBrickmapHandle handle, std::string* error);
    [[nodiscard]] bool wait_for_fence(std::uint64_t value, std::string* error) const;
    [[nodiscard]] bool execute_and_signal(std::uint64_t& fenceValue, std::string* error);
    [[nodiscard]] ID3D12Resource* resource_for(PersistentBrickmapHeapKind heap) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash_impl(
        RuntimeBrickmapHandle handle,
        std::string* error);
    void record_device_removed() noexcept;
};

} // namespace dve::gpu

#endif // _WIN32
