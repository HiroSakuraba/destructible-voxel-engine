#include <filesystem>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace dve::editor::detail {
// Keep replacement semantics across Windows/POSIX without deleting the previous
// record first. Native filesystem calls stay out of portable editor sources.
bool replace_workspace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}
} // namespace dve::editor::detail
