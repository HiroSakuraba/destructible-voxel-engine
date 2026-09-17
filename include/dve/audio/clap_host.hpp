#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dve::audio {

enum class ClapScanStatus : std::uint8_t { Available, Blacklisted, TimedOut, Invalid, Missing };

struct ClapPluginDescriptor {
    std::filesystem::path path;
    std::string identifier;
    std::string displayName;
    std::uint64_t fileHash{};
    std::uint64_t fileBytes{};
    ClapScanStatus status{ClapScanStatus::Invalid};
    std::string diagnostic;
};

struct ClapPluginState {
    std::uint32_t version{1U};
    std::string identifier;
    std::uint64_t pluginHash{};
    std::vector<std::byte> opaqueState;
};

// Control-thread CLAP registry foundation. v1.30 deliberately does not load untrusted plugin code
// into the editor process: it discovers bundles, hashes binaries, persists blacklist decisions, and
// produces missing-plugin placeholders. A later scanner helper process can populate richer metadata.
class ClapPluginRegistry {
public:
    void add_search_path(std::filesystem::path path);
    void blacklist_hash(std::uint64_t hash);
    void clear_blacklist() noexcept;
    [[nodiscard]] const std::vector<ClapPluginDescriptor>& plugins() const noexcept { return plugins_; }
    [[nodiscard]] const std::vector<std::filesystem::path>& search_paths() const noexcept { return searchPaths_; }
    std::size_t scan(std::string* error = nullptr);
    [[nodiscard]] const ClapPluginDescriptor* find(std::string_view identifier) const noexcept;
    [[nodiscard]] bool write_cache(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] bool read_cache(const std::filesystem::path& path, std::string* error = nullptr);
private:
    std::vector<std::filesystem::path> searchPaths_;
    std::vector<std::uint64_t> blacklist_;
    std::vector<ClapPluginDescriptor> plugins_;
};

} // namespace dve::audio
