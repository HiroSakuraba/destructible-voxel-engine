#pragma once
// SPIR-V for the Vulkan player renderer, compiled by DXC at build time and embedded by
// cmake/DveEmbedSpirv.cmake (dve_player_shader_blobs.cpp). Without DXC the build links
// player_shader_blobs_missing.cpp instead: available() is false and every span is empty.
#include <cstddef>
#include <span>

namespace dve::player::shader_blobs {
[[nodiscard]] bool available() noexcept;
[[nodiscard]] std::span<const std::byte> live_ibl_material_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_ibl_material_ps() noexcept;
[[nodiscard]] std::span<const std::byte> live_environment_skybox_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_environment_skybox_ps() noexcept;
[[nodiscard]] std::span<const std::byte> live_csm_caster_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_csm_caster_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_present_vs() noexcept;
[[nodiscard]] std::span<const std::byte> player_present_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_ssao_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_traced_ao_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_traced_gi_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_rc_cascade_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_rc_gather_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_gi_temporal_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_gi_upsample_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_surface_cs() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_trace_cs() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_upsample_cs() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_clear_cs() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_prepare_cs() noexcept;
[[nodiscard]] std::span<const std::byte> player_split_rc_weighted_merge_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_probe_assign_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_intervals_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_compact_offsets_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_build_queries_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_accumulate_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_resolve_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_spatial_merge_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_scatter_cs() noexcept;
[[nodiscard]] std::span<const std::byte> split_rc_diffuse_gather_cs() noexcept;
} // namespace dve::player::shader_blobs
