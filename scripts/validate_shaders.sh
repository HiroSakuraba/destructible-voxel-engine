#!/usr/bin/env bash
# Validates the checked-in shader inventory and CPU/HLSL contracts on every machine, then uses
# glslangValidator when available for syntax/type/semantic compilation. Run from anywhere; paths
# are resolved relative to this script's location.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHADER_DIR="$SCRIPT_DIR/../shaders"
ROOT_DIR="$SCRIPT_DIR/.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 is required for shader contract validation"
    exit 1
fi
python3 "$ROOT_DIR/tools/validate_shader_contracts.py" --root "$ROOT_DIR"

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator not found; shader contracts passed, compiler validation skipped (install glslang-tools to enable it)"
    exit 0
fi

# Compile exactly the checked-in manifest inventory. The contract validator above already proves
# that this list matches every top-level .hlsl source, so compiler validation cannot silently omit
# a new shader.
failures=0
checked=0
while IFS=$'\t' read -r file stage entry; do
    case "$stage" in
        compute) glslang_stage="comp" ;;
        vertex)  glslang_stage="vert" ;;
        pixel)   glslang_stage="frag" ;;
        *)
            echo "FAIL: $file has unsupported stage '$stage'"
            failures=$((failures + 1))
            continue
            ;;
    esac
    checked=$((checked + 1))
    if ! output=$(glslangValidator -D -S "$glslang_stage" -e "$entry" --target-env vulkan1.2 "$file" 2>&1); then
        echo "FAIL: $file"
        echo "$output"
        failures=$((failures + 1))
        continue
    fi
    if echo "$output" | grep -qi "warning\|error"; then
        echo "FAIL: $file produced warnings/errors:"
        echo "$output"
        failures=$((failures + 1))
    fi
done < <(python3 - "$SHADER_DIR/shader_manifest.json" <<'MANIFEST_PY'
import json
import sys
from pathlib import Path
manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
for record in manifest["shaders"]:
    print(f"{record['source']}\t{record['stage']}\t{record['entry']}")
MANIFEST_PY
)

if [[ "$checked" -eq 0 ]]; then
    echo "FAIL: no shaders were checked"
    failures=$((failures + 1))
fi

if [[ "$failures" -eq 0 ]]; then
    echo "dve_shader_validation: PASS ($checked shaders)"
    exit 0
fi
echo "dve_shader_validation: $failures FAILURE(S)"
exit 1
