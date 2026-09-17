#!/usr/bin/env sh
set -eu
if [ "$#" -lt 2 ]; then
  echo "usage: $0 /path/to/dve_mcp_server /path/to/project [port]" >&2
  exit 2
fi
SERVER=$1
PROJECT=$2
PORT=${3:-8765}
: "${DVE_MCP_BEARER_TOKEN:?Set DVE_MCP_BEARER_TOKEN in the environment}"
exec python3 "$(dirname "$0")/../tools/dve_mcp_streamable_http.py" \
  --server "$SERVER" \
  --project-root "$PROJECT" \
  --host 127.0.0.1 \
  --port "$PORT"
