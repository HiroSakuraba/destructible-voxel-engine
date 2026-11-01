#!/usr/bin/env bash
# Local checks that mirror the fastest-to-fail CI gates, so a failure is found
# in seconds on this machine instead of after a full CI round-trip.
#
# Currently: shader contract validation (scripts/validate_shaders.sh) — the
# check whose failure reached CI on PRs #269, #270 and #273 because
# shaders/shader_manifest.json had drifted from the shaders. With
# glslangValidator installed the script also compiles every manifest shader;
# without it the compile half is skipped and only the contracts are checked.
#
# Enable as a git hook once per clone:
#   git config core.hooksPath .githooks
# Bypass in an emergency with: git push --no-verify
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
bash scripts/validate_shaders.sh
