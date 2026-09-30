#pragma once

// Read-only game content access that is independent of where the bytes live.
//
// A shipped game reads everything from a mounted .dvepak; during development the same game
// runs from a loose project folder. Runtime loaders (scene loading into GameWorld, the
// game.dvegame manifest, and later scripts/audio in the player) take a ContentSource so both
// paths share one code path and one set of validation rules.
//
// Content paths are package-relative, use forward slashes, and never escape the root. They
// match the entry paths dve_pack writes (for example "scenes/main.dvoxscene.json").

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/v235_foundations.hpp"

namespace dve {

enum class ContentErrorCode : std::uint8_t {
    NoError,
    InvalidPath,     // empty, absolute, contains "..", backslashes, NUL, or similar
    NotFound,
    LimitExceeded,   // entry larger than the caller's maximumBytes
    Io,
    IntegrityFailure // pak entry failed its content hash check
};

struct ContentError {
    ContentErrorCode code{ContentErrorCode::NoError};
    std::string message;
    std::string path;

    [[nodiscard]] explicit operator bool() const noexcept { return code != ContentErrorCode::NoError; }
};

[[nodiscard]] const char* to_string(ContentErrorCode code) noexcept;

// Validates and canonicalizes a content path: collapses "." components and duplicate
// slashes, then rejects anything empty, absolute ("/x", "C:x"), containing "..", a
// backslash, a NUL or other control character, or longer than kMaximumContentPathBytes.
inline constexpr std::size_t kMaximumContentPathBytes = 4096U;
[[nodiscard]] std::optional<std::string> normalize_content_path(
    std::string_view path, std::string* error = nullptr);

// Joins `relative` onto the directory that contains the content path `file`, e.g.
// ("scenes/main.dvoxscene.json", "house.dvox") -> "scenes/house.dvox". `relative` must itself
// be a valid content path (so it can never climb out of the scene's folder with "..").
[[nodiscard]] std::optional<std::string> resolve_content_sibling(
    std::string_view file, std::string_view relative, std::string* error = nullptr);

class ContentSource {
public:
    virtual ~ContentSource() = default;

    // True if `path` names a readable file. Invalid paths are simply "not present".
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    // Size in bytes without reading the content, or nullopt if absent/invalid.
    [[nodiscard]] virtual std::optional<std::uint64_t> size(std::string_view path) const = 0;
    // Whole-entry read. Entries larger than `maximumBytes` fail with LimitExceeded before
    // any payload is read. Pak reads verify the per-entry content hash.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> read(
        std::string_view path, ContentError* error = nullptr,
        std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max()) const = 0;
    // Every file whose content path starts with `prefix` (plain string prefix; "" = all),
    // sorted by byte order, the same order dve_pack stores entries in.
    [[nodiscard]] virtual std::vector<std::string> list(std::string_view prefix = {}) const = 0;
    // Human-readable origin for logs and error messages, e.g. "pak:/games/demo.dvepak".
    [[nodiscard]] virtual std::string describe() const = 0;

    // Convenience: read() into a std::string (for text formats such as JSON or key=value).
    [[nodiscard]] std::optional<std::string> read_text(
        std::string_view path, ContentError* error = nullptr,
        std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max()) const;
};

// Loose project folder. Every lookup is confined to the canonical root: paths are validated
// with normalize_content_path and the resolved file (after symlinks) must still lie inside
// the root. list() skips non-regular files.
class LooseContentSource final : public ContentSource {
public:
    [[nodiscard]] static std::unique_ptr<LooseContentSource> open(
        const std::filesystem::path& root, std::string* error = nullptr);

    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<std::uint64_t> size(std::string_view path) const override;
    [[nodiscard]] std::optional<std::vector<std::byte>> read(
        std::string_view path, ContentError* error = nullptr,
        std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max()) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view prefix = {}) const override;
    [[nodiscard]] std::string describe() const override;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    explicit LooseContentSource(std::filesystem::path root) : root_(std::move(root)) {}
    [[nodiscard]] std::optional<std::filesystem::path> resolve(
        std::string_view path, ContentError* error) const;
    std::filesystem::path root_;
};

// Mounted .dvepak (read-only, whole-entry reads with FNV-1a integrity checks).
class PakContentSource final : public ContentSource {
public:
    [[nodiscard]] static std::unique_ptr<PakContentSource> open(
        const std::filesystem::path& package, std::string* error = nullptr);
    explicit PakContentSource(DvePakMount mount) : mount_(std::move(mount)) {}

    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<std::uint64_t> size(std::string_view path) const override;
    [[nodiscard]] std::optional<std::vector<std::byte>> read(
        std::string_view path, ContentError* error = nullptr,
        std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max()) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view prefix = {}) const override;
    [[nodiscard]] std::string describe() const override;
    [[nodiscard]] const DvePakMount& mount() const noexcept { return mount_; }

private:
    DvePakMount mount_;
};

// Opens a directory as a LooseContentSource and a regular file as a PakContentSource.
[[nodiscard]] std::unique_ptr<ContentSource> open_content_source(
    const std::filesystem::path& location, std::string* error = nullptr);

} // namespace dve
