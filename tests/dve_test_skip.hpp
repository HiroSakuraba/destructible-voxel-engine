#pragma once

// Honest skips for the test executables.
//
// A test that cannot run some of its checks (no Vulkan device, Jolt or a shader compiler not
// built in) calls note_skip() with the reason and returns exit_code(): 1 when anything failed,
// otherwise 77 when anything was skipped, otherwise 0. Every CTest test has SKIP_RETURN_CODE 77
// (cmake/DveTestCoverage.cmake), so CTest reports such a run as "Skipped", never "Passed".
//
// DVE_REQUIRE_COMPLETE_TESTS=1 in the environment turns every skip into a failure: a CI job that
// is meant to run everything cannot pass with parts missing. Which jobs set it (and which groups a
// job must build at configure time, DVE_REQUIRE_TEST_GROUPS) is in docs/honest-test-coverage-2026-10-27.md.

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace dve::testing {

inline int& skipped_parts() noexcept {
    static int count = 0;
    return count;
}

[[nodiscard]] inline bool complete_tests_required() noexcept {
    // Unset, empty, "0", "false", "off" and "no" mean "not required"; anything else requires it.
    const char* value = std::getenv("DVE_REQUIRE_COMPLETE_TESTS");
    if (value == nullptr) return false;
    const std::string_view text(value);
    for (const std::string_view off : {std::string_view{}, std::string_view{"0"}, std::string_view{"false"},
                                       std::string_view{"FALSE"}, std::string_view{"off"},
                                       std::string_view{"OFF"}, std::string_view{"no"}, std::string_view{"NO"}}) {
        if (text == off) return false;
    }
    return true;
}

// Records a part of the test that did not run, with the reason (printed as "SKIPPED: ...").
inline void note_skip(std::string_view reason) noexcept {
    std::printf("SKIPPED: %.*s%s\n", static_cast<int>(reason.size()), reason.data(),
                complete_tests_required() ? " (DVE_REQUIRE_COMPLETE_TESTS is set: this is a failure)" : "");
    std::fflush(stdout);
    ++skipped_parts();
}

[[nodiscard]] inline int exit_code(bool failed) noexcept {
    if (failed) return 1;
    if (skipped_parts() > 0) return complete_tests_required() ? 1 : 77;
    return 0;
}

// For a test that cannot run at all: note the reason and return the exit code.
[[nodiscard]] inline int skip(std::string_view reason) noexcept {
    note_skip(reason);
    return exit_code(false);
}

} // namespace dve::testing
