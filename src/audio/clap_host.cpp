#include "dve/audio/clap_host.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_set>

namespace dve::audio {
namespace {
constexpr std::array<char, 8> kMagic{'D','V','E','C','L','A','P','1'};
constexpr std::uint32_t kVersion = 1U;
constexpr std::size_t kMaximumPlugins = 1U << 20U;
constexpr std::size_t kMaximumString = 1U << 20U;

std::uint64_t hash_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0U;
    std::uint64_t hash = 1469598103934665603ULL;
    std::array<char, 65536> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(i)]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

template<class T> bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}
template<class T> bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}
bool write_string(std::ostream& stream, const std::string& value) {
    if (value.size() > kMaximumString) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return write_value(stream, size) &&
        (size == 0U || static_cast<bool>(stream.write(value.data(), static_cast<std::streamsize>(size))));
}
bool read_string(std::istream& stream, std::string& value) {
    std::uint32_t size{};
    if (!read_value(stream, size) || size > kMaximumString) return false;
    value.resize(size);
    if (size != 0U) stream.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}
}

void ClapPluginRegistry::add_search_path(std::filesystem::path path) {
    if (path.empty()) return;
    path = path.lexically_normal();
    if (std::find(searchPaths_.begin(), searchPaths_.end(), path) == searchPaths_.end())
        searchPaths_.push_back(std::move(path));
}

void ClapPluginRegistry::blacklist_hash(std::uint64_t hash) {
    if (hash != 0U && std::find(blacklist_.begin(), blacklist_.end(), hash) == blacklist_.end())
        blacklist_.push_back(hash);
}
void ClapPluginRegistry::clear_blacklist() noexcept { blacklist_.clear(); }

std::size_t ClapPluginRegistry::scan(std::string* error) {
    plugins_.clear();
    std::unordered_set<std::string> seen;
    std::error_code ec;
    for (const auto& root : searchPaths_) {
        if (!std::filesystem::exists(root, ec)) continue;
        const auto consider = [&](const std::filesystem::path& path) {
            if (plugins_.size() >= kMaximumPlugins || path.extension() != ".clap") return;
            const std::string canonical = path.lexically_normal().generic_string();
            if (!seen.insert(canonical).second) return;
            ClapPluginDescriptor descriptor;
            descriptor.path = path;
            descriptor.fileHash = hash_file(path);
            descriptor.fileBytes = std::filesystem::is_regular_file(path, ec)
                ? static_cast<std::uint64_t>(std::filesystem::file_size(path, ec)) : 0U;
            descriptor.identifier = path.stem().string();
            descriptor.displayName = descriptor.identifier;
            if (descriptor.fileHash == 0U || descriptor.fileBytes == 0U) {
                descriptor.status = ClapScanStatus::Invalid;
                descriptor.diagnostic = "bundle is unreadable or empty";
            } else if (std::find(blacklist_.begin(), blacklist_.end(), descriptor.fileHash) != blacklist_.end()) {
                descriptor.status = ClapScanStatus::Blacklisted;
                descriptor.diagnostic = "binary hash is blacklisted";
            } else {
                descriptor.status = ClapScanStatus::Available;
                descriptor.diagnostic = "discovered; metadata and DSP loading require scanner helper";
            }
            plugins_.push_back(std::move(descriptor));
        };
        if (std::filesystem::is_regular_file(root, ec)) consider(root);
        else {
            for (std::filesystem::recursive_directory_iterator it(root,
                    std::filesystem::directory_options::skip_permission_denied, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (it.depth() > 8) it.disable_recursion_pending();
                if (it->is_regular_file(ec) || (it->is_directory(ec) && it->path().extension() == ".clap"))
                    consider(it->path());
            }
        }
    }
    std::sort(plugins_.begin(), plugins_.end(), [](const auto& a, const auto& b) {
        return a.identifier < b.identifier;
    });
    if (plugins_.empty() && error != nullptr) *error = "no CLAP bundles found";
    return plugins_.size();
}

const ClapPluginDescriptor* ClapPluginRegistry::find(std::string_view identifier) const noexcept {
    const auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [identifier](const ClapPluginDescriptor& plugin) { return plugin.identifier == identifier; });
    return it == plugins_.end() ? nullptr : &*it;
}

bool ClapPluginRegistry::write_cache(const std::filesystem::path& path, std::string* error) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "unable to create CLAP cache"; return false; }
    stream.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    const auto pathCount = static_cast<std::uint32_t>(searchPaths_.size());
    const auto blacklistCount = static_cast<std::uint32_t>(blacklist_.size());
    const auto pluginCount = static_cast<std::uint32_t>(plugins_.size());
    if (!write_value(stream, kVersion) || !write_value(stream, pathCount) ||
        !write_value(stream, blacklistCount) || !write_value(stream, pluginCount)) return false;
    for (const auto& item : searchPaths_) if (!write_string(stream, item.generic_string())) return false;
    for (const auto item : blacklist_) if (!write_value(stream, item)) return false;
    for (const auto& plugin : plugins_) {
        const auto status = static_cast<std::uint8_t>(plugin.status);
        if (!write_string(stream, plugin.path.generic_string()) || !write_string(stream, plugin.identifier) ||
            !write_string(stream, plugin.displayName) || !write_value(stream, plugin.fileHash) ||
            !write_value(stream, plugin.fileBytes) || !write_value(stream, status) ||
            !write_string(stream, plugin.diagnostic)) return false;
    }
    return static_cast<bool>(stream);
}

bool ClapPluginRegistry::read_cache(const std::filesystem::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 8> magic{};
    std::uint32_t version{}, pathCount{}, blacklistCount{}, pluginCount{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kMagic || !read_value(stream, version) || version != kVersion ||
        !read_value(stream, pathCount) || !read_value(stream, blacklistCount) ||
        !read_value(stream, pluginCount) || pluginCount > kMaximumPlugins) {
        if (error) *error = "invalid CLAP cache";
        return false;
    }
    searchPaths_.clear(); blacklist_.clear(); plugins_.clear();
    for (std::uint32_t i = 0U; i < pathCount; ++i) {
        std::string value; if (!read_string(stream, value)) return false; searchPaths_.emplace_back(value);
    }
    blacklist_.resize(blacklistCount);
    for (auto& value : blacklist_) if (!read_value(stream, value)) return false;
    plugins_.resize(pluginCount);
    for (auto& plugin : plugins_) {
        std::string pathText; std::uint8_t status{};
        if (!read_string(stream, pathText) || !read_string(stream, plugin.identifier) ||
            !read_string(stream, plugin.displayName) || !read_value(stream, plugin.fileHash) ||
            !read_value(stream, plugin.fileBytes) || !read_value(stream, status) ||
            !read_string(stream, plugin.diagnostic)) return false;
        plugin.path = pathText;
        plugin.status = static_cast<ClapScanStatus>(status);
    }
    if (stream.peek() != std::char_traits<char>::eof()) { if (error) *error = "trailing CLAP cache data"; return false; }
    return true;
}

} // namespace dve::audio
