# Live-editor MCP host: closing the listening socket during shutdown

Found with ThreadSanitizer (GCC 13.3, `-fsanitize=thread`), which reported a data race in
`dve_live_editor_mcp_core_tests` and `dve_live_editor_mcp_tests` on every run.

## The bug

`LiveEditorMcpHost::stop()` closed the listening socket and set `listenSocket = -1` while
the accept thread could still be polling or accepting on it, and only then joined that
thread. Besides the unsynchronized read and write of `listenSocket`, the closed descriptor
number could be handed to any other `open()` or `socket()` in the editor before the accept
thread noticed the stop, so its next `poll()`/`accept()` would act on an unrelated file.

## The fix

`stop()` now calls `shutdown()` on the listening socket to wake a blocked `poll()`/`accept()`,
joins the accept thread, and closes the descriptor after that. The accept thread only reads
`listenSocket`, which is set before the thread starts and not changed until it has exited.
The 100 ms poll timeout still bounds the wait if `shutdown()` does not wake it.

## Validation

- Under ThreadSanitizer, both MCP tests report the race on every run before the change and
  none after it (5 runs of the core test, 3 of the editor test).
- `dve_live_editor_mcp_core_tests` adds 20 start/stop cycles of a fresh host, each stop
  required to finish within a second and remove its descriptor. The whole test runs in
  about 0.2 s under ThreadSanitizer.
