#include "dve/content_source.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace dve {
namespace {

void set_error(ContentError* error, ContentErrorCode code, std::string message, std::string_view path) {
    if (error == nullptr) return;
    error->code = code;
    error->message = std::move(message);
    error->path = std::string(path);
}

void set_text(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

} // namespace

const char* to_string(ContentErrorCode code) noexcept {
    switch (code) {
    case ContentErrorCode::NoError: return "no error";
    case ContentErrorCode::InvalidPath: return "invalid content path";
    case ContentErrorCode::NotFound: return "content not found";
    case ContentErrorCode::LimitExceeded: return "content exceeds size limit";
    case ContentErrorCode::Io: return "content I/O error";
    case ContentErrorCode::IntegrityFailure: return "content integrity check failed";
    }
    return "unknown content error";
}

std::optional<std::string> normalize_content_path(std::string_view path, std::string* error) {
    if (path.empty()) return set_text(error, "content path is empty"), std::nullopt;
    if (path.size() > kMaximumContentPathBytes) return set_text(error, "content path exceeds length limit"), std::nullopt;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || byte == 0x7FU) return set_text(error, "content path contains a control character"), std::nullopt;
        if (c == '\\') return set_text(error, "content path must use forward slashes"), std::nullopt;
    }
    if (path.front() == '/') return set_text(error, "content path must be relative"), std::nullopt;
    if (path.size() >= 2U && path[1] == ':') return set_text(error, "content path must not carry a drive letter"), std::nullopt;
    std::string normalized;
    normalized.reserve(path.size());
    std::size_t start = 0U;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) slash = path.size();
        const std::string_view component = path.substr(start, slash - start);
        if (component == "..") return set_text(error, "content path must not contain '..'"), std::nullopt;
        if (!component.empty() && component != ".") {
            if (!normalized.empty()) normalized.push_back('/');
            normalized.append(component);
        }
        start = slash + 1U;
    }
    if (normalized.empty()) return set_text(error, "content path names no file"), std::nullopt;
    return normalized;
}

std::optional<std::string> resolve_content_sibling(std::string_view file, std::string_view relative, std::string* error) {
    const auto base = normalize_content_path(file, error);
    if (!base) return std::nullopt;
    const auto child = normalize_content_path(relative, error);
    if (!child) return std::nullopt;
    const std::size_t slash = base->rfind('/');
    if (slash == std::string::npos) return child;
    return base->substr(0U, slash + 1U) + *child;
}

std::optional<std::string> ContentSource::read_text(
    std::string_view path, ContentError* error, std::uint64_t maximumBytes) const {
    auto bytes = read(path, error, maximumBytes);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// -----------------------------------------------------------------------------
// Loose folder

std::unique_ptr<LooseContentSource> LooseContentSource::open(const std::filesystem::path& root, std::string* error) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec) || ec) {
        set_text(error, "content root is not a directory: " + root.string());
        return nullptr;
    }
    return std::unique_ptr<LooseContentSource>(new LooseContentSource(canonical));
}

std::optional<std::filesystem::path> LooseContentSource::resolve(std::string_view path, ContentError* error) const {
    std::string message;
    const auto normalized = normalize_content_path(path, &message);
    if (!normalized) {
        set_error(error, ContentErrorCode::InvalidPath, message, path);
        return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(root_ / *normalized, ec);
    if (ec || !path_is_within(root_, resolved)) {
        set_error(error, ContentErrorCode::InvalidPath, "content path resolves outside the content root", *normalized);
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
        set_error(error, ContentErrorCode::NotFound, "content file not found", *normalized);
        return std::nullopt;
    }
    return resolved;
}

bool LooseContentSource::exists(std::string_view path) const { return resolve(path, nullptr).has_value(); }

std::optional<std::uint64_t> LooseContentSource::size(std::string_view path) const {
    const auto resolved = resolve(path, nullptr);
    if (!resolved) return std::nullopt;
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(*resolved, ec);
    if (ec) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

std::optional<std::vector<std::byte>> LooseContentSource::read(
    std::string_view path, ContentError* error, std::uint64_t maximumBytes) const {
    const auto resolved = resolve(path, error);
    if (!resolved) return std::nullopt;
    std::ifstream input(*resolved, std::ios::binary | std::ios::ate);
    if (!input) return set_error(error, ContentErrorCode::Io, "unable to open content file", path), std::nullopt;
    const std::streamoff end = input.tellg();
    if (end < 0) return set_error(error, ContentErrorCode::Io, "unable to determine content size", path), std::nullopt;
    if (static_cast<std::uint64_t>(end) > maximumBytes) {
        return set_error(error, ContentErrorCode::LimitExceeded, "content file exceeds size limit", path), std::nullopt;
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return set_error(error, ContentErrorCode::Io, "unable to read content file", path), std::nullopt;
    }
    return bytes;
}

std::vector<std::string> LooseContentSource::list(std::string_view prefix) const {
    std::vector<std::string> result;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root_, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || entryError) continue;
        const std::string relative = it->path().lexically_relative(root_).generic_string();
        if (relative.compare(0U, prefix.size(), prefix) != 0) continue;
        if (!normalize_content_path(relative)) continue; // names we could never address
        result.push_back(relative);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string LooseContentSource::describe() const { return "loose:" + root_.generic_string(); }

// -----------------------------------------------------------------------------
// Mounted pak

std::unique_ptr<PakContentSource> PakContentSource::open(const std::filesystem::path& package, std::string* error) {
    DvePakMount mount;
    std::string message;
    if (!mount.mount(package, &message)) {
        set_text(error, "could not mount " + package.string() + ": " + message);
        return nullptr;
    }
    return std::make_unique<PakContentSource>(std::move(mount));
}

bool PakContentSource::exists(std::string_view path) const {
    const auto normalized = normalize_content_path(path);
    return normalized && mount_.contains(*normalized);
}

std::optional<std::uint64_t> PakContentSource::size(std::string_view path) const {
    const auto normalized = normalize_content_path(path);
    if (!normalized) return std::nullopt;
    const DvePakEntry* entry = mount_.find(*normalized);
    if (entry == nullptr) return std::nullopt;
    return entry->size;
}

std::optional<std::vector<std::byte>> PakContentSource::read(
    std::string_view path, ContentError* error, std::uint64_t maximumBytes) const {
    std::string message;
    const auto normalized = normalize_content_path(path, &message);
    if (!normalized) return set_error(error, ContentErrorCode::InvalidPath, message, path), std::nullopt;
    const DvePakEntry* entry = mount_.find(*normalized);
    if (entry == nullptr) {
        return set_error(error, ContentErrorCode::NotFound, "package entry not found", *normalized), std::nullopt;
    }
    if (entry->size > maximumBytes) {
        return set_error(error, ContentErrorCode::LimitExceeded, "package entry exceeds size limit", *normalized), std::nullopt;
    }
    auto bytes = mount_.read(*normalized, &message);
    if (!bytes) {
        const bool integrity = message.find("integrity") != std::string::npos;
        set_error(error, integrity ? ContentErrorCode::IntegrityFailure : ContentErrorCode::Io,
                  message, *normalized);
        return std::nullopt;
    }
    return bytes;
}

std::vector<std::string> PakContentSource::list(std::string_view prefix) const {
    std::vector<std::string> result;
    for (const DvePakEntry& entry : mount_.manifest().entries) {
        if (entry.path.compare(0U, prefix.size(), prefix) == 0) result.push_back(entry.path);
    }
    return result; // manifest entries are already sorted (inspect_dvepak enforces it)
}

std::string PakContentSource::describe() const { return "pak:" + mount_.package_path().generic_string(); }

std::unique_ptr<ContentSource> open_content_source(const std::filesystem::path& location, std::string* error) {
    std::error_code ec;
    if (std::filesystem::is_directory(location, ec)) return LooseContentSource::open(location, error);
    if (std::filesystem::is_regular_file(location, ec)) return PakContentSource::open(location, error);
    set_text(error, "content location does not exist: " + location.string());
    return nullptr;
}

} // namespace dve
