#!/usr/bin/env python3
"""Bounded loopback MCP Streamable-HTTP adapter for the DVE stdio server.

The adapter intentionally exposes one serialized MCP session. It obtains bearer credentials from a
protected environment variable or file rather than command-line arguments, limits request rate and
concurrency, bounds child-response wait time, and strips known credentials from the MCP child's
environment. Put non-loopback deployments behind an authenticated TLS boundary.
"""
from __future__ import annotations

import argparse
from collections import defaultdict, deque
import json
import os
from pathlib import Path
import queue
import secrets
import stat
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


_SECRET_ENVIRONMENT_NAMES = {
    "OPENAI_API_KEY",
    "DVE_MCP_BEARER_TOKEN",
    "MCP_BEARER_TOKEN",
    "OAUTH_CLIENT_SECRET",
    "OPENAI_MCP_TOKEN",
}
_LOOPBACK_HOSTS = {"127.0.0.1", "::1", "localhost"}


class Bridge:
    def __init__(self, command: list[str], response_timeout: float):
        environment = os.environ.copy()
        for name in _SECRET_ENVIRONMENT_NAMES:
            environment.pop(name, None)
        self.child = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            env=environment,
        )
        self.response_timeout = max(1.0, response_timeout)
        self.lock = threading.RLock()
        self.responses: queue.Queue[str | BaseException | None] = queue.Queue()
        self.reader = threading.Thread(target=self._read_responses, name="dve-mcp-reader", daemon=True)
        self.reader.start()

    def _read_responses(self) -> None:
        try:
            assert self.child.stdout
            for line in self.child.stdout:
                self.responses.put(line.rstrip("\n"))
        except BaseException as exc:  # surfaced to the serialized request path
            self.responses.put(exc)
        finally:
            self.responses.put(None)

    def _write(self, payload: str) -> None:
        if self.child.poll() is not None:
            raise RuntimeError("DVE MCP child exited")
        assert self.child.stdin
        self.child.stdin.write(payload.replace("\n", "") + "\n")
        self.child.stdin.flush()

    def call(self, payload: str) -> str:
        with self.lock:
            self._write(payload)
            try:
                response = self.responses.get(timeout=self.response_timeout)
            except queue.Empty as exc:
                self.close()
                raise TimeoutError("DVE MCP child exceeded the response deadline") from exc
            if response is None:
                raise RuntimeError("DVE MCP child closed its response stream")
            if isinstance(response, BaseException):
                raise RuntimeError("DVE MCP response reader failed") from response
            return response

    def notify(self, payload: str) -> None:
        with self.lock:
            self._write(payload)

    def close(self) -> None:
        with self.lock:
            if self.child.poll() is None:
                self.child.terminate()
                try:
                    self.child.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self.child.kill()
                    self.child.wait(timeout=2.0)


class RateLimiter:
    def __init__(self, requests_per_minute: int):
        self.limit = max(1, requests_per_minute)
        self.lock = threading.Lock()
        self.history: dict[str, deque[float]] = defaultdict(deque)

    def allow(self, identity: str) -> bool:
        now = time.monotonic()
        cutoff = now - 60.0
        with self.lock:
            entries = self.history[identity]
            while entries and entries[0] < cutoff:
                entries.popleft()
            if len(entries) >= self.limit:
                return False
            entries.append(now)
            return True


class Handler(BaseHTTPRequestHandler):
    bridge: Bridge
    token: str
    session_id: str
    allowed_origin: str
    rate_limiter: RateLimiter
    request_slots: threading.BoundedSemaphore
    protocol_version = "HTTP/1.1"
    mcp_protocol_version = "2025-11-25"

    def _authorized(self) -> bool:
        supplied = self.headers.get("Authorization", "")
        return not self.token or secrets.compare_digest(supplied, f"Bearer {self.token}")

    def _origin_allowed(self) -> bool:
        origin = self.headers.get("Origin", "")
        return not origin or not self.allowed_origin or secrets.compare_digest(origin, self.allowed_origin)

    def _session_allowed(self, initialize: bool) -> bool:
        supplied = self.headers.get("Mcp-Session-Id", "")
        return initialize or not supplied or secrets.compare_digest(supplied, self.session_id)

    def _send_json(self, status_code: int, body: str = "", include_session: bool = True) -> None:
        encoded = body.encode("utf-8")
        self.send_response(status_code)
        if body:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        if include_session:
            self.send_header("Mcp-Session-Id", self.session_id)
        self.send_header("MCP-Protocol-Version", self.mcp_protocol_version)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(encoded)

    def do_POST(self) -> None:
        if self.path != "/mcp":
            self.send_error(404)
            return
        if not self.rate_limiter.allow(self.client_address[0]):
            self.send_error(429, "MCP request rate exceeded")
            return
        if not self.request_slots.acquire(blocking=False):
            self.send_error(503, "MCP request concurrency limit reached")
            return
        try:
            self._handle_post()
        finally:
            self.request_slots.release()

    def _handle_post(self) -> None:
        if not self._authorized():
            self.send_error(401)
            return
        if not self._origin_allowed():
            self.send_error(403)
            return
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            self.send_error(415)
            return
        accept = self.headers.get("Accept", "application/json")
        if "application/json" not in accept and "*/*" not in accept:
            self.send_error(406)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400)
            return
        if size <= 0 or size > 2_000_000:
            self.send_error(413)
            return
        try:
            body = self.rfile.read(size).decode("utf-8")
            request = json.loads(body)
        except Exception:
            self.send_error(400)
            return
        if not isinstance(request, dict):
            self.send_error(400, "DVE adapter accepts one JSON-RPC request at a time")
            return
        initialize = request.get("method") == "initialize"
        if not self._session_allowed(initialize):
            self.send_error(404, "unknown MCP session")
            return
        notification = "id" not in request
        try:
            if notification:
                self.bridge.notify(body)
                self._send_json(202)
                return
            response = self.bridge.call(body)
        except TimeoutError as exc:
            self.send_error(504, str(exc))
            return
        except Exception as exc:
            self.send_error(502, str(exc))
            return
        if not response:
            self.send_error(502, "DVE MCP request produced no response")
            return
        self._send_json(200, response)

    def do_GET(self) -> None:
        self.send_error(405, "DVE does not expose an MCP SSE event stream")

    def do_DELETE(self) -> None:
        if self.path != "/mcp" or not self._authorized():
            self.send_error(404)
            return
        if self.headers.get("Mcp-Session-Id", "") not in ("", self.session_id):
            self.send_error(404)
            return
        self.bridge.close()
        self._send_json(204, "", include_session=False)

    def log_message(self, fmt: str, *args: object) -> None:
        return


def _read_token(environment_name: str, token_file: str) -> str:
    if token_file:
        path = Path(token_file)
        if os.name == "posix":
            mode = stat.S_IMODE(path.stat().st_mode)
            if mode & (stat.S_IRWXG | stat.S_IRWXO):
                raise ValueError("token file must not be accessible by group or other users")
        return path.read_text(encoding="utf-8").strip()
    return os.environ.get(environment_name, "").strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--project-root", default=".")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token-env", default="DVE_MCP_BEARER_TOKEN")
    parser.add_argument("--token-file", default="")
    parser.add_argument("--allowed-origin", default="")
    parser.add_argument("--response-timeout", type=float, default=120.0)
    parser.add_argument("--requests-per-minute", type=int, default=60)
    parser.add_argument("--max-concurrent", type=int, default=4)
    parser.add_argument("--allow-non-loopback", action="store_true")
    parser.add_argument("--read-only", action="store_true")
    parser.add_argument("--allow-changes", action="store_true")
    args = parser.parse_args()
    if args.read_only and args.allow_changes:
        parser.error("choose at most one of --read-only and --allow-changes")
    try:
        token = _read_token(args.token_env, args.token_file)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    if args.host not in _LOOPBACK_HOSTS:
        if not args.allow_non_loopback:
            parser.error("non-loopback binding requires --allow-non-loopback")
        if not token or not args.allowed_origin:
            parser.error("non-loopback binding requires a bearer token and --allowed-origin")
    command = [args.server, "--project-root", args.project_root]
    if args.read_only:
        command.append("--read-only")
    elif args.allow_changes:
        command.append("--allow-changes")
    Handler.bridge = Bridge(command, args.response_timeout)
    Handler.token = token
    Handler.session_id = secrets.token_urlsafe(24)
    Handler.allowed_origin = args.allowed_origin
    Handler.rate_limiter = RateLimiter(args.requests_per_minute)
    Handler.request_slots = threading.BoundedSemaphore(max(1, args.max_concurrent))
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"DVE MCP listening on http://{args.host}:{args.port}/mcp", flush=True)
    try:
        server.serve_forever()
    finally:
        Handler.bridge.close()


if __name__ == "__main__":
    main()
