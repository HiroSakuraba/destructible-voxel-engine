# Full test suite under AddressSanitizer and UBSan in CI

`DVE_ENABLE_SANITIZERS` already instruments every DVE library and test with ASan and UBSan,
but CI only used it for the five-file contract test in `v235-foundations.yml`. The full
suite never ran sanitized.

## What changed

- New preset `linux-gcc-asan` (configure, build and test): `linux-gcc-release` with
  `DVE_ENABLE_SANITIZERS=ON` and `-g1`, so reports carry file and line numbers without full
  debug info.
- The `ctest` job in `ci.yml` runs it as a fourth matrix entry, `ctest (linux-gcc-asan)`,
  with the same build and CTest steps as the other presets. It gets 300 minutes instead of
  180. `UBSAN_OPTIONS=halt_on_error=1` makes any undefined-behaviour report fail its test;
  by default UBSan only prints and the test still passes. Leak checking is off
  (`ASAN_OPTIONS=detect_leaks=0`) until the suite has been checked for leaks.
- Sanitizer builds no longer register the notices, install-tree and packaging tests
  (`dve_third_party_notices_check_*`, `dve_install_tree_test`, `dve_package_consumer_test`,
  `dve_package_game_test`, `dve_cpack_test`). A sanitized binary needs the toolchain's
  `libasan`/`libubsan` and links a few extra X11 libraries (`libICE`, `libSM`), which those
  checks correctly report as bundled libraries with no notice. Sanitizer builds are never
  shipped, and the checks still run in every other preset.
- `dve_editor_synth_tests` overflowed the 8 MB stack under ASan: its `main` held the
  editor controller, the panel layout and six full synth preset copies, and ASan gives
  every local its own padded slot, so the frame came to about 9 MB. The controller, layout
  and preset copies now live on the heap and the scenario is split into one function per
  synth page (largest frame about 2.2 MB under ASan). The steps and checks are unchanged,
  except that the "audition with no candidates" step now checks the preset name stays the
  same; it used to copy the whole preset and never compare it.

## Validation

- A local ASan + UBSan build of the whole suite (GCC 13.3, Release, SDL3 unavailable)
  found no memory errors or undefined behaviour; the only failure was the synth test's
  stack overflow.
- `dve_editor_synth_tests` passes plain and under ASan + UBSan with the default 8 MB stack.
- First CI run (PR #55): every test passed under ASan and UBSan, including the SDL3 editor
  and player, except the six packaging tests above, which this change no longer registers in
  sanitizer builds.
