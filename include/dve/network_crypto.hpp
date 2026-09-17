#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace dve {

using NetworkSha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] NetworkSha256Digest network_sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] NetworkSha256Digest network_hmac_sha256(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool network_constant_time_equal(
    std::span<const std::uint8_t> lhs, std::span<const std::uint8_t> rhs) noexcept;

} // namespace dve
